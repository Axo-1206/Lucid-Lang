/**
 * @file IRLoweringBuilder.cpp
 * @brief Helper utilities for common IR construction patterns.
 * 
 * @responsibility Provides reusable helper functions for building common
 *                 LLVM IR patterns used across multiple lowering modules.
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Declarations for these helpers
 *   - src/codegen/IRLoweringDecl.cpp - Uses helpers for declarations
 *   - src/codegen/IRLoweringStmt.cpp - Uses helpers for statements
 *   - src/codegen/IRLoweringExpr.cpp - Uses helpers for expressions
 */

#include "IRLowering.hpp"
#include "IRLoweringBuilder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Allocation
// ─────────────────────────────────────────────────────────────────────────────

llvm::AllocaInst* IRBuilderHelper::createAlloca(IRLowering& lowerer, 
                                                 llvm::Type* type, 
                                                 const std::string& name) {
    if (!type) {
        return nullptr;
    }
    
    auto* function = lowerer.currentFunction().function;
    
    if (!function) {
        throw IRLoweringError(IRLoweringError::Kind::FunctionNotFound,
                              "Cannot create alloca outside a function");
    }
    
    // Insert at the start of the function for better optimization
    llvm::IRBuilder<> tmpBuilder(&function->getEntryBlock(),
                                 function->getEntryBlock().begin());
    
    return tmpBuilder.CreateAlloca(type, nullptr, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Load/Store
// ─────────────────────────────────────────────────────────────────────────────

llvm::LoadInst* IRBuilderHelper::createLoad(IRLowering& lowerer,
                                            llvm::Value* ptr,
                                            const std::string& name) {
    if (!ptr || !ptr->getType()->isPointerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    auto* elemType = ptr->getType()->getNonOpaquePointerElementType();
    
    if (!elemType) {
        // For opaque pointers, use the type from the pointer
        return builder.CreateLoad(llvm::Type::getInt8Ty(lowerer.getContext()), ptr, name);
    }
    
    return builder.CreateLoad(elemType, ptr, name);
}

llvm::StoreInst* IRBuilderHelper::createStore(IRLowering& lowerer,
                                              llvm::Value* value,
                                              llvm::Value* ptr) {
    if (!value || !ptr || !ptr->getType()->isPointerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateStore(value, ptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - GetElementPointer
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createGEP(IRLowering& lowerer,
                                        llvm::Type* elemType,
                                        llvm::Value* ptr,
                                        llvm::Value* index,
                                        const std::string& name) {
    if (!elemType || !ptr || !index) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateGEP(elemType, ptr, index, name);
}

llvm::Value* IRBuilderHelper::createStructGEP(IRLowering& lowerer,
                                              llvm::StructType* structType,
                                              llvm::Value* ptr,
                                              unsigned index,
                                              const std::string& name) {
    if (!structType || !ptr || !ptr->getType()->isPointerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    std::vector<llvm::Value*> indices = {
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 0),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(lowerer.getContext()), index)
    };
    
    return builder.CreateGEP(structType, ptr, indices, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Pointer Conversion
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createPtrToInt(IRLowering& lowerer,
                                             llvm::Value* ptr,
                                             const std::string& name) {
    if (!ptr) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    auto* intType = llvm::Type::getInt64Ty(lowerer.getContext());
    return builder.CreatePtrToInt(ptr, intType, name);
}

llvm::Value* IRBuilderHelper::createIntToPtr(IRLowering& lowerer,
                                             llvm::Value* intVal,
                                             llvm::Type* ptrType,
                                             const std::string& name) {
    if (!intVal || !ptrType || !ptrType->isPointerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateIntToPtr(intVal, ptrType, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Branching
// ─────────────────────────────────────────────────────────────────────────────

void IRBuilderHelper::createConditionalBranch(IRLowering& lowerer,
                                              llvm::Value* cond,
                                              llvm::BasicBlock* trueBlock,
                                              llvm::BasicBlock* falseBlock) {
    if (!cond || !trueBlock || !falseBlock) {
        return;
    }
    
    // Ensure condition is boolean
    auto& builder = lowerer.getBuilder();
    if (!cond->getType()->isIntegerTy(1)) {
        cond = builder.CreateICmpNE(
            cond,
            llvm::ConstantInt::get(cond->getType(), 0),
            "cond"
        );
    }
    
    builder.CreateCondBr(cond, trueBlock, falseBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Block Management
// ─────────────────────────────────────────────────────────────────────────────

llvm::BasicBlock* IRBuilderHelper::createBlock(IRLowering& lowerer,
                                               llvm::Function* fn,
                                               const std::string& name) {
    if (!fn) {
        return nullptr;
    }
    
    return llvm::BasicBlock::Create(lowerer.getContext(), name, fn);
}

void IRBuilderHelper::setInsertPoint(IRLowering& lowerer,
                                     llvm::BasicBlock* block) {
    if (block) {
        lowerer.getBuilder().SetInsertPoint(block);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Comparison
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createICmp(IRLowering& lowerer,
                                         llvm::CmpInst::Predicate pred,
                                         llvm::Value* lhs,
                                         llvm::Value* rhs,
                                         const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateICmp(pred, lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createFCmp(IRLowering& lowerer,
                                         llvm::CmpInst::Predicate pred,
                                         llvm::Value* lhs,
                                         llvm::Value* rhs,
                                         const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateFCmp(pred, lhs, rhs, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Arithmetic
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createAdd(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (lhs->getType()->isFloatingPointTy()) {
        return builder.CreateFAdd(lhs, rhs, name);
    }
    
    return builder.CreateAdd(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createSub(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (lhs->getType()->isFloatingPointTy()) {
        return builder.CreateFSub(lhs, rhs, name);
    }
    
    return builder.CreateSub(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createMul(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (lhs->getType()->isFloatingPointTy()) {
        return builder.CreateFMul(lhs, rhs, name);
    }
    
    return builder.CreateMul(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createDiv(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (lhs->getType()->isFloatingPointTy()) {
        return builder.CreateFDiv(lhs, rhs, name);
    }
    
    // Check if signed by looking at the type
    if (lhs->getType()->isIntegerTy()) {
        // Use SDiv for signed integers, UDiv for unsigned
        // Default to SDiv
        return builder.CreateSDiv(lhs, rhs, name);
    }
    
    return builder.CreateUDiv(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createRem(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (lhs->getType()->isFloatingPointTy()) {
        return builder.CreateFRem(lhs, rhs, name);
    }
    
    if (lhs->getType()->isIntegerTy()) {
        // Use SRem for signed integers, URem for unsigned
        // Default to SRem
        return builder.CreateSRem(lhs, rhs, name);
    }
    
    return builder.CreateURem(lhs, rhs, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Bitwise
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createAnd(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateAnd(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createOr(IRLowering& lowerer,
                                       llvm::Value* lhs,
                                       llvm::Value* rhs,
                                       const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateOr(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createXor(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateXor(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createShl(IRLowering& lowerer,
                                        llvm::Value* lhs,
                                        llvm::Value* rhs,
                                        const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateShl(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createLShr(IRLowering& lowerer,
                                         llvm::Value* lhs,
                                         llvm::Value* rhs,
                                         const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateLShr(lhs, rhs, name);
}

llvm::Value* IRBuilderHelper::createAShr(IRLowering& lowerer,
                                         llvm::Value* lhs,
                                         llvm::Value* rhs,
                                         const std::string& name) {
    if (!lhs || !rhs || !lhs->getType()->isIntegerTy()) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateAShr(lhs, rhs, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Null Checks
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createIsNull(IRLowering& lowerer,
                                           llvm::Value* value,
                                           const std::string& name) {
    if (!value) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (value->getType()->isPointerTy()) {
        auto* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(value->getType())
        );
        return builder.CreateICmpEQ(value, nullPtr, name);
    }
    
    // For non-pointer types, check against zero
    auto* zero = llvm::ConstantInt::get(value->getType(), 0);
    return builder.CreateICmpEQ(value, zero, name);
}

llvm::Value* IRBuilderHelper::createIsNotNull(IRLowering& lowerer,
                                              llvm::Value* value,
                                              const std::string& name) {
    if (!value) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    
    if (value->getType()->isPointerTy()) {
        auto* nullPtr = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(value->getType())
        );
        return builder.CreateICmpNE(value, nullPtr, name);
    }
    
    // For non-pointer types, check against zero
    auto* zero = llvm::ConstantInt::get(value->getType(), 0);
    return builder.CreateICmpNE(value, zero, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Select
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRBuilderHelper::createSelect(IRLowering& lowerer,
                                           llvm::Value* cond,
                                           llvm::Value* trueVal,
                                           llvm::Value* falseVal,
                                           const std::string& name) {
    if (!cond || !trueVal || !falseVal) {
        return nullptr;
    }
    
    auto& builder = lowerer.getBuilder();
    return builder.CreateSelect(cond, trueVal, falseVal, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Type Utilities
// ─────────────────────────────────────────────────────────────────────────────

bool IRBuilderHelper::isIntegerType(llvm::Type* type) {
    return type && type->isIntegerTy();
}

bool IRBuilderHelper::isFloatingPointType(llvm::Type* type) {
    return type && type->isFloatingPointTy();
}

bool IRBuilderHelper::isPointerType(llvm::Type* type) {
    return type && type->isPointerTy();
}

bool IRBuilderHelper::isStructType(llvm::Type* type) {
    return type && type->isStructTy();
}

bool IRBuilderHelper::isArrayType(llvm::Type* type) {
    return type && type->isArrayTy();
}

bool IRBuilderHelper::isVoidType(llvm::Type* type) {
    return type && type->isVoidTy();
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Type Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::IntegerType* IRBuilderHelper::getIntType(IRLowering& lowerer, unsigned bits) {
    return llvm::IntegerType::get(lowerer.getContext(), bits);
}

llvm::PointerType* IRBuilderHelper::getPtrType(IRLowering& lowerer, llvm::Type* elemType) {
    if (elemType) {
        return llvm::PointerType::get(elemType, 0);
    }
    return llvm::PointerType::getUnqual(lowerer.getContext());
}

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Constant Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::ConstantInt* IRBuilderHelper::getIntConstant(IRLowering& lowerer,
                                                   uint64_t value,
                                                   unsigned bits) {
    auto* intType = llvm::IntegerType::get(lowerer.getContext(), bits);
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(intType, value)
    );
}

llvm::ConstantInt* IRBuilderHelper::getInt32Constant(IRLowering& lowerer, uint32_t value) {
    return getIntConstant(lowerer, value, 32);
}

llvm::ConstantInt* IRBuilderHelper::getInt64Constant(IRLowering& lowerer, uint64_t value) {
    return getIntConstant(lowerer, value, 64);
}

// FIXED: Use llvm::cast to convert Constant* to ConstantFP*
llvm::ConstantFP* IRBuilderHelper::getFloatConstant(IRLowering& lowerer, float value) {
    return llvm::cast<llvm::ConstantFP>(
        llvm::ConstantFP::get(llvm::Type::getFloatTy(lowerer.getContext()), value)
    );
}

// FIXED: Use llvm::cast to convert Constant* to ConstantFP*
llvm::ConstantFP* IRBuilderHelper::getDoubleConstant(IRLowering& lowerer, double value) {
    return llvm::cast<llvm::ConstantFP>(
        llvm::ConstantFP::get(llvm::Type::getDoubleTy(lowerer.getContext()), value)
    );
}

llvm::Constant* IRBuilderHelper::getBoolConstant(IRLowering& lowerer, bool value) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(lowerer.getContext()), value);
}