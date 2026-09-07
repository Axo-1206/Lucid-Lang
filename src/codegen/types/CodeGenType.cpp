/// @file CodeGenType.cpp
/// @brief Implementation of type mapping from Lucid AST types to LLVM types.

#include "CodeGenType.hpp"
#include "core/ASTStrings.hpp"
#include "../generic/CodeGenGeneric.hpp"  // For GenericSubstitution
#include "../generic/GenericMangledName.hpp"
#include "core/ast/DeclAST.hpp"
#include "sema/types/SemaType.hpp"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Module.h>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Type* getType(CodeGenContext& ctx, TypeAST* type) {
    return getType(ctx, type, nullptr);
}

llvm::Type* getType(CodeGenContext& ctx, TypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    // ─── If no substitution was passed, check the context ──────────────
    if (!subst) {
        subst = ctx.currentGenericSubstitution;
    }

    // ─── Check cache only when no substitution ──────────────────────────────
    // When substitution is present, we cannot cache by type alone because
    // the same type with different substitutions yields different LLVM types.
    if (!subst) {
        auto it = ctx.typeCache.find(type);
        if (it != ctx.typeCache.end()) {
            return it->second;
        }
    }

    llvm::Type* result = nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            result = getPrimitiveType(ctx, type->as<PrimitiveTypeAST>());
            break;

        case ASTKind::SimdType:
            result = getSimdType(ctx, type->as<SimdTypeAST>());
            break;

        case ASTKind::ArenaType:
            result = getArenaType(ctx);
            break;

        case ASTKind::ArenaDescriptorType:
            result = getArenaDescriptorType(ctx);
            break;

        case ASTKind::NamedType:
            result = getNamedType(ctx, type->as<NamedTypeAST>(), subst);
            break;

        case ASTKind::ModuleTypeAccess:
            result = getModuleTypeAccess(ctx, type->as<ModuleTypeAccessAST>());
            break;

        case ASTKind::PtrType:
            result = getPtrType(ctx, type->as<PtrTypeAST>());
            break;

        case ASTKind::RefType:
            result = getRefType(ctx, type->as<RefTypeAST>());
            break;

        case ASTKind::ArrayType:
            result = getArrayType(ctx, type->as<ArrayTypeAST>(), subst);
            break;

        case ASTKind::FuncType:
            result = getFunctionType(ctx, type->as<FuncTypeAST>(), false);
            break;

        case ASTKind::NullableType:
            result = getNullableType(ctx, type->as<NullableTypeAST>(), subst);
            break;

        case ASTKind::FallibleType:
            result = getFallibleType(ctx, type->as<FallibleTypeAST>(), subst);
            break;

        case ASTKind::CombinedType:
            result = getCombinedType(ctx, type->as<CombinedTypeAST>(), subst);
            break;

        case ASTKind::FutureType:
            result = getFutureType(ctx, type->as<FutureTypeAST>(), subst);
            break;

        case ASTKind::ThreadType:
            result = getThreadType(ctx, type->as<ThreadTypeAST>(), subst);
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, type->loc,
                                    "unknown type kind in code generation");
            return nullptr;
    }

    // ─── Cache only when no substitution ──────────────────────────────────
    if (result && !subst) {
        ctx.typeCache[type] = result;
    }

    return result;
}

// ─── Built-in Type Accessors ─────────────────────────────────────────────

llvm::VectorType* getSimdType(CodeGenContext& ctx, SimdTypeAST* simd) {
    if (!simd) return nullptr;

    // Sema should have already validated these, but keep safety checks.
    if (simd->laneCount == 0) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSimdLaneCount, simd->loc,
                                "Simd lane count must be > 0");
        return nullptr;
    }

    if (!simd->elementType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSimdElementType, simd->loc,
                                "Simd type has no element type");
        return nullptr;
    }

    // Get the LLVM type for the element type.
    // No substitution needed - Sema ensures the element type is already concrete.
    llvm::Type* elemType = getType(ctx, simd->elementType);
    if (!elemType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSimdElementType, simd->loc,
                                "Simd element type has unknown type");
        return nullptr;
    }

    // Return LLVM vector type: <N x T>
    return llvm::VectorType::get(elemType, simd->laneCount, false);
}

llvm::StructType* getArenaType(CodeGenContext& ctx) {
    // Arena is an opaque struct: { i8* base, i64 size, i64 cursor }
    // The compiler manages this directly; user code never sees the fields.
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx.llvmCtx, "lucid.Arena");
    if (!type) {
        type = llvm::StructType::create(ctx.llvmCtx, "lucid.Arena");
        type->setBody({
            llvm::PointerType::get(ctx.llvmCtx, 0),  // base: i8*
            llvm::Type::getInt64Ty(ctx.llvmCtx),    // size: i64
            llvm::Type::getInt64Ty(ctx.llvmCtx)     // cursor: i64
        });
    }
    return type;
}

llvm::StructType* getArenaDescriptorType(CodeGenContext& ctx) {
    // ArenaDescriptor: { i8* base, i64 size }
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx.llvmCtx, "lucid.ArenaDescriptor");
    if (!type) {
        type = llvm::StructType::create(ctx.llvmCtx, "lucid.ArenaDescriptor");
        type->setBody({
            llvm::PointerType::get(ctx.llvmCtx, 0),  // base: i8*
            llvm::Type::getInt64Ty(ctx.llvmCtx)     // size: i64
        });
    }
    return type;
}

// ─── Named Type ────────────────────────────────────────────────────────────

llvm::Type* getNamedType(CodeGenContext& ctx, NamedTypeAST* named, const GenericSubstitution* subst) {
    if (!named) return nullptr;

    // ─── If no substitution was passed, check the context ──────────────
    if (!subst) {
        subst = ctx.currentGenericSubstitution;
    }

    // ─── Defensive check for traits ──────────────────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<TraitDeclAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TraitInvalidContext, named->loc,
                                "INTERNAL ERROR: trait '", ctx.pool.lookup(named->name),
                                "' reached CodeGen - Sema should have rejected this");
        return llvm::StructType::create(ctx.llvmCtx, 
            ctx.pool.lookup(named->name) + "__trait_placeholder");
    }

    std::string typeName = ctx.pool.lookup(named->name);

    // ─── 1. Check if this is a generic parameter ──────────────────────────
    if (subst) {
        // First, check if this name is a generic parameter in the substitution
        if (subst->isGenericParam(named->name)) {
            TypeAST* substituted = subst->lookup(named->name);
            if (substituted) {
                // Recursively get LLVM type of the substituted type
                // This handles T -> int, or T -> Box<int>, etc.
                return getType(ctx, substituted, subst);
            }
            // Shouldn't happen if arity was validated
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, named->loc,
                                    "missing type argument for generic parameter '",
                                    ctx.pool.lookup(named->name), "'");
            return nullptr;
        }
        
        // Also check if the name matches a generic parameter from the outer context
        // This handles the case where subst is for Box<T> and named->name is "T"
        // but "T" is actually from the outer Wrapper<T> context.
        // The outer context is ctx.currentGenericSubstitution, which we already have.
    }

    // ─── 2. Resolve struct/enum via resolvedDecl ──────────────────────────
    if (named->resolvedDecl) {
        if (named->resolvedDecl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = named->resolvedDecl->as<StructDeclAST>();
            
            // ─── Generic struct: use specialized resolution ────────────────────
            if (isGenericStruct(structDecl)) {
                // Pass the generic args from the NamedTypeAST.
                // These args may contain generic parameters from the outer context
                // (e.g., Box<T> where T is from Wrapper<T>).
                // getOrCreateSpecializedStruct will handle this.
                return getOrCreateSpecializedStruct(structDecl, named->genericArgs, ctx);
            }
            
            // ─── Non-generic struct: normal lookup ────────────────────────────
            return getStructType(ctx, structDecl);
        }
        if (named->resolvedDecl->isa<EnumDeclAST>()) {
            return getEnumType(ctx, named->resolvedDecl->as<EnumDeclAST>());
        }
    }

    // ─── 3. Try to resolve as a primitive type ──────────────────────────────
    static const std::unordered_map<std::string, PrimitiveKind> primMap = {
        {"bool", PrimitiveKind::Bool},
        {"int8", PrimitiveKind::Int8},
        {"int16", PrimitiveKind::Int16},
        {"int32", PrimitiveKind::Int32},
        {"int64", PrimitiveKind::Int64},
        {"uint8", PrimitiveKind::Uint8},
        {"uint16", PrimitiveKind::Uint16},
        {"uint32", PrimitiveKind::Uint32},
        {"uint64", PrimitiveKind::Uint64},
        {"byte", PrimitiveKind::Byte},
        {"short", PrimitiveKind::Short},
        {"int", PrimitiveKind::Int},
        {"long", PrimitiveKind::Long},
        {"ubyte", PrimitiveKind::Ubyte},
        {"ushort", PrimitiveKind::Ushort},
        {"uint", PrimitiveKind::Uint},
        {"ulong", PrimitiveKind::Ulong},
        {"float", PrimitiveKind::Float},
        {"double", PrimitiveKind::Double},
        {"decimal", PrimitiveKind::Decimal},
        {"string", PrimitiveKind::String},
        {"char", PrimitiveKind::Char}
    };
    
    auto it = primMap.find(typeName);
    if (it != primMap.end()) {
        PrimitiveTypeAST tmp(it->second);
        return getPrimitiveType(ctx, &tmp);
    }

    // ─── 4. Try to find an existing struct type ─────────────────────────────
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName)) {
        return existing;
    }

    // ─── 5. Unknown type - create forward declaration ──────────────────────
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, named->loc,
                              "type '", typeName, "' not yet defined, "
                              "creating forward declaration");

    return llvm::StructType::create(ctx.llvmCtx, typeName);
}

// ─── Struct Type ──────────────────────────────────────────────────────────

/// @brief Get the LLVM struct type for a Lucid struct declaration.
///
/// ─── Self-Reference Handling ─────────────────────────────────────────────
/// This function handles self-referential structs using the opaque type pattern:
///   1. Create an OPAQUE (incomplete) struct type FIRST
///   2. Cache the opaque type
///   3. Build field types (self-references return pointers to the opaque type)
///   4. Set the struct body with all field types
///
/// The key insight is that the opaque type breaks the infinite recursion.
llvm::StructType* getStructType(CodeGenContext& ctx, StructDeclAST* decl) {
    if (!decl) return nullptr;

    // ─── 1. Check cache ─────────────────────────────────────────────────────
    auto it = ctx.structCache.find(decl);
    if (it != ctx.structCache.end()) {
        return it->second;
    }

    // ─── 2. Get the mangled name ───────────────────────────────────────────
    std::string structName;
    if (decl->mangledName.isValid()) {
        structName = ctx.pool.lookup(decl->mangledName);
    } else {
        structName = ctx.pool.lookup(decl->name);
    }

    // ─── 3. Check if the struct type already exists by name ──────────────
    llvm::StructType* structType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        structName
    );

    // ─── 4. Create OPAQUE type FIRST (critical for self-reference) ──────
    // This must happen BEFORE building field types!
    // The opaque type serves as a forward declaration that self-referential
    // fields can reference.
    if (!structType) {
        structType = llvm::StructType::create(ctx.llvmCtx, structName);
    }

    // ─── 5. Cache the opaque type BEFORE building fields ──────────────────
    // This is critical - self-referential fields need to find the struct
    // in the cache when getType() is called recursively.
    ctx.cacheStruct(decl, structType);

    // ─── 6. Build field types ──────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.reserve(decl->fields.size());

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = nullptr;
        
        if (field->type && field->type->isa<FuncTypeAST>()) {
            // Function-typed field: use runtime type { ptr, ptr }
            fieldType = getFunctionRuntimeType(
                ctx,
                field->type->as<FuncTypeAST>(),
                true
            );
        } else {
            // For self-referential fields, getType() will return a pointer
            // to the opaque struct type because it's already in the cache.
            fieldType = getType(ctx, field->type);
        }

        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, field->loc,
                                    "field '", ctx.pool.lookup(field->name),
                                    "' has unknown type");
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    // ─── 7. Define the opaque struct ──────────────────────────────────────
    if (structType->isOpaque()) {
        structType->setBody(fieldTypes);
    }

    // ─── 8. Store in cache ─────────────────────────────────────────────────
    ctx.cacheStruct(decl, structType);
    decl->llvmType = structType;

    return structType;
}

// ─── Enum Type ────────────────────────────────────────────────────────────

llvm::IntegerType* getEnumType(CodeGenContext& ctx, const EnumDeclAST* decl) {
    if (!decl) return nullptr;

    // If a backing type is specified, use it
    if (decl->backingType) {
        return getIntegerType(ctx, decl->backingType->primitiveKind);
    }

    // Default: int32 (matches C enum behavior)
    return llvm::Type::getInt32Ty(ctx.llvmCtx);
}

// ─── Function Type ────────────────────────────────────────────────────────

llvm::FunctionType* getFunctionType(CodeGenContext& ctx, FuncTypeAST* funcType, bool isClosure) {
    if (!funcType) return nullptr;

    std::vector<llvm::Type*> paramTypes;

    // ─── For closures, add environment pointer as first parameter ──────────
    if (isClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // ─── Add regular parameters ────────────────────────────────────────────
    for (ParamAST* param : funcType->params) {
        llvm::Type* paramType = nullptr;
        if (param->isVariadic) {
            paramType = ctx.getSliceType();
        } else if (param->type && param->type->isa<FuncTypeAST>()) {
            paramType = getFunctionRuntimeType(
                ctx,
                param->type->as<FuncTypeAST>(),
                true
            );
        } else {
            paramType = getType(ctx, param->type);
        }
        if (!paramType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, param->loc,
                                    "parameter '", ctx.pool.lookup(param->name),
                                    "' has unknown type");
            return nullptr;
        }
        paramTypes.push_back(paramType);
    }

    // ─── Get return type ────────────────────────────────────────────────────
    llvm::Type* returnType = nullptr;
    if (funcType->returnType) {
        if (funcType->returnType->isa<FuncTypeAST>()) {
            // Curried return: pointer to inner function type
            llvm::FunctionType* innerType = getFunctionType(
                ctx,
                funcType->returnType->as<FuncTypeAST>(),
                false
            );
            returnType = llvm::PointerType::get(innerType, 0);
        } else {
            returnType = getType(ctx, funcType->returnType);
        }
    }

    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    // Source-level variadic parameters are lowered as one explicit slice.
    return llvm::FunctionType::get(returnType, paramTypes, false);
}

llvm::Type* getFunctionRuntimeType(CodeGenContext& ctx, FuncTypeAST* funcType, bool isClosure) {
    if (!funcType) return nullptr;

    if (isClosure) {
        return ctx.getClosureType();
    }

    llvm::FunctionType* functionType = getFunctionType(ctx, funcType, false);
    return functionType
        ? llvm::PointerType::get(ctx.llvmCtx, 0)
        : nullptr;
}

// ─── Primitive Type ───────────────────────────────────────────────────────

llvm::Type* getPrimitiveType(CodeGenContext& ctx, PrimitiveTypeAST* type) {
    if (!type) return nullptr;

    switch (type->primitiveKind) {
        case PrimitiveKind::Bool:
            return llvm::Type::getInt1Ty(ctx.llvmCtx);

        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
            return llvm::Type::getInt8Ty(ctx.llvmCtx);

        case PrimitiveKind::Int16:
        case PrimitiveKind::Short:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Ushort:
            return llvm::Type::getInt16Ty(ctx.llvmCtx);

        case PrimitiveKind::Int32:
        case PrimitiveKind::Int:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint:
            return llvm::Type::getInt32Ty(ctx.llvmCtx);

        case PrimitiveKind::Int64:
        case PrimitiveKind::Long:
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Ulong:
            return llvm::Type::getInt64Ty(ctx.llvmCtx);

        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(ctx.llvmCtx);

        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(ctx.llvmCtx);

        case PrimitiveKind::Decimal:
            return llvm::Type::getFP128Ty(ctx.llvmCtx);

        case PrimitiveKind::String:
            // Strings are heap-allocated UTF-8 buffers
            // Represented as a pointer to the buffer with a length
            // For now, just use a pointer
            return llvm::PointerType::get(ctx.llvmCtx, 0);

        case PrimitiveKind::Char:
            return llvm::Type::getInt8Ty(ctx.llvmCtx);

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, type->loc,
                                    "unknown primitive type");
            return nullptr;
    }
}

// ─── Pointer Type ─────────────────────────────────────────────────────────

llvm::Type* getPtrType(CodeGenContext& ctx, PtrTypeAST* type) {
    if (!type) return nullptr;

    // Raw pointers are always opaque pointers
    // We don't need the pointee type for LLVM's opaque pointer model
    (void)type;
    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

// ─── Reference Type ──────────────────────────────────────────────────────

llvm::Type* getRefType(CodeGenContext& ctx, RefTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidPointerTarget, type->loc,
                                "reference target type has unknown type");
        return llvm::PointerType::get(ctx.llvmCtx, 0);
    }

    // With opaque pointers, we don't need the element type
    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

// ─── Array Type ──────────────────────────────────────────────────────────

llvm::Type* getArrayType(CodeGenContext& ctx, ArrayTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    // ─── Pass substitution to element type ──────────────────────────────
    // This handles arrays of generic types: [*]T where T is a generic param
    llvm::Type* elemType = getType(ctx, type->element, subst);
    if (!elemType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, type->loc,
                                "array element type has unknown type");
        return nullptr;
    }

    switch (type->arrayKind) {
        case ArrayKind::Fixed:
            return llvm::ArrayType::get(elemType, type->size);

        case ArrayKind::Dynamic:
            // Dynamic arrays are heap-allocated, stored as a pointer
            return llvm::PointerType::get(ctx.llvmCtx, 0);

        case ArrayKind::Slice:
            // Slices are { ptr, len, cap }
            {
                llvm::Type* ptrType = llvm::PointerType::get(ctx.llvmCtx, 0);
                llvm::Type* lenType = llvm::Type::getInt64Ty(ctx.llvmCtx);
                std::string typeName = "slice_" + typeToString(type->element, ctx.pool);
                return llvm::StructType::create(
                    ctx.llvmCtx,
                    llvm::ArrayRef<llvm::Type*>{ptrType, lenType, lenType},
                    typeName
                );
            }

        default:
            return nullptr;
    }
}

// ─── Nullable Type ───────────────────────────────────────────────────────

llvm::StructType* getNullableType(CodeGenContext& ctx, NullableTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner, subst);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "nullable inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "nullable_" + typeToString(type->inner, ctx.pool);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Fallible Type ───────────────────────────────────────────────────────

llvm::StructType* getFallibleType(CodeGenContext& ctx, FallibleTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner, subst);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "fallible inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "fallible_" + typeToString(type->inner, ctx.pool);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Combined Type ───────────────────────────────────────────────────────

llvm::StructType* getCombinedType(CodeGenContext& ctx, CombinedTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner, subst);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "combined inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "combined_" + typeToString(type->inner, ctx.pool);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Future Type ─────────────────────────────────────────────────────────

llvm::StructType* getFutureType(CodeGenContext& ctx, FutureTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner, subst);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "future inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    // Future<T> = { T value, i8 state }
    // state: 0 = pending, 1 = ready, 2 = consumed
    std::string typeName = "future_" + typeToString(type->inner, ctx.pool);
    llvm::Type* stateType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{innerType, stateType},
        typeName
    );
}

// ─── Thread Type ─────────────────────────────────────────────────────────

llvm::StructType* getThreadType(CodeGenContext& ctx, const ThreadTypeAST* type, const GenericSubstitution* subst) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner, subst);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "thread inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    // Thread<T> = { T value, i8 state }
    // state: 0 = running, 1 = done, 2 = joined
    std::string typeName = "thread_" + typeToString(type->inner, ctx.pool);
    llvm::Type* stateType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{innerType, stateType},
        typeName
    );
}

// ─── Module Type Access ──────────────────────────────────────────────────

llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, ModuleTypeAccessAST* type) {
    if (!type) return nullptr;

    std::string moduleName = ctx.pool.lookup(type->moduleName);
    std::string typeName = ctx.pool.lookup(type->typeName);

    // ─── Try to find the target module ────────────────────────────────────
    ModuleAST* targetModule = nullptr;
    if (ctx.currentModule) {
        auto it = ctx.currentModule->resolvedImports.find(type->moduleName);
        if (it != ctx.currentModule->resolvedImports.end()) {
            targetModule = it->second;
        }
    }

    // ─── If we have the target module, resolve the declaration ────────────
    if (targetModule) {
        for (DeclAST* decl : targetModule->decls) {
            if (decl->isa<TypeDeclAST>()) {
                TypeDeclAST* typeDecl = decl->as<TypeDeclAST>();
                if (typeDecl->name == type->typeName) {
                    
                    if (typeDecl->isa<StructDeclAST>()) {
                        StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
                        
                        if (isGenericStruct(structDecl)) {
                            // ─── Generic struct: use specialized resolution ──
                            // The generic args may contain generic parameters
                            // from the current context.
                            return getOrCreateSpecializedStruct(structDecl, type->genericArgs, ctx);
                        }
                        
                        // ─── Non-generic struct ──────────────────────────────
                        return getStructType(ctx, structDecl);
                    }
                    
                    if (typeDecl->isa<EnumDeclAST>()) {
                        return getEnumType(ctx, typeDecl->as<EnumDeclAST>());
                    }
                }
            }
        }
    }

    // ─── Fallback: try qualified name ──────────────────────────────────────
    std::string qualifiedName = moduleName + "." + typeName;
    llvm::StructType* structType = llvm::StructType::getTypeByName(ctx.llvmCtx, qualifiedName);
    
    if (!structType) {
        structType = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName);
    }

    if (!structType) {
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, type->loc,
                                  "module type '", moduleName, ":", typeName,
                                  "' not found, creating forward declaration");
        structType = llvm::StructType::create(ctx.llvmCtx, qualifiedName);
    }

    return structType;
}

// ─── Helper Functions ─────────────────────────────────────────────────────

llvm::IntegerType* getIntegerType(CodeGenContext& ctx, PrimitiveKind kind) {
    size_t bits = getPrimitiveBitWidth(kind);
    if (bits == 0) {
        // Fallback for non-integer types
        return llvm::Type::getInt32Ty(ctx.llvmCtx);
    }
    return llvm::IntegerType::get(ctx.llvmCtx, static_cast<unsigned>(bits));
}

llvm::Type* getFloatType(CodeGenContext& ctx, PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Float:
            return llvm::Type::getFloatTy(ctx.llvmCtx);
        case PrimitiveKind::Double:
            return llvm::Type::getDoubleTy(ctx.llvmCtx);
        case PrimitiveKind::Decimal:
            return llvm::Type::getFP128Ty(ctx.llvmCtx);
        default:
            return llvm::Type::getFloatTy(ctx.llvmCtx);
    }
}

std::string getTypeName(CodeGenContext& ctx, TypeAST* type) {
    if (!type) return "void";
    
    // For primitive types, use typeToString or a simplified mapping
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
        return std::string(1, encodePrimitiveKind(prim->primitiveKind));
    }
    
    // For Simd types, include element type and lane count
    if (type->isa<SimdTypeAST>()) {
        SimdTypeAST* simd = type->as<SimdTypeAST>();
        return "Simd_" + getTypeName(ctx, simd->elementType) + "_" + std::to_string(simd->laneCount);
    }
    
    // For Arena and ArenaDescriptor
    if (type->isa<ArenaTypeAST>()) {
        return "Arena";
    }
    if (type->isa<ArenaDescriptorTypeAST>()) {
        return "ArenaDescriptor";
    }
    
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        return ctx.pool.lookup(named->name);
    }
    
    // Fallback: use typeToString
    return typeToString(type, ctx.pool);
}

uint64_t getTypeSize(CodeGenContext& ctx, TypeAST* type) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return 0;

    if (llvmType->isSized()) {
        return ctx.module->getDataLayout().getTypeAllocSize(llvmType);
    }

    return 0;
}

uint64_t getTypeAlign(CodeGenContext& ctx, TypeAST* type) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return 0;

    if (llvmType->isSized()) {
        return ctx.module->getDataLayout().getABITypeAlign(llvmType).value();
    }

    return 0;
}

} // namespace codegen