/// @file CodeGenType.cpp
/// @brief Implementation of type mapping from Lucid AST types to LLVM types.

#include "CodeGenType.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Module.h>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Type* getType(CodeGenContext& ctx, const TypeAST* type) {
    if (!type) return nullptr;

    // Check cache first
    auto it = ctx.typeCache.find(type);
    if (it != ctx.typeCache.end()) {
        return it->second;
    }

    llvm::Type* result = nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            result = getPrimitiveType(ctx, type->as<PrimitiveTypeAST>());
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
            result = getFunctionType(ctx, type->as<FuncTypeAST>());
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
        default:
            return nullptr;
    }

    if (result) {
        ctx.typeCache[type] = result;
    }

    return result;
}

llvm::StructType* getStructType(CodeGenContext& ctx, const StructDeclAST* decl) {
    if (!decl) return nullptr;

    auto it = ctx.structCache.find(decl);
    if (it != ctx.structCache.end()) {
        return it->second;
    }

    std::vector<llvm::Type*> fieldTypes;
    std::string structName = ctx.pool.lookup(decl->name);

    for (const FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = getType(ctx, field->type);
        if (!fieldType) {
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, field->loc,
                                      "field '", ctx.pool.lookup(field->name),
                                      "' has unknown type, using placeholder");
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

llvm::FunctionType* getFunctionType(
    CodeGenContext& ctx, 
    const FuncTypeAST* funcType,
    bool isClosure = false  // Set to true for inner closure functions
) {
    if (!funcType) return nullptr;

    std::vector<llvm::Type*> paramTypes;

    // ─── For closures, add environment pointer as first parameter ──────
    if (isClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // ─── Add regular parameters ─────────────────────────────────────────
    for (const ParamAST* param : funcType->params) {
        llvm::Type* paramType = getType(ctx, param->type);
        if (!paramType) return nullptr;
        paramTypes.push_back(paramType);
    }

    // ─── Get return type (may be another function type) ────────────────
    llvm::Type* returnType = nullptr;
    if (funcType->returnType) {
        if (funcType->returnType->isa<FuncTypeAST>()) {
            // Curried return: pointer to inner function type
            llvm::FunctionType* innerType = getFunctionType(
                ctx, 
                funcType->returnType->as<FuncTypeAST>(),
                false  // Inner function type, not a closure yet
            );
            returnType = llvm::PointerType::get(innerType, 0);
        } else {
            returnType = getType(ctx, funcType->returnType);
        }
    } else {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    bool isVarArg = /* check variadic */ false;

    return llvm::FunctionType::get(returnType, paramTypes, isVarArg);
}

llvm::Type* getPrimitiveType(CodeGenContext& ctx, const PrimitiveTypeAST* type) {
    if (!type) return nullptr;

    switch (type->primitiveKind) {
        case PrimitiveKind::Bool:
            return llvm::Type::getInt1Ty(ctx.llvmCtx);
        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
            return llvm::Type::getInt8Ty(ctx.llvmCtx);
        case PrimitiveKind::Int16:
        case PrimitiveKind::Short:
            return llvm::Type::getInt16Ty(ctx.llvmCtx);
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int:
            return llvm::Type::getInt32Ty(ctx.llvmCtx);
        case PrimitiveKind::Int64:
        case PrimitiveKind::Long:
            return llvm::Type::getInt64Ty(ctx.llvmCtx);
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
            return llvm::Type::getInt8Ty(ctx.llvmCtx);
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Ushort:
            return llvm::Type::getInt16Ty(ctx.llvmCtx);
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint:
            return llvm::Type::getInt32Ty(ctx.llvmCtx);
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

llvm::Type* getNamedType(CodeGenContext& ctx, const NamedTypeAST* type) {
    if (!type) return nullptr;

    std::string typeName = ctx.pool.lookup(type->name);
    
    // Check for built-in primitive types
    if (typeName == "int" || typeName == "int32" || typeName == "uint32") {
        return llvm::Type::getInt32Ty(ctx.llvmCtx);
    }
    if (typeName == "int64" || typeName == "uint64") {
        return llvm::Type::getInt64Ty(ctx.llvmCtx);
    }
    if (typeName == "int16" || typeName == "uint16") {
        return llvm::Type::getInt16Ty(ctx.llvmCtx);
    }
    if (typeName == "int8" || typeName == "uint8" || typeName == "byte") {
        return llvm::Type::getInt8Ty(ctx.llvmCtx);
    }
    if (typeName == "float") {
        return llvm::Type::getFloatTy(ctx.llvmCtx);
    }
    if (typeName == "double") {
        return llvm::Type::getDoubleTy(ctx.llvmCtx);
    }
    if (typeName == "bool") {
        return llvm::Type::getInt1Ty(ctx.llvmCtx);
    }
    if (typeName == "string") {
        return llvm::PointerType::get(ctx.llvmCtx, 0);
    }
    if (typeName == "char") {
        return llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    // Query by name using StructType helper (works across LLVM versions)
    if (llvm::StructType* existing = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName)) {
        return existing;
    }

    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        typeName
    );

    return llvm::PointerType::get(structType, 0);
}

llvm::Type* getPtrType(CodeGenContext& ctx, const PtrTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, type->loc,
                                  "pointer target type has unknown type, using i8*");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

llvm::Type* getRefType(CodeGenContext& ctx, const RefTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidPointerTarget, type->loc,
                                "reference target type has unknown type");
        return llvm::PointerType::get(ctx.llvmCtx, 0);
    }

    return llvm::PointerType::get(innerType, 0);
}

llvm::Type* getArrayType(CodeGenContext& ctx, const ArrayTypeAST* type) {
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
            return llvm::PointerType::get(elemType, 0);
        case ArrayKind::Slice:
            {
                llvm::Type* ptrType = llvm::PointerType::get(elemType, 0);
                llvm::Type* lenType = llvm::Type::getInt64Ty(ctx.llvmCtx);
                return llvm::StructType::create(
                    ctx.llvmCtx,
                    llvm::ArrayRef<llvm::Type*>{ptrType, lenType, lenType},
                    "slice"
                );
            }
        default:
            return nullptr;
    }
}

llvm::StructType* getNullableType(CodeGenContext& ctx, const NullableTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "nullable inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    std::string typeName = "nullable_" + debug::typeToString(type->inner, ctx.pool);

    return llvm::StructType::create(ctx.llvmCtx, llvm::ArrayRef<llvm::Type*>{tagType, innerType}, typeName);
}

llvm::StructType* getFallibleType(CodeGenContext& ctx, const FallibleTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "fallible inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    std::string typeName = "fallible_" + debug::typeToString(type->inner, ctx.pool);

    return llvm::StructType::create(ctx.llvmCtx, llvm::ArrayRef<llvm::Type*>{tagType, innerType}, typeName);
}

llvm::StructType* getCombinedType(CodeGenContext& ctx, const CombinedTypeAST* type) {
    if (!type) return nullptr;

    llvm::Type* innerType = getType(ctx, type->inner);
    if (!innerType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, type->loc,
                                "combined inner type has unknown type");
        innerType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    }

    llvm::Type* tagType = llvm::Type::getInt8Ty(ctx.llvmCtx);
    std::string typeName = "combined_" + debug::typeToString(type->inner, ctx.pool);

    return llvm::StructType::create(ctx.llvmCtx, llvm::ArrayRef<llvm::Type*>{tagType, innerType}, typeName);
}

llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, const ModuleTypeAccessAST* type) {
    if (!type) return nullptr;

    std::string moduleName = ctx.pool.lookup(type->moduleName);
    std::string typeName = ctx.pool.lookup(type->typeName);
    
    // Use getTypeByName on the LLVM context since Module no longer exposes it.
    llvm::StructType* structType = llvm::StructType::getTypeByName(ctx.llvmCtx, typeName);
    if (!structType) {
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, type->loc,
                                  "module type '", moduleName, ":", typeName,
                                  "' not found, creating forward declaration");
        structType = llvm::StructType::create(ctx.llvmCtx, typeName);
    }

    return llvm::PointerType::get(structType, 0);
}

// ─── Helper Functions ─────────────────────────────────────────────────────

llvm::IntegerType* getIntegerType(CodeGenContext& ctx, PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Byte:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Char:
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
        default:
            return llvm::Type::getInt32Ty(ctx.llvmCtx);
    }
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

uint64_t getTypeSize(CodeGenContext& ctx, const TypeAST* type) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return 0;

    if (llvmType->isSized()) {
        return ctx.module->getDataLayout().getTypeAllocSize(llvmType);
    }

    return 0;
}

uint64_t getTypeAlign(CodeGenContext& ctx, const TypeAST* type) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return 0;

    if (llvmType->isSized()) {
        return ctx.module->getDataLayout().getABITypeAlign(llvmType).value();
    }

    return 0;
}

} // namespace codegen