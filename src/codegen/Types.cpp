/// @file codegen/Types.cpp
/// @brief Implementation of the type mapping layer.
///
/// See Types.hpp for the design rationale. This file is the mechanical
/// translation of `CodeGenType.cpp` to method-call syntax plus the state
/// move from `CodeGenContext` to `Types`.

#include "Types.hpp"
#include "LLVMTypeHelpers.hpp"
#include "core/ASTStrings.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <algorithm>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Types::Types(llvm::LLVMContext& llvmCtx_,
             llvm::Module& module_,
             StringPool& pool_)
    : llvmCtx(llvmCtx_)
    , module(module_)
    , pool(pool_)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Entry Point
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::get(TypeAST* type) {
    if (!type) return nullptr;

    // ─── Cache hit ────────────────────────────────────────────────────────
    auto it = typeCache.find(type);
    if (it != typeCache.end()) {
        return it->second;
    }

    llvm::Type* result = nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            result = primitiveType(type->as<PrimitiveTypeAST>());
            break;

        case ASTKind::SimdType:
            result = simdType(type->as<SimdTypeAST>());
            break;

        case ASTKind::ArenaType:
            result = arenaType();
            break;

        case ASTKind::ArenaDescriptorType:
            result = arenaDescriptorType();
            break;

        case ASTKind::NamedType:
            result = namedType(type->as<NamedTypeAST>());
            break;

        case ASTKind::ModuleTypeAccess:
            result = moduleTypeAccess(type->as<ModuleTypeAccessAST>());
            break;

        case ASTKind::PtrType:
            result = ptrType(type->as<PtrTypeAST>());
            break;

        case ASTKind::RefType:
            result = refType(type->as<RefTypeAST>());
            break;

        case ASTKind::ArrayType:
            result = arrayType(type->as<ArrayTypeAST>());
            break;

        case ASTKind::FuncType:
            result = functionType(type->as<FuncTypeAST>(), false);
            break;

        case ASTKind::NullableType:
            result = nullableType(type->as<NullableTypeAST>());
            break;

        case ASTKind::FallibleType:
            result = fallibleType(type->as<FallibleTypeAST>());
            break;

        case ASTKind::CombinedType:
            result = combinedType(type->as<CombinedTypeAST>());
            break;

        case ASTKind::FutureType:
            result = futureType(type->as<FutureTypeAST>());
            break;

        case ASTKind::ThreadType:
            result = threadType(type->as<ThreadTypeAST>());
            break;

        default:
            // No `ctx.diagnostics.errorAt` here — `Types` does not have a
            // diagnostics sink. Callers that hit this path have a bug in
            // their own dispatch; they should have validated the type
            // before calling `get`. Returning null is the correct answer:
            // the caller checks and emits a diagnostic if it can.
            return nullptr;
    }

    if (result) {
        typeCache[type] = result;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Built-in Type Accessors
// ─────────────────────────────────────────────────────────────────────────────

llvm::VectorType* Types::simdType(SimdTypeAST* simd) {
    if (!simd || simd->laneCount == 0) return nullptr;

    llvm::Type* elemType = get(simd->elementType);
    if (!elemType) return nullptr;

    return llvm::VectorType::get(elemType, simd->laneCount, false);
}

llvm::StructType* Types::arenaType() {
    // `lucid.Arena` is a compiler-managed opaque struct:
    //     { ptr base, i64 size, i64 cursor }
    // The user never sees the fields. Named so it appears as
    // `%lucid.Arena` in IR dumps and so its layout is asserted against
    // `lucid::abi::LucidArena` in the ABI header.
    llvm::StructType* type =
        llvm::StructType::getTypeByName(llvmCtx, "lucid.Arena");
    if (!type) {
        type = llvm::StructType::create(llvmCtx, "lucid.Arena");
        type->setBody({
            llvm::PointerType::get(llvmCtx, 0),  // base
            llvm::Type::getInt64Ty(llvmCtx),     // size
            llvm::Type::getInt64Ty(llvmCtx)      // cursor
        });
    }
    return type;
}

llvm::StructType* Types::arenaDescriptorType() {
    // `lucid.ArenaDescriptor` is the FFI-visible projection of `Arena`:
    //     { ptr base, i64 size }
    // Cursor is deliberately excluded — it's not part of the stable
    // boundary, so a future change to the bump strategy doesn't change
    // the FFI layout.
    llvm::StructType* type =
        llvm::StructType::getTypeByName(llvmCtx, "lucid.ArenaDescriptor");
    if (!type) {
        type = llvm::StructType::create(llvmCtx, "lucid.ArenaDescriptor");
        type->setBody({
            llvm::PointerType::get(llvmCtx, 0),  // base
            llvm::Type::getInt64Ty(llvmCtx)      // size
        });
    }
    return type;
}

llvm::StructType* Types::stringType() {
    // `lucid.String`:
    //     { ptr data, i64 len, i64 cap }
    // The layout is asserted against `lucid::abi::LucidString` in the ABI
    // header. `cap == 0` is the sentinel for a static string literal,
    // whose `data` pointer must not be freed.
    llvm::StructType* type =
        llvm::StructType::getTypeByName(llvmCtx, "lucid.String");
    if (!type) {
        type = llvm::StructType::create(llvmCtx, "lucid.String");
        type->setBody({
            llvm::PointerType::get(llvmCtx, 0),  // data
            llvm::Type::getInt64Ty(llvmCtx),     // len
            llvm::Type::getInt64Ty(llvmCtx)      // cap
        });
    }
    return type;
}

llvm::StructType* Types::sliceType() {
    // `lucid.Slice` is the generic slice shape, used where the element
    // type is not statically known. Typed slices (one per element type)
    // are produced by `arrayType` with `ArrayKind::Slice` and are not
    // this type — this is only for the type-erased slot in a variadic
    // call. It has the same LLVM shape but a distinct name so it doesn't
    // collide with any typed slice.
    llvm::StructType* type =
        llvm::StructType::getTypeByName(llvmCtx, "lucid.Slice");
    if (!type) {
        type = llvm::StructType::create(llvmCtx, "lucid.Slice");
        type->setBody({
            llvm::PointerType::get(llvmCtx, 0),  // data
            llvm::Type::getInt64Ty(llvmCtx),     // len
            llvm::Type::getInt64Ty(llvmCtx)      // cap
        });
    }
    return type;
}

llvm::StructType* Types::closureType() {
    // `lucid.Closure`:
    //     { ptr fn, ptr env }
    // The runtime shape of every `cls`-shaped function value. `fn` is a
    // bare function pointer whose first parameter is `env`; `env` is
    // either null (non-capturing) or a pointer to a `ClosureEnvHeader`-
    // prefixed block (see src/runtime/ClosureEnvironment.hpp).
    llvm::StructType* type =
        llvm::StructType::getTypeByName(llvmCtx, "lucid.Closure");
    if (!type) {
        type = llvm::StructType::create(llvmCtx, "lucid.Closure");
        type->setBody({
            llvm::PointerType::get(llvmCtx, 0),  // fn
            llvm::PointerType::get(llvmCtx, 0)   // env
        });
    }
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Named Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::namedType(NamedTypeAST* named) {
    if (!named) return nullptr;

    // ─── Traits have no runtime representation ────────────────────────────
    // A trait name reaching `Types` means Sema let a trait through as a
    // value type, which is a Sema bug. Return an opaque placeholder so the
    // type mapping doesn't crash, and let the caller report it.
    if (named->resolvedDecl
        && named->resolvedDecl->isa<TraitDeclAST>()) {
        return llvm::StructType::create(
            llvmCtx, pool.lookup(named->name) + "__trait_placeholder");
    }

    // ─── Sema-resolved declaration ────────────────────────────────────────
    // `resolvedDecl` is always non-null after Sema; if it's a struct or
    // enum, that's the answer. There is no template path — Sema
    // produced a specialized declaration before codegen runs.
    if (named->resolvedDecl) {
        if (named->resolvedDecl->isa<StructDeclAST>()) {
            return structType(named->resolvedDecl->as<StructDeclAST>());
        }
        if (named->resolvedDecl->isa<EnumDeclAST>()) {
            return enumType(named->resolvedDecl->as<EnumDeclAST>());
        }
    }

    // ─── Primitive alias ──────────────────────────────────────────────────
    // Some `NamedTypeAST`s name a primitive (`int`, `float`, `string`,
    // ...) rather than a user type. This shouldn't happen after Sema
    // normalization — Sema turns primitive names into `PrimitiveTypeAST`
    // — but the fallback is cheap and it keeps `Types` from crashing on
    // a not-yet-normalized input.
    std::string typeNameStr = pool.lookup(named->name);
    static const std::unordered_map<std::string, PrimitiveKind> primMap = {
        {"bool", PrimitiveKind::Bool},
        {"int8", PrimitiveKind::Int8},     {"int16", PrimitiveKind::Int16},
        {"int32", PrimitiveKind::Int32},   {"int64", PrimitiveKind::Int64},
        {"uint8", PrimitiveKind::Uint8},   {"uint16", PrimitiveKind::Uint16},
        {"uint32", PrimitiveKind::Uint32}, {"uint64", PrimitiveKind::Uint64},
        {"byte", PrimitiveKind::Byte},     {"short", PrimitiveKind::Short},
        {"int", PrimitiveKind::Int},       {"long", PrimitiveKind::Long},
        {"ubyte", PrimitiveKind::Ubyte},   {"ushort", PrimitiveKind::Ushort},
        {"uint", PrimitiveKind::Uint},     {"ulong", PrimitiveKind::Ulong},
        {"float", PrimitiveKind::Float},   {"double", PrimitiveKind::Double},
        {"decimal", PrimitiveKind::Decimal},
        {"string", PrimitiveKind::String}, {"char", PrimitiveKind::Char},
    };

    auto it = primMap.find(typeNameStr);
    if (it != primMap.end()) {
        llvm::Type* prim = integerType(it->second);
        if (it->second == PrimitiveKind::Bool
            || it->second == PrimitiveKind::Char) {
            // `integerType` handles all integer kinds. For `string` and
            // the float kinds we fall through to a real lookup below.
        }
        if (prim) return prim;
    }

    // ─── Existing named LLVM struct ───────────────────────────────────────
    // If some other declaration already created a struct with this name,
    // reuse it. This is what makes a module-qualified and unqualified
    // reference to the same struct produce the same LLVM type.
    if (llvm::StructType* existing =
            llvm::StructType::getTypeByName(llvmCtx, typeNameStr)) {
        return existing;
    }

    // ─── Forward declaration ──────────────────────────────────────────────
    // The type is not yet defined — Sema may have produced a reference
    // before the definition was lowered. Return an opaque struct; when
    // the definition lowers, `structType` will find this same named type
    // and set its body.
    return llvm::StructType::create(llvmCtx, typeNameStr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Struct Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::StructType* Types::structType(StructDeclAST* decl) {
    if (!decl) return nullptr;

    auto it = structCache.find(decl);
    if (it != structCache.end()) {
        return it->second;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    // The mangled name is the LLVM type name. It is unique across the
    // whole program (Sema guarantees it), so an `llvm::StructType` named
    // after it is the right identity for the struct.
    std::string structName = decl->mangledName.isValid()
        ? pool.lookup(decl->mangledName)
        : pool.lookup(decl->name);

    // ─── Reuse if a forward declaration already exists ────────────────────
    llvm::StructType* structType =
        llvm::StructType::getTypeByName(llvmCtx, structName);
    if (!structType) {
        structType = llvm::StructType::create(llvmCtx, structName);
    }

    // ─── Cache the opaque type BEFORE building fields ─────────────────────
    // This is what makes self-referential structs work. While building the
    // field types, a field of type `Node<T>?` will call back into
    // `structType(Node)` via `nullableType`. The cache hit returns the
    // opaque type, which is fine — a nullable struct is `{ i8, ptr }`, not
    // `{ i8, Node }`, so the recursion terminates.
    structCache[decl] = structType;
    decl->llvmType = structType;   // still cached on the AST for now

    // ─── Build field types ────────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.reserve(decl->fields.size());

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = nullptr;

        // Function-typed fields use the runtime shape of the function
        // value (`ptr` for `fn`, `lucid.Closure` for `cls`), not the
        // function's signature type.
        if (field->type && field->type->isa<FuncTypeAST>()) {
            fieldType = functionRuntimeType(
                field->type->as<FuncTypeAST>(), /*isClosure=*/true);
        } else {
            fieldType = get(field->type);
        }

        if (!fieldType) {
            // Placeholder so the struct is still well-formed. The caller
            // that asked for the type will report the diagnostic; we
            // don't have a `DiagnosticEngine&` here.
            fieldType = llvm::Type::getInt8Ty(llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    // ─── Define the struct body ───────────────────────────────────────────
    if (structType->isOpaque()) {
        structType->setBody(fieldTypes);
    }

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enum Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::IntegerType* Types::enumType(const EnumDeclAST* decl) {
    if (!decl) return nullptr;

    if (decl->backingType) {
        return integerType(decl->backingType->primitiveKind);
    }

    // Default backing type for an enum with no explicit annotation. Sema
    // fills `backingType` with a default in practice, but the fallback
    // keeps this function total.
    return llvm::Type::getInt32Ty(llvmCtx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Function Types
// ─────────────────────────────────────────────────────────────────────────────

llvm::FunctionType* Types::functionType(FuncTypeAST* funcType,
                                        bool isClosure) {
    if (!funcType) return nullptr;

    std::vector<llvm::Type*> paramTypes;

    // A `cls`-shaped function's underlying implementation takes the
    // environment as a leading `ptr` parameter. The declared parameters
    // follow. This is the runtime signature of the closure body, not the
    // type of the value — the value's shape is `lucid.Closure`.
    if (isClosure) {
        paramTypes.push_back(llvm::PointerType::get(llvmCtx, 0));
    }

    for (ParamAST* param : funcType->params) {
        llvm::Type* paramType = nullptr;

        if (param->isVariadic) {
            // A variadic parameter is lowered as a slice. The generic
            // `lucid.Slice` shape is used because the element type at the
            // function's signature is not specialized — but the call site
            // still needs to build a typed slice. This is a known
            // simplification that a later refactor may revisit.
            paramType = sliceType();
        } else if (param->type && param->type->isa<FuncTypeAST>()) {
            paramType = functionRuntimeType(
                param->type->as<FuncTypeAST>(), /*isClosure=*/true);
        } else {
            paramType = get(param->type);
        }

        if (!paramType) {
            return nullptr;
        }
        paramTypes.push_back(paramType);
    }

    // Return type. A curried function whose return type is itself a
    // function lowers to a `ptr` return — the caller invokes it via
    // `emitCallableCall`, and the pointer is opaque at the LLVM level.
    llvm::Type* returnType = nullptr;
    if (funcType->returnType) {
        if (funcType->returnType->isa<FuncTypeAST>()) {
            llvm::FunctionType* innerFn =
                functionType(funcType->returnType->as<FuncTypeAST>(), false);
            if (!innerFn) return nullptr;
            returnType = llvm::PointerType::get(innerFn, 0);
        } else {
            returnType = get(funcType->returnType);
        }
    }
    if (!returnType) {
        returnType = llvm::Type::getVoidTy(llvmCtx);
    }

    return llvm::FunctionType::get(returnType, paramTypes, /*isVarArg=*/false);
}

llvm::Type* Types::functionRuntimeType(FuncTypeAST* funcType,
                                       bool isClosure) {
    if (!funcType) return nullptr;

    if (isClosure) {
        return closureType();
    }

    // A `fn`-shaped function value is a bare function pointer. Opaque
    // pointers mean the LLVM type is just `ptr` — the signature is
    // recoverable from the AST at every call site, and passing the
    // function as a value erases it, which is fine: the callee re-derives
    // the signature.
    return llvm::PointerType::get(llvmCtx, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::primitiveType(PrimitiveTypeAST* type) {
    if (!type) return nullptr;

    switch (type->primitiveKind) {
        case PrimitiveKind::Bool:
            return llvm::Type::getInt1Ty(llvmCtx);

        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
            return llvm::Type::getInt8Ty(llvmCtx);

        case PrimitiveKind::Int16:
        case PrimitiveKind::Short:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Ushort:
            return llvm::Type::getInt16Ty(llvmCtx);

        case PrimitiveKind::Int32:
        case PrimitiveKind::Int:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint:
            return llvm::Type::getInt32Ty(llvmCtx);

        case PrimitiveKind::Int64:
        case PrimitiveKind::Long:
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Ulong:
            return llvm::Type::getInt64Ty(llvmCtx);

        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(llvmCtx);

        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(llvmCtx);

        case PrimitiveKind::Decimal:
            return llvm::Type::getFP128Ty(llvmCtx);

        case PrimitiveKind::String:
            return stringType();

        case PrimitiveKind::Char:
            return llvm::Type::getInt8Ty(llvmCtx);
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pointer and Reference
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::ptrType(PtrTypeAST* /*type*/) {
    // Raw pointers are the sealed conduit — they lower to an opaque `ptr`.
    // The pointee type is metadata for Sema's checks, not for LLVM.
    return llvm::PointerType::get(llvmCtx, 0);
}

llvm::Type* Types::refType(RefTypeAST* /*type*/) {
    // References are non-null pointers at the LLVM level. The pointee
    // type is checked by Sema; at codegen time a reference is just a
    // pointer with a stronger invariant.
    return llvm::PointerType::get(llvmCtx, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Array Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::arrayType(ArrayTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* elemType = get(type->element);
    if (!elemType) return nullptr;

    switch (type->arrayKind) {
        case ArrayKind::Fixed:
            // `[N]T` is an inline LLVM array. The size is part of the type.
            return llvm::ArrayType::get(elemType, type->size);

        case ArrayKind::Dynamic:
            // `[*]T` is a heap-owned buffer. Today it lowers to a bare
            // `ptr`; the length is not tracked in the type. This is a
            // known limitation — the ownership layer needs the length
            // to free correctly, and today it doesn't have it. A future
            // refactor may change this to `{ ptr data, i64 len, i64 cap }`
            // to match `OwnedBuffer`'s actual shape. For now, preserving
            // the current shape keeps this task a rename.
            return llvm::PointerType::get(llvmCtx, 0);

        case ArrayKind::Slice: {
            // `[_]T` is a borrowed view: `{ ptr data, i64 len, i64 cap }`.
            // Named after the element type so two slices with different
            // element types get distinct LLVM types (and are not
            // assignable to each other even though the layouts match).
            std::string typeNameStr = "slice_" + typeName(type->element);
            return llvm::StructType::create(
                llvmCtx,
                llvm::ArrayRef<llvm::Type*>{
                    llvm::PointerType::get(llvmCtx, 0),
                    llvm::Type::getInt64Ty(llvmCtx),
                    llvm::Type::getInt64Ty(llvmCtx)
                },
                typeNameStr);
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Nullable / Fallible / Combined / Future / Thread
// ─────────────────────────────────────────────────────────────────────────────
//
// All five lower to `{ i8 tag, T inner }` for some tag convention. The
// tag encodes the state: for nullable, 0=nil/1=value; for fallible,
// 0=err/1=value; for combined, 0=nil/1=value/2=err; for future/thread,
// 0=pending/1=ready/2=consumed/3=error.
//
// The exact tag encoding is a codegen-private convention — the ABI does
// not expose tagged slots, so the values can change without touching
// `lucid_abi.h`. The `Ownership` layer is the only code that needs to
// know the encoding, and it does, because it's the layer that reads the
// tag when it needs to decide whether to drop the inner value.

llvm::StructType* Types::nullableType(NullableTypeAST* type) {
    if (!type) return nullptr;
    llvm::Type* inner = get(type->inner);
    if (!inner) inner = llvm::Type::getInt8Ty(llvmCtx);

    return llvm::StructType::create(
        llvmCtx,
        llvm::ArrayRef<llvm::Type*>{
            llvm::Type::getInt8Ty(llvmCtx),  // tag
            inner
        },
        "nullable_" + typeName(type->inner));
}

llvm::StructType* Types::fallibleType(FallibleTypeAST* type) {
    if (!type) return nullptr;
    llvm::Type* inner = get(type->inner);
    if (!inner) inner = llvm::Type::getInt8Ty(llvmCtx);

    return llvm::StructType::create(
        llvmCtx,
        llvm::ArrayRef<llvm::Type*>{
            llvm::Type::getInt8Ty(llvmCtx),
            inner
        },
        "fallible_" + typeName(type->inner));
}

llvm::StructType* Types::combinedType(CombinedTypeAST* type) {
    if (!type) return nullptr;
    llvm::Type* inner = get(type->inner);
    if (!inner) inner = llvm::Type::getInt8Ty(llvmCtx);

    return llvm::StructType::create(
        llvmCtx,
        llvm::ArrayRef<llvm::Type*>{
            llvm::Type::getInt8Ty(llvmCtx),
            inner
        },
        "combined_" + typeName(type->inner));
}

llvm::StructType* Types::futureType(FutureTypeAST* type) {
    if (!type) return nullptr;
    llvm::Type* inner = get(type->inner);
    if (!inner) inner = llvm::Type::getInt8Ty(llvmCtx);

    return llvm::StructType::create(
        llvmCtx,
        llvm::ArrayRef<llvm::Type*>{
            inner,
            llvm::Type::getInt8Ty(llvmCtx)  // state
        },
        "future_" + typeName(type->inner));
}

llvm::StructType* Types::threadType(const ThreadTypeAST* type) {
    if (!type) return nullptr;
    llvm::Type* inner = get(type->inner);
    if (!inner) inner = llvm::Type::getInt8Ty(llvmCtx);

    return llvm::StructType::create(
        llvmCtx,
        llvm::ArrayRef<llvm::Type*>{
            inner,
            llvm::Type::getInt8Ty(llvmCtx)
        },
        "thread_" + typeName(type->inner));
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Type Access
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* Types::moduleTypeAccess(ModuleTypeAccessAST* type) {
    // A module-qualified type reference — `parser:Result` rather than
    // `Result`. Sema transforms these into `NamedTypeAST`s during
    // resolution, so by the time `get` sees one it should be rare. But
    // it's not impossible: an unresolved module-qualified reference can
    // slip through if a module failed to load. Return a struct named
    // after the qualified name and hope for the best; a real error is
    // reported at the call site.
    if (!type) return nullptr;

    std::string qualifiedName =
        pool.lookup(type->moduleName) + "." + pool.lookup(type->typeName);

    llvm::StructType* structType =
        llvm::StructType::getTypeByName(llvmCtx, qualifiedName);
    if (!structType) {
        structType = llvm::StructType::getTypeByName(
            llvmCtx, pool.lookup(type->typeName));
    }
    if (!structType) {
        structType = llvm::StructType::create(llvmCtx, qualifiedName);
    }
    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Instance Layout
// ─────────────────────────────────────────────────────────────────────────────

llvm::StructType* Types::moduleInstanceType(
    ModuleAST* module,
    std::unordered_map<ModuleAST*, ModuleInstanceLayout>& layouts)
{
    if (!module) return nullptr;

    // ─── Cache hit ────────────────────────────────────────────────────────
    auto cached = layouts.find(module);
    if (cached != layouts.end() && cached->second.type) {
        return cached->second.type;
    }

    // ─── Collect module-level bindings in index order ─────────────────────
    // Sema assigned a contiguous index to each module-level binding. The
    // declaration list is in source order, and Sema assigned indices in
    // that same order, but sorting defensively is cheap and documents
    // the invariant.
    std::vector<ValueDeclAST*> fields;
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        if (!decl->isa<ValueDeclAST>()) continue;
        ValueDeclAST* v = decl->as<ValueDeclAST>();
        if (v->moduleFieldIndex == SIZE_MAX) continue;
        fields.push_back(v);
    }
    std::sort(fields.begin(), fields.end(),
              [](ValueDeclAST* a, ValueDeclAST* b) {
                  return a->moduleFieldIndex < b->moduleFieldIndex;
              });

    // ─── Build the field types ────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.reserve(fields.size());
    for (ValueDeclAST* v : fields) {
        llvm::Type* fieldType = nullptr;
        if (v->isa<VarDeclAST>()) {
            fieldType = get(v->type);
        } else if (v->isa<FuncDeclAST>()) {
            // Reserved for the cls-shaped module-level binding follow-up.
            // Today, a module-level FuncDeclAST that is `cls`-shaped
            // would have its value stored in this slot as a fat pointer.
            fieldType = closureType();
        }
        if (!fieldType) {
            // Placeholder. The caller that asked for the type should
            // have validated it; we return a sized struct so downstream
            // GEPs don't crash, and let a later pass diagnose.
            fieldType = llvm::Type::getInt8Ty(llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    // ─── Create (or reuse) the named struct type ──────────────────────────
    std::string structName =
        "module_" + pool.lookup(module->filePath);
    // Sanitize: replace characters that aren't valid in LLVM symbol names.
    for (char& c : structName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }

    llvm::StructType* structType =
        llvm::StructType::getTypeByName(llvmCtx, structName);
    if (!structType) {
        structType = llvm::StructType::create(llvmCtx, structName);
    }
    if (structType->isOpaque()) {
        structType->setBody(fieldTypes);
    }

    // ─── Cache the layout ─────────────────────────────────────────────────
    ModuleInstanceLayout layout;
    layout.type = structType;
    layout.fields = std::move(fields);
    for (size_t i = 0; i < layout.fields.size(); ++i) {
        layout.fieldOf[layout.fields[i]] = i;
    }
    layouts[module] = std::move(layout);

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::IntegerType* Types::integerType(PrimitiveKind kind) {
    size_t bits = getPrimitiveBitWidth(kind);
    if (bits == 0) {
        // Not an integer kind. Return i32 as a deterministic fallback;
        // callers that needed a non-integer should not have called this.
        return llvm::Type::getInt32Ty(llvmCtx);
    }
    return llvm::IntegerType::get(llvmCtx, static_cast<unsigned>(bits));
}

llvm::Type* Types::floatType(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(llvmCtx);
        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(llvmCtx);
        case PrimitiveKind::Decimal:
            return llvm::Type::getFP128Ty(llvmCtx);
        default:
            return llvm::Type::getFloatTy(llvmCtx);
    }
}

std::string Types::typeName(TypeAST* type) {
    if (!type) return "void";

    // ─── Primitives: short names ──────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        switch (type->as<PrimitiveTypeAST>()->primitiveKind) {
            case PrimitiveKind::Bool:    return "bool";
            case PrimitiveKind::Int8:
            case PrimitiveKind::Byte:    return "int8";
            case PrimitiveKind::Int16:
            case PrimitiveKind::Short:   return "int16";
            case PrimitiveKind::Int32:
            case PrimitiveKind::Int:     return "int32";
            case PrimitiveKind::Int64:
            case PrimitiveKind::Long:    return "int64";
            case PrimitiveKind::Uint8:
            case PrimitiveKind::Ubyte:   return "uint8";
            case PrimitiveKind::Uint16:
            case PrimitiveKind::Ushort:  return "uint16";
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Uint:    return "uint32";
            case PrimitiveKind::Uint64:
            case PrimitiveKind::Ulong:   return "uint64";
            case PrimitiveKind::Float:   return "float";
            case PrimitiveKind::Double:  return "double";
            case PrimitiveKind::Decimal: return "decimal";
            case PrimitiveKind::String:  return "string";
            case PrimitiveKind::Char:    return "char";
        }
        return "primitive";
    }

    // ─── SIMD ─────────────────────────────────────────────────────────────
    if (type->isa<SimdTypeAST>()) {
        SimdTypeAST* simd = type->as<SimdTypeAST>();
        return "Simd_" + typeName(simd->elementType) + "_"
             + std::to_string(simd->laneCount);
    }

    // ─── Built-ins ────────────────────────────────────────────────────────
    if (type->isa<ArenaTypeAST>())           return "Arena";
    if (type->isa<ArenaDescriptorTypeAST>()) return "ArenaDescriptor";

    // ─── Named types: use the source name ─────────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        return pool.lookup(type->as<NamedTypeAST>()->name);
    }

    // ─── Fallback ─────────────────────────────────────────────────────────
    return astKindToString(type->kind);
}

uint64_t Types::sizeOf(TypeAST* type) {
    llvm::Type* llvmType = get(type);
    if (!llvmType || !llvmType->isSized()) return 0;
    return module.getDataLayout()
                 .getTypeAllocSize(llvmType)
                 .getFixedValue();
}

uint64_t Types::alignOf(TypeAST* type) {
    llvm::Type* llvmType = get(type);
    if (!llvmType || !llvmType->isSized()) return 0;
    return module.getDataLayout()
                 .getABITypeAlign(llvmType)
                 .value();
}

// ─────────────────────────────────────────────────────────────────────────────
// Static Predicates
// ─────────────────────────────────────────────────────────────────────────────

bool Types::isOwnedBuffer(TypeAST* type) {
    if (!type) return false;

    // A string or a dynamic array is an owned buffer. Both lower to
    // the same `{ ptr, i64, i64 }` shape (or, for the dynamic array
    // today, to a bare `ptr` that will be a `{ ptr, i64, i64 }` after
    // the ownership refactor) and both share the same semantics: a
    // copy is a deep copy, a drop frees the buffer.
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind
            == PrimitiveKind::String;
    }
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isDynamic();
    }
    return false;
}

} // namespace codegen