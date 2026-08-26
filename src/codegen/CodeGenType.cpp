/// @file CodeGenType.cpp
/// @brief Implementation of type mapping from Lucid AST types to LLVM types.

#include "CodeGenType.hpp"
#include "core/ASTStrings.hpp"
#include "generic/CodeGenGeneric.hpp"  // For GenericSubstitution
#include "generic/GenericMangledName.hpp"
#include "core/ast/DeclAST.hpp"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Module.h>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Type* getType(CodeGenContext& ctx, TypeAST* type) {
    return getType(ctx, type, nullptr);
}

llvm::Type* getType(
    CodeGenContext& ctx,
    TypeAST* type,
    const GenericSubstitution* subst
) {
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

        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            // ─── Check if this is a generic parameter ──────────────────────
            if (subst) {
                TypeAST* substituted = subst->lookup(named->name);
                if (substituted) {
                    // Recursively get type of the substituted type
                    result = getType(ctx, substituted, subst);
                    break;
                }
                if (subst->isGenericParam(named->name)) {
                    // `named` IS one of this instantiation's generic params,
                    // but typeArgs has no entry for it - an arity mismatch,
                    // not an unresolved user type. Report it directly rather
                    // than falling through to getNamedType(), which would
                    // otherwise just warn and forward-declare a bogus empty
                    // struct for what's actually a generics bug.
                    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, type->loc,
                                            "missing type argument for generic parameter '",
                                            ctx.pool.lookup(named->name), "'");
                    return nullptr;
                }
            }
            // ─── Resolved struct/enum: dispatch directly via resolvedDecl ───
            // Sema (resolveNamedType) already sets named->resolvedDecl to the
            // concrete StructDeclAST/EnumDeclAST/TraitDeclAST this name
            // refers to. Using it here means getStructType/getEnumType are
            // reached directly instead of falling through to
            // getNamedType()'s fragile by-LLVM-name lookup below, which
            // never had a path to getEnumType at all - any enum-typed
            // value previously got a bogus empty forward-declared struct
            // in its place. TraitDeclAST is intentionally not handled yet
            // (see getNamedType) - there's no canonical trait-by-value
            // storage type defined in this file yet.
            if (named->resolvedDecl) {
                if (named->resolvedDecl->isa<StructDeclAST>()) {
                    result = getStructType(ctx, named->resolvedDecl->as<StructDeclAST>());
                    break;
                }
                if (named->resolvedDecl->isa<EnumDeclAST>()) {
                    result = getEnumType(ctx, named->resolvedDecl->as<EnumDeclAST>());
                    break;
                }
            }
            // ─── Otherwise, resolve as a normal named type ──────────────────
            result = getNamedType(ctx, named);
            break;
        }

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

llvm::StructType* getStructType(CodeGenContext& ctx, StructDeclAST* decl) {
    if (!decl) return nullptr;

    auto it = ctx.structCache.find(decl);
    if (it != ctx.structCache.end()) {
        return it->second;
    }

    std::vector<llvm::Type*> fieldTypes;
    std::string structName = ctx.pool.lookup(decl->name);

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = getType(ctx, field->type);
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

llvm::IntegerType* getEnumType(CodeGenContext& ctx, const EnumDeclAST* decl) {
    if (!decl) return nullptr;

    // If a backing type is specified, use it
    if (decl->backingType) {
        return getIntegerType(ctx, decl->backingType->primitiveKind);
    }

    // Default: int32 (matches C enum behavior)
    return llvm::Type::getInt32Ty(ctx.llvmCtx);
}

llvm::FunctionType* getFunctionType(
    CodeGenContext& ctx,
    FuncTypeAST* funcType,
    bool isClosure
) {
    if (!funcType) return nullptr;

    std::vector<llvm::Type*> paramTypes;

    // ─── For closures, add environment pointer as first parameter ──────────
    if (isClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // ─── Add regular parameters ────────────────────────────────────────────
    for (ParamAST* param : funcType->params) {
        llvm::Type* paramType = getType(ctx, param->type);
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

    // ─── Check for variadic parameters ──────────────────────────────────────
    bool isVarArg = false;
    for (ParamAST* param : funcType->params) {
        if (param->isVariadic) {
            isVarArg = true;
            break;
        }
    }

    return llvm::FunctionType::get(returnType, paramTypes, isVarArg);
}

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

llvm::Type* getNamedType(CodeGenContext& ctx, NamedTypeAST* type) {
    if (!type) return nullptr;

    std::string typeName = ctx.pool.lookup(type->name);

    // ─── 1. Try to resolve as a primitive type ──────────────────────────────
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

    // ─── 2. Check if it's a generic type parameter ──────────────────────────
    // If the named type is actually a generic parameter (T in Box<T>),
    // it should have been resolved by Sema. If we're here, it's a
    // user-defined type.

    // ─── 3. Try to find an existing struct type ─────────────────────────────
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName)) {
        return existing;
    }

    // ─── 4. Unknown type - create forward declaration ──────────────────────
    // This typically means the type is defined in another module or
    // we're still in the declaration phase.
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, type->loc,
                              "type '", typeName, "' not yet defined, "
                              "creating forward declaration");

    llvm::StructType* structType = llvm::StructType::create(ctx.llvmCtx, typeName);
    return structType;
}

llvm::Type* getPtrType(CodeGenContext& ctx, PtrTypeAST* type) {
    if (!type) return nullptr;

    // Raw pointers are always opaque pointers
    // We don't need the pointee type for LLVM's opaque pointer model
    (void)type;
    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

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

llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, ModuleTypeAccessAST* type) {
    if (!type) return nullptr;

    std::string moduleName = ctx.pool.lookup(type->moduleName);
    std::string typeName = ctx.pool.lookup(type->typeName);

    // Previously this looked up `typeName` alone, ignoring `moduleName`
    // entirely - two modules defining the same type name (e.g. both
    // declaring `Foo`) would silently collide on whichever struct happened
    // to be registered first under that bare name in the shared
    // LLVMContext. Try a module-qualified name first so same-named types
    // from different modules don't alias each other.
    //
    // NOTE: this qualified name is only used as a local lookup/forward-decl
    // key within this function and does not attempt to replicate whatever
    // canonical mangling scheme generateMangledName() (MangledName.hpp)
    // uses elsewhere - if struct types for cross-module access are meant to
    // be registered under that scheme instead, this should call into it
    // rather than building its own "module.type" key.
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