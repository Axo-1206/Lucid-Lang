/// @file types/LLVMTypeHelpers.hpp
/// @brief Pure LLVM type and value utilities.
///
/// This file provides utility functions for querying LLVM types and values
/// WITHOUT depending on CodeGenContext. These are pure type/value utilities
/// that can be used anywhere in the code generator.
///
/// ─── Why Separate from CodeGenContext? ──────────────────────────────────────
/// CodeGenContext is heavy - it holds the module, builder, symbol tables, etc.
/// These helpers are pure functions that only need LLVM types/values and
/// optionally a DataLayout/Module for size/alignment queries.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// These helpers are used by CodeGenType (Lucid → LLVM mapping) and by
/// expression/statement lowering functions that need to inspect LLVM types.
///
/// @example
///   // Check if a type is an integer
///   if (isIntegerType(type)) { ... }
///
///   // Get the size of a type
///   uint64_t size = getTypeSize(type, module);
///
///   // Get the i64 type
///   llvm::Type* i64 = getI64Type(ctx);

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
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Intrinsics.h>

#include <llvm/Support/Casting.h>
#include <string>
#include <cassert>

namespace codegen {

// =============================================================================
// TYPE PREDICATES
// =============================================================================

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

/// @brief Check if an LLVM type is a function type.
inline bool isFunctionType(llvm::Type* type) {
    return type && type->isFunctionTy();
}

/// @brief Check if an LLVM type is a label type.
inline bool isLabelType(llvm::Type* type) {
    return type && type->isLabelTy();
}

/// @brief Check if an LLVM type is a metadata type.
inline bool isMetadataType(llvm::Type* type) {
    return type && type->isMetadataTy();
}

/// @brief Check if an LLVM type is a token type.
inline bool isTokenType(llvm::Type* type) {
    return type && type->isTokenTy();
}

// =============================================================================
// STRUCT TYPE PREDICATES
// =============================================================================

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

/// @brief Check if an LLVM type is a string type (lucid.String).
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

/// @brief Check if an LLVM type is an Arena type (lucid.Arena).
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly three fields: { base, size, cursor }.
inline bool isArenaType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 3) return false;
    return isPointerType(structType->getElementType(0)) &&
           isIntegerType(structType->getElementType(1)) &&
           isIntegerType(structType->getElementType(2));
}

/// @brief Check if an LLVM type is an ArenaDescriptor type (lucid.ArenaDescriptor).
/// @param type The LLVM type to check.
/// @return True if the type is a struct with exactly two fields: { base, size }.
inline bool isArenaDescriptorType(llvm::Type* type) {
    if (!isStructType(type)) return false;
    llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
    if (structType->getNumElements() != 2) return false;
    return isPointerType(structType->getElementType(0)) &&
           isIntegerType(structType->getElementType(1));
}

// =============================================================================
// VALUE PREDICATES
// =============================================================================

/// @brief Check if an LLVM value is a constant integer.
inline bool isConstantInt(llvm::Value* value) {
    return value && llvm::isa<llvm::ConstantInt>(value);
}

/// @brief Get the integer value from a constant integer.
inline uint64_t getConstantIntValue(llvm::Value* value) {
    if (auto* cint = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        return cint->getZExtValue();
    }
    return 0;
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
    if (llvm::ConstantAggregateZero* caz = llvm::dyn_cast<llvm::ConstantAggregateZero>(value)) {
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
    return value && isPointerType(value->getType());
}

/// @brief Check if an LLVM value is numeric (integer or floating-point).
inline bool isNumericValue(llvm::Value* value) {
    return value && isNumericType(value->getType());
}

/// @brief Check if an LLVM value is an instruction (as opposed to a constant/argument).
inline bool isInstruction(llvm::Value* value) {
    return value && llvm::isa<llvm::Instruction>(value);
}

/// @brief Check if an LLVM value is an argument (function parameter).
inline bool isArgument(llvm::Value* value) {
    return value && llvm::isa<llvm::Argument>(value);
}

/// @brief Check if an LLVM value is a global variable.
inline bool isGlobalVariable(llvm::Value* value) {
    return value && llvm::isa<llvm::GlobalVariable>(value);
}

/// @brief Check if an LLVM value is a function.
inline bool isFunction(llvm::Value* value) {
    return value && llvm::isa<llvm::Function>(value);
}

/// @brief Check if an LLVM value is an alloca instruction.
inline bool isAlloca(llvm::Value* value) {
    return value && llvm::isa<llvm::AllocaInst>(value);
}

// =============================================================================
// TYPE EXTRACTION HELPERS
// =============================================================================

/// @brief Get the integer bit width of a type, or 0 if not an integer.
inline unsigned getIntegerBitWidth(llvm::Type* type) {
    if (!isIntegerType(type)) return 0;
    return llvm::cast<llvm::IntegerType>(type)->getBitWidth();
}

/// @brief Get the integer bit width of a value, or 0 if not an integer.
inline unsigned getIntegerBitWidth(llvm::Value* value) {
    if (!value) return 0;
    return getIntegerBitWidth(value->getType());
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

/// @brief Get the lane count from a vector type.
inline uint64_t getLaneCount(llvm::Type* type) {
    if (!type || !type->isVectorTy()) return 0;
    return llvm::cast<llvm::VectorType>(type)->getElementCount().getKnownMinValue();
}

/// @brief Get the element type from a vector type.
inline llvm::Type* getVectorElementType(llvm::Type* type) {
    if (!type || !type->isVectorTy()) return nullptr;
    return llvm::cast<llvm::VectorType>(type)->getElementType();
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

/// @brief Get the return type of a function type.
inline llvm::Type* getFunctionReturnType(llvm::Type* type) {
    if (!isFunctionType(type)) return nullptr;
    return llvm::cast<llvm::FunctionType>(type)->getReturnType();
}

/// @brief Get the parameter type of a function type at a given index.
inline llvm::Type* getFunctionParamType(llvm::Type* type, unsigned index) {
    if (!isFunctionType(type)) return nullptr;
    llvm::FunctionType* fnType = llvm::cast<llvm::FunctionType>(type);
    if (index >= fnType->getNumParams()) return nullptr;
    return fnType->getParamType(index);
}

// =============================================================================
// DATA LAYOUT HELPERS
// =============================================================================

/// @brief Get the size of an LLVM type in bytes using a DataLayout.
/// @param type The LLVM type to get the size of.
/// @param dl The DataLayout to use.
/// @return The size in bytes, or 0 if the type has no size.
inline uint64_t getTypeSize(llvm::Type* type, const llvm::DataLayout& dl) {
    if (!type) return 0;
    if (!type->isSized()) return 0;
    return dl.getTypeAllocSize(type);
}

/// @brief Get the size of an LLVM type in bytes using a module's DataLayout.
/// @param type The LLVM type to get the size of.
/// @param module The LLVM module (provides DataLayout).
/// @return The size in bytes, or 0 if the type has no size.
inline uint64_t getTypeSize(llvm::Type* type, const llvm::Module* module) {
    if (!module) return 0;
    return getTypeSize(type, module->getDataLayout());
}

/// @brief Get the alignment of an LLVM type in bytes using a DataLayout.
/// @param type The LLVM type to get the alignment of.
/// @param dl The DataLayout to use.
/// @return The alignment in bytes, or 0 if the type has no alignment.
inline uint64_t getTypeAlign(llvm::Type* type, const llvm::DataLayout& dl) {
    if (!type) return 0;
    if (!type->isSized()) return 0;
    return dl.getABITypeAlign(type).value();
}

/// @brief Get the alignment of an LLVM type in bytes using a module's DataLayout.
/// @param type The LLVM type to get the alignment of.
/// @param module The LLVM module (provides DataLayout).
/// @return The alignment in bytes, or 0 if the type has no alignment.
inline uint64_t getTypeAlign(llvm::Type* type, const llvm::Module* module) {
    if (!module) return 0;
    return getTypeAlign(type, module->getDataLayout());
}

/// @brief Get the preferred alignment of an LLVM type in bytes using a DataLayout.
/// @param type The LLVM type to get the alignment of.
/// @param dl The DataLayout to use.
/// @return The preferred alignment in bytes, or 0 if the type has no alignment.
inline uint64_t getTypePrefAlign(llvm::Type* type, const llvm::DataLayout& dl) {
    if (!type) return 0;
    if (!type->isSized()) return 0;
    return dl.getPrefTypeAlign(type).value();
}

/// @brief Get the preferred alignment of an LLVM type in bytes using a module's DataLayout.
/// @param type The LLVM type to get the alignment of.
/// @param module The LLVM module (provides DataLayout).
/// @return The preferred alignment in bytes, or 0 if the type has no alignment.
inline uint64_t getTypePrefAlign(llvm::Type* type, const llvm::Module* module) {
    if (!module) return 0;
    return getTypePrefAlign(type, module->getDataLayout());
}

// =============================================================================
// TYPE CONSTANTS
// =============================================================================

/// @brief Get the i1 type (for booleans).
inline llvm::Type* getI1Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt1Ty(ctx);
}

/// @brief Get the i8 type (for tags and bytes).
inline llvm::Type* getI8Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt8Ty(ctx);
}

/// @brief Get the i16 type.
inline llvm::Type* getI16Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt16Ty(ctx);
}

/// @brief Get the i32 type (for integers and indices).
inline llvm::Type* getI32Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt32Ty(ctx);
}

/// @brief Get the i64 type (for lengths, pointers, and sizes).
inline llvm::Type* getI64Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt64Ty(ctx);
}

/// @brief Get the float type (32-bit).
inline llvm::Type* getFloatType(llvm::LLVMContext& ctx) {
    return llvm::Type::getFloatTy(ctx);
}

/// @brief Get the double type (64-bit).
inline llvm::Type* getDoubleType(llvm::LLVMContext& ctx) {
    return llvm::Type::getDoubleTy(ctx);
}

/// @brief Get the fp128 type (128-bit high precision).
inline llvm::Type* getFP128Type(llvm::LLVMContext& ctx) {
    return llvm::Type::getFP128Ty(ctx);
}

/// @brief Get the void type.
inline llvm::Type* getVoidType(llvm::LLVMContext& ctx) {
    return llvm::Type::getVoidTy(ctx);
}

/// @brief Get an opaque pointer type.
inline llvm::PointerType* getPtrType(llvm::LLVMContext& ctx) {
    return llvm::PointerType::get(ctx, 0);
}

/// @brief Get a pointer type with a specific address space.
inline llvm::PointerType* getPtrType(llvm::LLVMContext& ctx, unsigned addrSpace) {
    return llvm::PointerType::get(ctx, addrSpace);
}

// =============================================================================
// STRUCT TYPE CREATION
// =============================================================================

/// @brief Get or create the lucid.String struct type.
/// @param module The LLVM module to create the type in.
/// @return The lucid.String struct type.
inline llvm::StructType* getStringType(llvm::Module* module) {
    assert(module && "Module cannot be null");
    llvm::LLVMContext& ctx = module->getContext();
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx, "lucid.String");
    if (!type) {
        type = llvm::StructType::create(ctx, "lucid.String");
        type->setBody({
            getPtrType(ctx),   // ptr: i8*
            getI64Type(ctx),   // len: i64
            getI64Type(ctx)    // cap: i64
        });
    }
    return type;
}

/// @brief Get or create the lucid.Slice struct type.
/// @param module The LLVM module to create the type in.
/// @return The lucid.Slice struct type.
inline llvm::StructType* getSliceType(llvm::Module* module) {
    assert(module && "Module cannot be null");
    llvm::LLVMContext& ctx = module->getContext();
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx, "lucid.Slice");
    if (!type) {
        type = llvm::StructType::create(ctx, "lucid.Slice");
        type->setBody({
            getPtrType(ctx),   // ptr: i8*
            getI64Type(ctx),   // len: i64
            getI64Type(ctx)    // cap: i64
        });
    }
    return type;
}

/// @brief Get or create the lucid.Closure struct type.
/// @param module The LLVM module to create the type in.
/// @return The lucid.Closure struct type.
inline llvm::StructType* getClosureType(llvm::Module* module) {
    assert(module && "Module cannot be null");
    llvm::LLVMContext& ctx = module->getContext();
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx, "lucid.Closure");
    if (!type) {
        type = llvm::StructType::create(ctx, "lucid.Closure");
        type->setBody({
            getPtrType(ctx),   // func ptr: i8*
            getPtrType(ctx)    // env ptr: i8*
        });
    }
    return type;
}

/// @brief Get or create the lucid.Arena struct type.
/// @param module The LLVM module to create the type in.
/// @return The lucid.Arena struct type.
inline llvm::StructType* getArenaType(llvm::Module* module) {
    assert(module && "Module cannot be null");
    llvm::LLVMContext& ctx = module->getContext();
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx, "lucid.Arena");
    if (!type) {
        type = llvm::StructType::create(ctx, "lucid.Arena");
        type->setBody({
            getPtrType(ctx),   // base: i8*
            getI64Type(ctx),   // size: i64
            getI64Type(ctx)    // cursor: i64
        });
    }
    return type;
}

/// @brief Get or create the lucid.ArenaDescriptor struct type.
/// @param module The LLVM module to create the type in.
/// @return The lucid.ArenaDescriptor struct type.
inline llvm::StructType* getArenaDescriptorType(llvm::Module* module) {
    assert(module && "Module cannot be null");
    llvm::LLVMContext& ctx = module->getContext();
    llvm::StructType* type = llvm::StructType::getTypeByName(ctx, "lucid.ArenaDescriptor");
    if (!type) {
        type = llvm::StructType::create(ctx, "lucid.ArenaDescriptor");
        type->setBody({
            getPtrType(ctx),   // base: i8*
            getI64Type(ctx)    // size: i64
        });
    }
    return type;
}

// =============================================================================
// VALUE CREATION HELPERS
// =============================================================================

/// @brief Create a zero constant for a type.
inline llvm::Constant* getZeroValue(llvm::Type* type) {
    if (!type) return nullptr;
    return llvm::Constant::getNullValue(type);
}

/// @brief Create an integer constant of the specified bit width.
inline llvm::ConstantInt* getIntConstant(llvm::LLVMContext& ctx, uint64_t value, unsigned bitWidth) {
    return llvm::ConstantInt::get(llvm::IntegerType::get(ctx, bitWidth), value);
}

/// @brief Create an i1 constant (bool).
inline llvm::ConstantInt* getI1Constant(llvm::LLVMContext& ctx, bool value) {
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(getI1Type(ctx), value ? 1 : 0)
    );
}

/// @brief Create an i8 constant.
inline llvm::ConstantInt* getI8Constant(llvm::LLVMContext& ctx, uint8_t value) {
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(getI8Type(ctx), value)
    );
}

/// @brief Create an i16 constant.
inline llvm::ConstantInt* getI16Constant(llvm::LLVMContext& ctx, uint16_t value) {
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(getI16Type(ctx), value)
    );
}

/// @brief Create an i32 constant.
inline llvm::ConstantInt* getI32Constant(llvm::LLVMContext& ctx, uint32_t value) {
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(getI32Type(ctx), value)
    );
}

/// @brief Create an i64 constant.
inline llvm::ConstantInt* getI64Constant(llvm::LLVMContext& ctx, uint64_t value) {
    return llvm::cast<llvm::ConstantInt>(
        llvm::ConstantInt::get(getI64Type(ctx), value)
    );
}

/// @brief Create a float constant.
inline llvm::ConstantFP* getFloatConstant(llvm::LLVMContext& ctx, float value) {
    return llvm::cast<llvm::ConstantFP>(
        llvm::ConstantFP::get(getFloatType(ctx), value)
    );
}

/// @brief Create a double constant.
inline llvm::ConstantFP* getDoubleConstant(llvm::LLVMContext& ctx, double value) {
    return llvm::cast<llvm::ConstantFP>(
        llvm::ConstantFP::get(getDoubleType(ctx), value)
    );
}

/// @brief Create a null pointer constant.
inline llvm::ConstantPointerNull* getNullPtr(llvm::LLVMContext& ctx) {
    return llvm::ConstantPointerNull::get(getPtrType(ctx));
}

// =============================================================================
// MEMORY ORDERING HELPERS
// =============================================================================

/// @brief Parse a memory ordering string to llvm::AtomicOrdering.
/// @param order The ordering string ("relaxed", "acquire", "release", "acq_rel", "seq_cst").
/// @return The corresponding llvm::AtomicOrdering, or llvm::AtomicOrdering::Monotonic if invalid.
inline llvm::AtomicOrdering parseAtomicOrdering(const std::string& order) {
    using llvm::AtomicOrdering;
    if (order == "relaxed") return AtomicOrdering::Monotonic;
    if (order == "acquire") return AtomicOrdering::Acquire;
    if (order == "release") return AtomicOrdering::Release;
    if (order == "acq_rel") return AtomicOrdering::AcquireRelease;
    if (order == "seq_cst") return AtomicOrdering::SequentiallyConsistent;
    return AtomicOrdering::Monotonic;
}

/// @brief Get the synchronization scope from a string.
/// @param scope The scope string ("singlethread", "system").
/// @return The corresponding llvm::SyncScope::ID.
inline llvm::SyncScope::ID parseSyncScope(const std::string& scope, llvm::LLVMContext& ctx) {
    if (scope == "singlethread") {
        return llvm::SyncScope::SingleThread;
    }
    return llvm::SyncScope::System;
}

// =============================================================================
// DEBUG HELPERS
// =============================================================================

/// @brief Get the string representation of an LLVM type for debugging.
inline std::string getTypeName(llvm::Type* type) {
    if (!type) return "null";
    std::string name;
    llvm::raw_string_ostream os(name);
    type->print(os);
    return name;
}

/// @brief Get the string representation of an LLVM value for debugging.
inline std::string getValueName(llvm::Value* value) {
    if (!value) return "null";
    std::string name;
    llvm::raw_string_ostream os(name);
    value->print(os);
    return name;
}

/// @brief Check if a type has a name.
inline bool hasName(llvm::Type* type) {
    if (!type) return false;
    if (type->isStructTy()) {
        llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
        return !structType->getName().empty();
    }
    return false;
}

/// @brief Get the name of a type if it has one.
inline std::string getTypeNameIfNamed(llvm::Type* type) {
    if (!type) return "";
    if (type->isStructTy()) {
        llvm::StructType* structType = llvm::cast<llvm::StructType>(type);
        return structType->getName().str();
    }
    return "";
}

// =============================================================================
// INT TYPE UTILITIES
// =============================================================================

/// @brief Check if an integer type is signed (based on context).
/// This is a heuristic - LLVM integer types don't have signedness.
/// We use this when we know the semantic signedness from the AST.
inline bool isSignedInteger(llvm::Type* type) {
    // This is just a heuristic - we need the AST info for true signedness
    // CodeGen should use the AST's PrimitiveKind for signedness.
    // This helper is provided for convenience but should be used with care.
    (void)type;
    return true;  // Default to signed
}

/// @brief Get the next larger integer type.
inline llvm::IntegerType* getLargerIntegerType(llvm::LLVMContext& ctx, llvm::IntegerType* type) {
    if (!type) return nullptr;
    unsigned bits = type->getBitWidth();
    if (bits == 8) return llvm::IntegerType::get(ctx, 16);
    if (bits == 16) return llvm::IntegerType::get(ctx, 32);
    if (bits == 32) return llvm::IntegerType::get(ctx, 64);
    // No larger than 64-bit for now
    return nullptr;
}

/// @brief Check if an integer type can represent all values of another.
inline bool canRepresentInteger(llvm::IntegerType* target, llvm::IntegerType* source) {
    if (!target || !source) return false;
    unsigned targetBits = target->getBitWidth();
    unsigned sourceBits = source->getBitWidth();
    return targetBits >= sourceBits;
}

} // namespace codegen