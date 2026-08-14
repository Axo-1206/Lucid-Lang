/// @file CodeGenGeneric.cpp
/// @brief Implementation of generic instantiation.

#include "CodeGenGeneric.hpp"
#include "../CodeGenType.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "GenericMangledName.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Detection Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool isGenericFunction(FuncDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool isGenericStruct(StructDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool shouldSpecialize(DeclAST* decl) {
    if (!decl) return false;
    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->shouldSpecialize;
    }
    if (decl->isa<StructDeclAST>()) {
        return decl->as<StructDeclAST>()->shouldSpecialize;
    }
    return false;
}

bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams) {
    for (GenericParamDeclAST* param : genericParams) {
        if (param->name == name) return true;
    }
    return false;
}

size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams) {
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i]->name == name) return i;
    }
    return SIZE_MAX;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    // ... (implementation unchanged, uses GenericMangledName)
}

llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    // ... (implementation unchanged)
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
) {
    if (!funcDecl) return nullptr;

    std::string funcName = ctx.pool.lookup(funcDecl->name);
    std::string mangledName = funcName + "__erased";

    std::vector<llvm::Type*> paramTypes;

    if (funcDecl->hasClosure) {
        paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    FuncTypeAST* funcType = funcDecl->funcType;
    while (funcType) {
        for (size_t i = 0; i < funcType->params.size(); ++i) {
            paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0));
        }
        funcType = funcType->getNext();
    }

    llvm::Type* returnType = llvm::PointerType::get(ctx.llvmCtx, 0);

    llvm::FunctionType* llvmFuncType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    llvm::Function* existingFunc = ctx.module->getFunction(mangledName);
    if (existingFunc) {
        return existingFunc;
    }

    llvm::Function* func = llvm::Function::Create(
        llvmFuncType,
        llvm::Function::ExternalLinkage,
        mangledName,
        ctx.module
    );

    size_t paramIndex = 0;
    if (funcDecl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }

    FuncTypeAST* paramTypeIter = funcDecl->funcType;
    while (paramTypeIter) {
        for (ParamAST* param : paramTypeIter->params) {
            if (paramIndex < func->arg_size()) {
                std::string paramName = ctx.pool.lookup(param->name);
                func->getArg(paramIndex)->setName(paramName + "_tagged");
                paramIndex++;
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    LOG_CODEGEN("Created type-erased generic function: ", mangledName,
                " (", paramTypes.size(), " params)");

    return func;
}

llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
) {
    if (!structDecl) return nullptr;

    std::string structName = ctx.pool.lookup(structDecl->name);
    std::string mangledName = structName + "__erased";

    // ─── Get or create the canonical TaggedSlot type ──────────────────────
    // This is a SHARED type across all erased generic structs.
    // DO NOT recreate it per struct - that creates distinct types
    // that are structurally identical but nominally different.
    static const char* slotName = "TaggedSlot";
    llvm::StructType* slotType = llvm::StructType::getTypeByName(ctx.llvmCtx, slotName);
    if (!slotType) {
        std::vector<llvm::Type*> slotFields = {
            llvm::Type::getInt8Ty(ctx.llvmCtx),              // tag (0 = valid, 1 = nil, 2 = err)
            llvm::PointerType::get(ctx.llvmCtx, 0)          // value (opaque pointer)
        };
        slotType = llvm::StructType::create(ctx.llvmCtx, slotFields, slotName);
    }

    // ─── Check if this erased struct already exists ──────────────────────
    llvm::StructType* existingType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        mangledName
    );
    if (existingType && !existingType->isOpaque()) {
        return existingType;
    }

    // ─── Build field types using the shared TaggedSlot ───────────────────
    std::vector<llvm::Type*> fieldTypes;
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldTypes.push_back(slotType);
    }

    // ─── Create the erased struct type ───────────────────────────────────
    llvm::StructType* structType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        mangledName
    );

    LOG_CODEGEN("Created type-erased generic struct: ", mangledName,
                " (", fieldTypes.size(), " fields)");

    return structType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!funcDecl || !isGenericFunction(funcDecl)) return nullptr;

    if (!shouldSpecialize(funcDecl)) {
        return generateErasedGenericFunction(funcDecl, ctx);
    }

    GenericInstantiationKey key{funcDecl, typeArgs};

    auto funcIt = ctx.genericRegistry.functionInstantiations.find(funcDecl);
    if (funcIt != ctx.genericRegistry.functionInstantiations.end()) {
        auto typeIt = funcIt->second.find(key);
        if (typeIt != funcIt->second.end()) {
            return typeIt->second;
        }
    }

    llvm::Function* specialized = createSpecializedFunction(funcDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    ctx.genericRegistry.functionInstantiations[funcDecl][key] = specialized;

    return specialized;
}

llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!structDecl || !isGenericStruct(structDecl)) return nullptr;

    if (!shouldSpecialize(structDecl)) {
        return generateErasedGenericStruct(structDecl, ctx);
    }

    GenericInstantiationKey key{structDecl, typeArgs};

    auto structIt = ctx.genericRegistry.structInstantiations.find(structDecl);
    if (structIt != ctx.genericRegistry.structInstantiations.end()) {
        auto typeIt = structIt->second.find(key);
        if (typeIt != structIt->second.end()) {
            return typeIt->second;
        }
    }

    llvm::Type* specialized = createSpecializedStruct(structDecl, typeArgs, ctx);
    if (!specialized) return nullptr;

    ctx.genericRegistry.structInstantiations[structDecl][key] = specialized;

    return specialized;
}

} // namespace codegen