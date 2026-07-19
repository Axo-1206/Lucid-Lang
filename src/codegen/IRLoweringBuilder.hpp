/**
 * @file IRLoweringBuilder.hpp
 * @brief Helper utilities for common IR construction patterns.
 * 
 * @responsibility Provides reusable helper functions for building common
 *                 LLVM IR patterns used across multiple lowering modules.
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Main IR lowering class
 *   - src/codegen/IRLoweringBuilder.cpp - Implementation
 */

#pragma once

#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"

// Forward declaration
class IRLowering;

/**
 * @brief Helper utilities for common IR construction patterns.
 * 
 * Implemented in IRLoweringBuilder.cpp
 */
struct IRBuilderHelper {
    // ─── Allocation ────────────────────────────────────────────────────────

    static llvm::AllocaInst* createAlloca(IRLowering& lowerer, llvm::Type* type,
                                          const std::string& name = "");

    // ─── Load/Store ────────────────────────────────────────────────────────

    static llvm::LoadInst* createLoad(IRLowering& lowerer, llvm::Value* ptr,
                                      const std::string& name = "");
    static llvm::StoreInst* createStore(IRLowering& lowerer, llvm::Value* value,
                                        llvm::Value* ptr);

    // ─── GetElementPointer ─────────────────────────────────────────────────

    static llvm::Value* createGEP(IRLowering& lowerer, llvm::Type* elemType,
                                  llvm::Value* ptr, llvm::Value* index,
                                  const std::string& name = "");
    static llvm::Value* createStructGEP(IRLowering& lowerer,
                                        llvm::StructType* structType,
                                        llvm::Value* ptr, unsigned index,
                                        const std::string& name = "");

    // ─── Pointer Conversion ────────────────────────────────────────────────

    static llvm::Value* createPtrToInt(IRLowering& lowerer, llvm::Value* ptr,
                                       const std::string& name = "");
    static llvm::Value* createIntToPtr(IRLowering& lowerer, llvm::Value* intVal,
                                       llvm::Type* ptrType, const std::string& name = "");

    // ─── Branching ─────────────────────────────────────────────────────────

    static void createConditionalBranch(IRLowering& lowerer, llvm::Value* cond,
                                        llvm::BasicBlock* trueBlock,
                                        llvm::BasicBlock* falseBlock);

    // ─── Block Management ──────────────────────────────────────────────────

    static llvm::BasicBlock* createBlock(IRLowering& lowerer, llvm::Function* fn,
                                         const std::string& name = "");
    static void setInsertPoint(IRLowering& lowerer, llvm::BasicBlock* block);

    // ─── Comparisons ───────────────────────────────────────────────────────

    static llvm::Value* createICmp(IRLowering& lowerer, llvm::CmpInst::Predicate pred,
                                   llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    static llvm::Value* createFCmp(IRLowering& lowerer, llvm::CmpInst::Predicate pred,
                                   llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");

    // ─── Arithmetic ────────────────────────────────────────────────────────

    static llvm::Value* createAdd(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createSub(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createMul(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createDiv(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createRem(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");

    // ─── Bitwise ───────────────────────────────────────────────────────────

    static llvm::Value* createAnd(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createOr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                 const std::string& name = "");
    static llvm::Value* createXor(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createShl(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createLShr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    static llvm::Value* createAShr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");

    // ─── Null Checks ───────────────────────────────────────────────────────

    static llvm::Value* createIsNull(IRLowering& lowerer, llvm::Value* value,
                                     const std::string& name = "");
    static llvm::Value* createIsNotNull(IRLowering& lowerer, llvm::Value* value,
                                        const std::string& name = "");

    // ─── Select ────────────────────────────────────────────────────────────

    static llvm::Value* createSelect(IRLowering& lowerer, llvm::Value* cond,
                                     llvm::Value* trueVal, llvm::Value* falseVal,
                                     const std::string& name = "");

    // ─── Type Utilities ────────────────────────────────────────────────────

    static bool isIntegerType(llvm::Type* type);
    static bool isFloatingPointType(llvm::Type* type);
    static bool isPointerType(llvm::Type* type);
    static bool isStructType(llvm::Type* type);
    static bool isArrayType(llvm::Type* type);
    static bool isVoidType(llvm::Type* type);

    // ─── Type Creation ─────────────────────────────────────────────────────

    static llvm::IntegerType* getIntType(IRLowering& lowerer, unsigned bits);
    static llvm::PointerType* getPtrType(IRLowering& lowerer, llvm::Type* elemType = nullptr);

    // ─── Constants ─────────────────────────────────────────────────────────

    static llvm::ConstantInt* getIntConstant(IRLowering& lowerer, uint64_t value, unsigned bits);
    static llvm::ConstantInt* getInt32Constant(IRLowering& lowerer, uint32_t value);
    static llvm::ConstantInt* getInt64Constant(IRLowering& lowerer, uint64_t value);
    static llvm::ConstantFP* getFloatConstant(IRLowering& lowerer, float value);
    static llvm::ConstantFP* getDoubleConstant(IRLowering& lowerer, double value);
    static llvm::Constant* getBoolConstant(IRLowering& lowerer, bool value);
};