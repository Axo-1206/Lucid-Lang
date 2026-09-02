/// @file CodeGenContext.cpp
/// @brief Implementation of CodeGenContext methods

#include "CodeGenContext.hpp"
#include "../intrinsic/LucidIntrinsicEmitter.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/GlobalVariable.h>

namespace codegen {

// ─── Runtime Function Helpers ─────────────────────────────────────────────

llvm::Function* CodeGenContext::getOrCreateRuntimeFunction(
    const std::string& name,
    llvm::FunctionType* type
) {
    llvm::Function* func = getRuntimeFunction(name);
    if (func) return func;

    func = llvm::Function::Create(
        type,
        llvm::Function::ExternalLinkage,
        name,
        module
    );
    setRuntimeFunction(name, func);
    return func;
}

llvm::Function* CodeGenContext::getRuntimeFn(RuntimeFn fn) {
    const RuntimeFunctionInfo& info = getRuntimeFunctionInfo(fn);
    std::string name(info.name);
    
    llvm::Function* func = getRuntimeFunction(name);
    if (func) return func;
    
    llvm::FunctionType* type = info.buildType(*this);
    
    func = llvm::Function::Create(
        type,
        llvm::Function::ExternalLinkage,
        name,
        module
    );
    
    setRuntimeFunction(name, func);
    return func;
}

llvm::Function* CodeGenContext::getOrInsertFunction(
    const std::string& name,
    llvm::FunctionType* type
) {
    llvm::FunctionCallee callee = module->getOrInsertFunction(name, type);
    return llvm::dyn_cast<llvm::Function>(callee.getCallee());
}

// ─── Live Variable Helpers ────────────────────────────────────────────────

void CodeGenContext::emitCleanupForTracker(LiveVariableTracker& tracker) {
    if (!getCurrentFunction()) return;

    // ─── Phase 1: user #scope_exit callbacks ──────────────────────────
    if (tracker.block) {
        for (size_t i = tracker.block->scopeExits.size(); i > 0; --i) {
            const ScopeExitRegistration* reg = tracker.block->scopeExits[i - 1];
            emitScopeExitCallback(reg, *this);
        }
    }

    // ─── Phase 2: implicit cleanup ──────────────────────────────────────
    llvm::Function* releaseFn = getRuntimeFn(RuntimeFn::ReleaseEnv);
    llvm::Function* freeFn = getRuntimeFn(RuntimeFn::Free);
    std::vector<ValueDeclAST*> declarations = tracker.getAliveVariables();

    auto loadValue = [&](llvm::Value* val) -> llvm::Value* {
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
            return builder.CreateLoad(alloca->getAllocatedType(), alloca, "cleanup_load");
        }
        return val;
    };

    for (ValueDeclAST* decl : declarations) {
        llvm::Value* binding = lookupValue(decl);
        if (!binding || !decl->type) continue;

        llvm::Value* value = loadValue(binding);

        // ─── 2a. CLOSURE ENVIRONMENTS (FuncTypeAST) ──────────────────────
        if (decl->type->isa<FuncTypeAST>()) {
            if (value->getType()->isStructTy() &&
                value->getType()->getStructNumElements() == 2) {
                llvm::Value* envPtr = builder.CreateExtractValue(value, 1, "closure_env");
                llvm::Value* isNull = builder.CreateIsNull(envPtr, "env_is_null");

                llvm::Function* func = getCurrentFunction();
                llvm::BasicBlock* releaseBlock = llvm::BasicBlock::Create(
                    llvmCtx, "release_env", func);
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    llvmCtx, "release_continue", func);

                builder.CreateCondBr(isNull, continueBlock, releaseBlock);
                builder.SetInsertPoint(releaseBlock);
                builder.CreateCall(releaseFn, {envPtr});
                builder.CreateBr(continueBlock);
                builder.SetInsertPoint(continueBlock);

                tracker.markConsumed(decl);
            }
            continue;
        }

        // ─── 2b. DYNAMIC ARRAYS [*]T ──────────────────────────────────────
        if (decl->type->isa<ArrayTypeAST>()) {
            ArrayTypeAST* arrayType = decl->type->as<ArrayTypeAST>();
            if (arrayType->isDynamic()) {
                if (value->getType()->isStructTy() &&
                    value->getType()->getStructNumElements() == 3) {
                    llvm::Value* dataPtr = builder.CreateExtractValue(value, 0, "array_data");
                    llvm::Value* isNull = builder.CreateIsNull(dataPtr, "array_is_null");

                    llvm::Function* func = getCurrentFunction();
                    llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(
                        llvmCtx, "free_array", func);
                    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                        llvmCtx, "free_array_continue", func);

                    builder.CreateCondBr(isNull, continueBlock, freeBlock);
                    builder.SetInsertPoint(freeBlock);
                    builder.CreateCall(freeFn, {dataPtr});
                    builder.CreateBr(continueBlock);
                    builder.SetInsertPoint(continueBlock);

                    tracker.markConsumed(decl);
                }
            }
            continue;
        }

        // ─── 2c. STRINGS ──────────────────────────────────────────────────
        if (decl->type->isa<PrimitiveTypeAST>()) {
            PrimitiveTypeAST* primType = decl->type->as<PrimitiveTypeAST>();
            if (primType->primitiveKind == PrimitiveKind::String) {
                if (value->getType()->isStructTy() &&
                    value->getType()->getStructNumElements() == 3) {
                    llvm::Value* dataPtr = builder.CreateExtractValue(value, 0, "string_data");

                    bool isStaticString = false;
                    if (llvm::Constant* constPtr = llvm::dyn_cast<llvm::Constant>(dataPtr)) {
                        if (llvm::isa<llvm::GlobalVariable>(constPtr)) {
                            isStaticString = true;
                        }
                    }

                    if (isStaticString) {
                        tracker.markConsumed(decl);
                        continue;
                    }

                    llvm::Value* isNull = builder.CreateIsNull(dataPtr, "string_is_null");
                    llvm::Function* func = getCurrentFunction();
                    llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(
                        llvmCtx, "free_string", func);
                    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                        llvmCtx, "free_string_continue", func);

                    builder.CreateCondBr(isNull, continueBlock, freeBlock);
                    builder.SetInsertPoint(freeBlock);
                    builder.CreateCall(freeFn, {dataPtr});
                    builder.CreateBr(continueBlock);
                    builder.SetInsertPoint(continueBlock);

                    tracker.markConsumed(decl);
                }
            }
            continue;
        }
    }
}

// ─── String Literal Helper ───────────────────────────────────────────────

llvm::Value* CodeGenContext::createStringLiteral(const std::string& str) {
    llvm::Constant* strConst = llvm::ConstantDataArray::getString(llvmCtx, str);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        *module,
        strConst->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        strConst
    );

    llvm::Type* strType = getStringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(llvmCtx, 0);

    llvm::Value* ptr = builder.CreateBitCast(global, i8Ptr);
    llvm::Value* len = llvm::ConstantInt::get(i64, str.length());

    llvm::Value* result = llvm::UndefValue::get(strType);
    result = builder.CreateInsertValue(result, ptr, 0);
    result = builder.CreateInsertValue(result, len, 1);
    result = builder.CreateInsertValue(result, len, 2);
    return result;
}

// ─── Intrinsic Helpers ─────────────────────────────────────────────────────

llvm::Function* CodeGenContext::getLLVMIntrinsicDecl(
    llvm::Intrinsic::ID id,
    llvm::ArrayRef<llvm::Type*> argTypes
) {
    return llvm::Intrinsic::getDeclaration(module, id, argTypes);
}

// ─── Pointee Type Helpers ─────────────────────────────────────────────────

llvm::Type* CodeGenContext::getPointeeType(llvm::Value* ptr) const {
    (void)ptr;
    return llvm::Type::getInt8Ty(llvmCtx);
}

llvm::Type* CodeGenContext::getPointeeType(llvm::Type* type) const {
    (void)type;
    return llvm::Type::getInt8Ty(llvmCtx);
}

} // namespace codegen