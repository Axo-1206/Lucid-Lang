/// @file CodeGenType.cpp
/// @brief Implementation of type mapping from Lucid AST types to LLVM types.

#include "CodeGenType.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Module.h>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Type* getType(CodeGenContext& ctx, TypeAST* type) {
    if (!type) return nullptr;

    // ─── Check cache ────────────────────────────────────────────────────────
    auto it = ctx.typeCache.find(type);
    if (it != ctx.typeCache.end()) {
        return it->second;
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
            result = getNamedType(ctx, type->as<NamedTypeAST>());
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
            result = getArrayType(ctx, type->as<ArrayTypeAST>());
            break;

        case ASTKind::FuncType:
            result = getFunctionType(ctx, type->as<FuncTypeAST>(), false);
            break;

        case ASTKind::NullableType:
            result = getNullableType(ctx, type->as<NullableTypeAST>());
            break;

        case ASTKind::FallibleType:
            result = getFallibleType(ctx, type->as<FallibleTypeAST>());
            break;

        case ASTKind::CombinedType:
            result = getCombinedType(ctx, type->as<CombinedTypeAST>());
            break;

        case ASTKind::FutureType:
            result = getFutureType(ctx, type->as<FutureTypeAST>());
            break;

        case ASTKind::ThreadType:
            result = getThreadType(ctx, type->as<ThreadTypeAST>());
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_UnknownType, type->loc,
                                    "unknown type kind in code generation");
            return nullptr;
    }

    // ─── Cache result ──────────────────────────────────────────────────────
    if (result) {
        ctx.typeCache[type] = result;
    }

    return result;
}

// ─── Built-in Type Accessors ─────────────────────────────────────────────

llvm::VectorType* getSimdType(CodeGenContext& ctx, SimdTypeAST* simd) {
    if (!simd) return nullptr;

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

    // Sema ensures the element type is already concrete
    llvm::Type* elemType = getType(ctx, simd->elementType);
    if (!elemType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSimdElementType, simd->loc,
                                "Simd element type has unknown type");
        return nullptr;
    }

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

llvm::Type* getNamedType(CodeGenContext& ctx, NamedTypeAST* named) {
    if (!named) return nullptr;

    // ─── Defensive check for traits ──────────────────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<TraitDeclAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TraitInvalidContext, named->loc,
                                "INTERNAL ERROR: trait '", ctx.pool.lookup(named->name),
                                "' reached CodeGen - Sema should have rejected this");
        return llvm::StructType::create(ctx.llvmCtx, 
            ctx.pool.lookup(named->name) + "__trait_placeholder");
    }

    std::string typeName = ctx.pool.lookup(named->name);

    // ─── 1. Resolve struct/enum via resolvedDecl ──────────────────────────
    // Sema has already resolved every type. If resolvedDecl points to a
    // generic StructDeclAST, it means Sema chose the @[erased] path and
    // kept the template — CodeGen will generate the erased version.
    if (named->resolvedDecl) {
        if (named->resolvedDecl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = named->resolvedDecl->as<StructDeclAST>();
            
            // Under the specialization-only design, CodeGen receives
            // concrete StructDeclAST nodes after Sema resolves the type.
            // Generic instantiation is no longer a CodeGen responsibility.
            return getStructType(ctx, structDecl);
        }
        if (named->resolvedDecl->isa<EnumDeclAST>()) {
            return getEnumType(ctx, named->resolvedDecl->as<EnumDeclAST>());
        }
    }

    // ─── 2. Try to resolve as a primitive type ──────────────────────────────
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

    // ─── 3. Try to find an existing struct type ─────────────────────────────
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName)) {
        return existing;
    }

    // ─── 4. Unknown type - create forward declaration ──────────────────────
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, named->loc,
                              "type '", typeName, "' not yet defined, "
                              "creating forward declaration");

    return llvm::StructType::create(ctx.llvmCtx, typeName);
}

// ─── Struct Type ──────────────────────────────────────────────────────────

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
    if (!structType) {
        structType = llvm::StructType::create(ctx.llvmCtx, structName);
    }

    // ─── 5. Cache the opaque type BEFORE building fields ──────────────────
    ctx.cacheStruct(decl, structType);

    // ─── 6. Build field types ──────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.reserve(decl->fields.size());

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = nullptr;
        
        if (field->type && field->type->isa<FuncTypeAST>()) {
            fieldType = getFunctionRuntimeType(
                ctx,
                field->type->as<FuncTypeAST>(),
                true
            );
        } else {
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

    if (decl->backingType) {
        return getIntegerType(ctx, decl->backingType->primitiveKind);
    }

    return llvm::Type::getInt32Ty(ctx.llvmCtx);
}

// ─── Function Type ────────────────────────────────────────────────────────

llvm::FunctionType* getFunctionType(CodeGenContext& ctx, FuncTypeAST* funcType, bool isClosure) {
    if (!funcType) return nullptr;

    std::vector<llvm::Type*> paramTypes;

    if (isClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

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

    llvm::Type* returnType = nullptr;
    if (funcType->returnType) {
        if (funcType->returnType->isa<FuncTypeAST>()) {
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

    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

// ─── Array Type ──────────────────────────────────────────────────────────

llvm::Type* getArrayType(CodeGenContext& ctx, ArrayTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* elemType = getType(ctx, type->element);
    if (!elemType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, type->loc,
                                "array element type has unknown type");
        return nullptr;
    }

    switch (type->arrayKind) {
        case ArrayKind::Fixed:
            return llvm::ArrayType::get(elemType, type->size);

        case ArrayKind::Dynamic:
            return llvm::PointerType::get(ctx.llvmCtx, 0);

        case ArrayKind::Slice:
            {
                llvm::Type* ptrType = llvm::PointerType::get(ctx.llvmCtx, 0);
                llvm::Type* lenType = llvm::Type::getInt64Ty(ctx.llvmCtx);
                std::string typeName = "slice_" + getTypeName(ctx, type->element);
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

llvm::StructType* getNullableType(CodeGenContext& ctx, NullableTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "nullable inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "nullable_" + getTypeName(ctx, type->inner);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Fallible Type ───────────────────────────────────────────────────────

llvm::StructType* getFallibleType(CodeGenContext& ctx, FallibleTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "fallible inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "fallible_" + getTypeName(ctx, type->inner);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Combined Type ───────────────────────────────────────────────────────

llvm::StructType* getCombinedType(CodeGenContext& ctx, CombinedTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "combined inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "combined_" + getTypeName(ctx, type->inner);
    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{tagType, innerType},
        typeName
    );
}

// ─── Future Type ─────────────────────────────────────────────────────────

llvm::StructType* getFutureType(CodeGenContext& ctx, FutureTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "future inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "future_" + getTypeName(ctx, type->inner);
    llvm::Type* stateType = llvm::Type::getInt8Ty(ctx.llvmCtx);

    return llvm::StructType::create(
        ctx.llvmCtx,
        llvm::ArrayRef<llvm::Type*>{innerType, stateType},
        typeName
    );
}

// ─── Thread Type ─────────────────────────────────────────────────────────

llvm::StructType* getThreadType(CodeGenContext& ctx, const ThreadTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "thread inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    std::string typeName = "thread_" + getTypeName(ctx, type->inner);
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

    ModuleAST* targetModule = nullptr;
    if (ctx.currentModule) {
        auto it = ctx.currentModule->resolvedImports.find(type->moduleName);
        if (it != ctx.currentModule->resolvedImports.end()) {
            targetModule = it->second;
        }
    }

    if (targetModule) {
        for (DeclAST* decl : targetModule->decls) {
            if (decl->isa<TypeDeclAST>()) {
                TypeDeclAST* typeDecl = decl->as<TypeDeclAST>();
                if (typeDecl->name == type->typeName) {
                    
                    if (typeDecl->isa<StructDeclAST>()) {
                        StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
                        
                        // Generic instantiation happens before CodeGen sees this
                        // declaration; the lowered type is simply the concrete
                        // struct type for the resolved declaration.
                        return getStructType(ctx, structDecl);
                    }
                    
                    if (typeDecl->isa<EnumDeclAST>()) {
                        return getEnumType(ctx, typeDecl->as<EnumDeclAST>());
                    }
                }
            }
        }
    }

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

// ─── getTypeName — CodeGen-local, no Sema dependency ─────────────────────
// 
// Produces readable names for LLVM struct types (nullable_int, slice_Point,
// etc.). This is NOT the same as Sema's mangled name — that's for linker
// symbols. This is only for LLVM type identification and debugging.

std::string getTypeName(CodeGenContext& ctx, TypeAST* type) {
    if (!type) return "void";
    
    // ─── Primitives: simple name ────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
        switch (prim->primitiveKind) {
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
    
    // ─── Simd: Simd_<elem>_<lanes> ─────────────────────────────────────
    if (type->isa<SimdTypeAST>()) {
        SimdTypeAST* simd = type->as<SimdTypeAST>();
        return "Simd_" + getTypeName(ctx, simd->elementType) + "_" + std::to_string(simd->laneCount);
    }
    
    // ─── Built-in types ────────────────────────────────────────────────
    if (type->isa<ArenaTypeAST>()) {
        return "Arena";
    }
    if (type->isa<ArenaDescriptorTypeAST>()) {
        return "ArenaDescriptor";
    }
    
    // ─── Named type: use the source name ───────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        return ctx.pool.lookup(named->name);
    }
    
    // ─── Structs, arrays, pointers, etc.: use AST kind name ────────────
    // Fallback: use the AST kind name
    return astKindToString(type->kind);
}

uint64_t getTypeSize(CodeGenContext& ctx, TypeAST* type) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return 0;

    if (llvmType->isSized()) {
        return ctx.module->getDataLayout().getTypeAllocSize(llvmType).getFixedValue();
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