/// @file support/LLVMHelpers.hpp
/// @brief Pure utility helpers for working with LLVM types and values.
///
/// This file provides utility functions for querying LLVM types and values
/// WITHOUT depending on CodeGenContext. These are pure type/value utilities.
///
/// ─── Why Centralize? ────────────────────────────────────────────────────────
/// 1. Consistent type checking across all CodeGen files
/// 2. Easy to update when LLVM's API changes
/// 3. Clear documentation of type predicates
/// 4. Single place to add new type helpers

#pragma once

#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <string>

namespace codegen {

// ─── Type Predicates ───────────────────────────────────────────────────────

/// @brief Check if an LLVM type is an integer type (any bit width).
inline bool isIntegerType(llvm::Type* type) {
    return type && type->isIntegerTy();
}

/// @brief Check if an LLVM type is a floating-point type (float, double, etc.).
inline bool isFloatType(llvm::Type* type) {
    return type && type->isFloatingPointTy();
}

/// @brief Check if an LLVM type is a pointer type.
inline bool isPointerType(llvm::Type* type) {
    return type && type->isPointerTy();
}

/// @brief Check if an LLVM type is a struct type.
inline bool isStructType(llvm::Type* type) {
    return type && type->isStructTy();
}

/// @brief Check if an LLVM type is an array type.
inline bool isArrayType(llvm::Type* type) {
    return type && type->isArrayTy();
}

/// @brief Check if an LLVM type is a vector type (SIMD).
inline bool isVectorType(llvm::Type* type) {
    return type && type->isVectorTy();
}

/// @brief Check if an LLVM type is a void type.
inline bool isVoidType(llvm::Type* type) {
    return type && type->isVoidTy();
}

/// @brief Check if an LLVM type is numeric (integer or floating-point).
inline bool isNumericType(llvm::Type* type) {
    return isIntegerType(type) || isFloatType(type);
}

/// @brief Check if an LLVM type is a tagged slot (nullable/fallible).
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly two fields: { tag, value }.
inline bool isTaggedType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 2) return false;
    return structType->getElementType(0)->isIntegerTy(8);
}

/// @brief Check if an LLVM type is a slice type.
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly three fields: { ptr, len, cap }.
inline bool isSliceType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 3) return false;
    return isPointerType(structType->getElementType(0)) &&
           isIntegerType(structType->getElementType(1)) &&
           isIntegerType(structType->getElementType(2));
}

/// @brief Check if an LLVM type is a closure type.
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly two fields: { func, env }.
inline bool isClosureType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 2) return false;
    return isPointerType(structType->getElementType(0)) &&
           isPointerType(structType->getElementType(1));
}

/// @brief Check if an LLVM type is a string type.
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly three fields: { ptr, len, cap }.
inline bool isStringType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 3) return false;
    return isPointerType(structType->getElementType(0)) &&
           isIntegerType(structType->getElementType(1)) &&
           isIntegerType(structType->getElementType(2));
}

/// @brief Check if an LLVM type is a function type.
inline bool isFunctionType(llvm::Type* type) {
    return type && type->isFunctionTy();
}

// ─── Value Predicates ──────────────────────────────────────────────────────

/// @brief Check if an LLVM value is a constant integer.
inline bool isConstantInt(llvm::Value* value) {
    return value && llvm::isa<llvm::ConstantInt>(value);
}

/// @brief Check if an LLVM value is a constant floating-point.
inline bool isConstantFP(llvm::Value* value) {
    return value && llvm::isa<llvm::ConstantFP>(value);
}

/// @brief Check if an LLVM value is a constant pointer (null or global).
inline bool isConstantPointer(llvm::Value* value) {
    return value && llvm::isa<llvm::ConstantPointerNull>(value);
}

/// @brief Check if an LLVM value is a constant (any kind).
inline bool isConstant(llvm::Value* value) {
    return value && llvm::isa<llvm::Constant>(value);
}

/// @brief Check if an LLVM value is null (zero or null pointer).
inline bool isNullValue(llvm::Value* value) {
    if (!value) return true;
    if (llvm::ConstantInt* cint = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        return cint->isZero();
    }
    if (llvm::ConstantFP* cfp = llvm::dyn_cast<llvm::ConstantFP>(value)) {
        return cfp->isZero();
    }
    if (llvm::ConstantPointerNull* cpn = llvm::dyn_cast<llvm::ConstantPointerNull>(value)) {
        return true;
    }
    return false;
}

/// @brief Check if an LLVM value is an integer with bit width equal to 1 (bool).
inline bool isBoolValue(llvm::Value* value) {
    return value && value->getType()->isIntegerTy(1);
}

/// @brief Check if an LLVM value is a pointer.
inline bool isPointerValue(llvm::Value* value) {
    return value && value->getType()->isPointerTy();
}

/// @brief Check if an LLVM value is numeric (integer or floating-point).
inline bool isNumericValue(llvm::Value* value) {
    return value && isNumericType(value->getType());
}

// ─── Type Extraction ──────────────────────────────────────────────────────

/// @brief Get the integer bit width of a type, or 0 if not an integer.
inline unsigned getIntegerBitWidth(llvm::Type* type) {
    if (!isIntegerType(type)) return 0;
    return llvm::cast<llvm::IntegerType>(type)->getBitWidth();
}

/// @brief Get the element type of a pointer, array, or vector.
/// @param type The type to get the element type from.
/// @return The element type, or nullptr if not applicable.
inline llvm::Type* getElementType(llvm::Type* type) {
    if (!type) return nullptr;
    if (type->isPointerTy()) {
        // With opaque pointers (LLVM 17+), we can't get the element type
        // from a pointer. Return nullptr to indicate opaque.
        return nullptr;
    }
    if (type->isArrayTy()) {
        return llvm::cast<llvm::ArrayType>(type)->getElementType();
    }
    if (type->isVectorTy()) {
        return llvm::cast<llvm::VectorType>(type)->getElementType();
    }
    if (type->isStructTy()) {
        llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
        if (structType->getNumElements() > 0) {
            return structType->getElementType(0);
        }
    }
    return nullptr;
}

/// @brief Get the number of elements in an array or vector.
/// @param type The array or vector type.
/// @return The number of elements, or 0 if not an array or vector.
inline uint64_t getNumElements(llvm::Type* type) {
    if (!type) return 0;
    if (type->isArrayTy()) {
        return llvm::cast<llvm::ArrayType>(type)->getNumElements();
    }
    if (type->isVectorTy()) {
        return llvm::cast<llvm::VectorType>(type)->getElementCount().getKnownMinValue();
    }
    return 0;
}

/// @brief Get the tag type from a tagged slot type.
/// @param type The tagged slot type.
/// @return The tag type (usually i8), or nullptr if not a tagged type.
inline llvm::Type* getTagType(llvm::Type* type) {
    if (!isTaggedType(type)) return nullptr;
    return llvm::cast<llvm::StructType>(type)->getElementType(0);
}

/// @brief Get the value type from a tagged slot type.
/// @param type The tagged slot type.
/// @return The value type (the second field), or nullptr if not a tagged type.
inline llvm::Type* getTaggedValueType(llvm::Type* type) {
    if (!isTaggedType(type)) return nullptr;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() < 2) return nullptr;
    return structType->getElementType(1);
}

/// @brief Get the integer bit width of a value, or 0 if not an integer.
inline unsigned getIntegerBitWidth(llvm::Value* value) {
    if (!value) return 0;
    return getIntegerBitWidth(value->getType());
}

/// @brief Get the number of function parameters.
inline unsigned getFunctionNumParams(llvm::Type* type) {
    if (!isFunctionType(type)) return 0;
    return llvm::cast<llvm::FunctionType>(type)->getNumParams();
}

/// @brief Check if a function type is vararg.
inline bool isFunctionVarArg(llvm::Type* type) {
    if (!isFunctionType(type)) return false;
    return llvm::cast<llvm::FunctionType>(type)->isVarArg();
}

// ─── Type Constants ───────────────────────────────────────────────────────

/// @brief Get the i8 type (for tags).
inline llvm::Type* getI8Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt8Ty(ctx);
}

/// @brief Get the i32 type (for integers).
inline llvm::Type* getI32Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt32Ty(ctx);
}

/// @brief Get the i64 type (for lengths and pointers).
inline llvm::Type* getI64Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt64Ty(ctx);
}

/// @brief Get the i1 type (for booleans).
inline llvm::Type* getI1Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt1Ty(ctx);
}

/// @brief Get the float type.
inline llvm::Type* getFloatType(llvm::LLVMContext& ctx) {
    return llvm::Type::getFloatTy(ctx);
}

/// @brief Get the double type.
inline llvm::Type* getDoubleType(llvm::LLVMContext& ctx) {
    return llvm::Type::getDoubleTy(ctx);
}

/// @brief Get the void type.
inline llvm::Type* getVoidType(llvm::LLVMContext& ctx) {
    return llvm::Type::getVoidTy(ctx);
}

/// @brief Get the pointer type.
inline llvm::Type* getPtrType(llvm::LLVMContext& ctx) {
    return llvm::PointerType::get(ctx, 0);
}

/// @brief Get the string type (struct { ptr, len, cap }).
/// @param ctx The LLVM context.
/// @return The string struct type.
inline llvm::StructType* getStringType(llvm::LLVMContext& ctx) {
    llvm::Type* i8Ptr = llvm::PointerType::get(ctx, 0);
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
    return llvm::StructType::get(ctx, {i8Ptr, i64, i64});
}

// ─── Value Creation Helpers ──────────────────────────────────────────────

/// @brief Create a zero constant for a type.
inline llvm::Constant* getZeroValue(llvm::Type* type) {
    if (!type) return nullptr;
    return llvm::Constant::getNullValue(type);
}

/// @brief Create an integer constant of the specified bit width.
inline llvm::ConstantInt* getIntConstant(llvm::LLVMContext& ctx, uint64_t value, unsigned bitWidth) {
    return llvm::ConstantInt::get(llvm::IntegerType::get(ctx, bitWidth), value);
}

/// @brief Create an i64 constant.
inline llvm::ConstantInt* getI64Constant(llvm::LLVMContext& ctx, uint64_t value) {
    return getIntConstant(ctx, value, 64);
}

/// @brief Create an i32 constant.
inline llvm::ConstantInt* getI32Constant(llvm::LLVMContext& ctx, uint32_t value) {
    return getIntConstant(ctx, value, 32);
}

/// @brief Create an i8 constant.
inline llvm::ConstantInt* getI8Constant(llvm::LLVMContext& ctx, uint8_t value) {
    return getIntConstant(ctx, value, 8);
}

// ─── Debug Helpers ─────────────────────────────────────────────────────────

/// @brief Get the string type name for debugging.
inline std::string getTypeName(llvm::Type* type) {
    if (!type) return "null";
    std::string name;
    llvm::raw_string_ostream os(name);
    type->print(os);
    return name;
}

} // namespace codegen