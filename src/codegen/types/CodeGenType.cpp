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

    // ─── Defensive check for traits ──────────────────────────────────
    // Traits should never reach CodeGen because Sema rejects them everywhere
    // except generic constraints. This is a safety net.
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
        TypeAST* substituted = subst->lookup(named->name);
        if (substituted) {
            // Recursively get type of the substituted type
            return getType(ctx, substituted, subst);
        }
        if (subst->isGenericParam(named->name)) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, named->loc,
                                    "missing type argument for generic parameter '",
                                    ctx.pool.lookup(named->name), "'");
            return nullptr;
        }
    }

    // ─── 2. Resolve struct/enum via resolvedDecl ──────────────────────────
    if (named->resolvedDecl) {
        if (named->resolvedDecl->isa<StructDeclAST>()) {
            return getStructType(ctx, named->resolvedDecl->as<StructDeclAST>());
        }
        if (named->resolvedDecl->isa<EnumDeclAST>()) {
            return getEnumType(ctx, named->resolvedDecl->as<EnumDeclAST>());
        }
    }

    // ─── 3. Try to resolve as a primitive type ──────────────────────────────
    // Delegate to getPrimitiveType via the canonical mapping.
    // This is the SOURCE OF TRUTH for primitive type mapping.
    static const std::unordered_map<std::string, PrimitiveKind> primMap = {
        // Boolean
        {"bool", PrimitiveKind::Bool},
        
        // Signed integers (fixed-width)
        {"int8", PrimitiveKind::Int8},
        {"int16", PrimitiveKind::Int16},
        {"int32", PrimitiveKind::Int32},
        {"int64", PrimitiveKind::Int64},
        
        // Unsigned integers (fixed-width)
        {"uint8", PrimitiveKind::Uint8},
        {"uint16", PrimitiveKind::Uint16},
        {"uint32", PrimitiveKind::Uint32},
        {"uint64", PrimitiveKind::Uint64},
        
        // Signed integers (machine-dependent)
        {"byte", PrimitiveKind::Byte},
        {"short", PrimitiveKind::Short},
        {"int", PrimitiveKind::Int},
        {"long", PrimitiveKind::Long},
        
        // Unsigned integers (machine-dependent)
        {"ubyte", PrimitiveKind::Ubyte},
        {"ushort", PrimitiveKind::Ushort},
        {"uint", PrimitiveKind::Uint},
        {"ulong", PrimitiveKind::Ulong},
        
        // Floating point
        {"float", PrimitiveKind::Float},
        {"double", PrimitiveKind::Double},
        {"decimal", PrimitiveKind::Decimal},
        
        // Text
        {"string", PrimitiveKind::String},
        {"char", PrimitiveKind::Char}
    };
    
    auto it = primMap.find(typeName);
    if (it != primMap.end()) {
        // Create a temporary PrimitiveTypeAST and delegate
        PrimitiveTypeAST tmp(it->second);
        return getPrimitiveType(ctx, &tmp);
    }

    // ─── 4. Try to find an existing struct type ─────────────────────────────
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName)) {
        return existing;
    }

    // ─── 5. Unknown type - create forward declaration ──────────────────────
    // This typically means the type is defined in another module or
    // we're still in the declaration phase.
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, named->loc,
                              "type '", typeName, "' not yet defined, "
                              "creating forward declaration");

    return llvm::StructType::create(ctx.llvmCtx, typeName);
}

// ─── Struct Type ──────────────────────────────────────────────────────────

llvm::StructType* getStructType(CodeGenContext& ctx, StructDeclAST* decl) {
    if (!decl) return nullptr;

    auto it = ctx.structCache.find(decl);
    if (it != ctx.structCache.end()) {
        return it->second;
    }

    std::vector<llvm::Type*> fieldTypes;
    std::string structName = ctx.pool.lookup(decl->name);

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = field->type && field->type->isa<FuncTypeAST>()
            ? getFunctionRuntimeType(ctx, field->type->as<FuncTypeAST>(), true)
            : getType(ctx, field->type);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, field->loc,
                                    "field '", ctx.pool.lookup(field->name),
                                    "' has unknown type");
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>(fieldTypes),
        structName
    );

    ctx.structCache[decl] = structType;
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
            // With opaque pointers, we don't need the element type
            return llvm::PointerType::get(ctx.llvmCtx, 0);

        case ArrayKind::Slice:
            // Slices are { ptr, len, cap }
            {
                // With opaque pointers, ptr is just a pointer
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

    // Try a module-qualified name first so same-named types from different
    // modules don't alias each other.
    std::string qualifiedName = moduleName + "." + typeName;

    llvm::StructType* structType = llvm::StructType::getTypeByName(ctx.llvmCtx, qualifiedName);
    if (!structType) {
        // Fall back to the bare name for compatibility with types that were
        // registered without module qualification.
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