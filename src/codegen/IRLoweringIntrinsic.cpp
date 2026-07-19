/**
 * @file IRLoweringIntrinsic.cpp
 * @brief Lowers Lucid intrinsics to LLVM IR.
 * 
 * @responsibility Handles all intrinsic calls: LLVM intrinsics (sqrt, memcpy, etc.)
 *                 and compiler-handled intrinsics (sizeof, typeof, tostr, etc.).
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Declarations
 *   - src/codegen/IRLoweringExpr.cpp - Expression lowering (calls this)
 *   - src/codegen/IRLoweringBuilder.hpp - Helper builders
 *   - src/codegen/IntrinsicRegistry.hpp - Intrinsic validation and mapping
 */

#include "IRLowering.hpp"
#include "IRLoweringBuilder.hpp"
#include "../sema/support/IntrinsicRegistry.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"

#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// IRIntrinsicLowering - Main Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRIntrinsicLowering::lowerIntrinsic(IRLowering& lowerer, 
                                                  IntrinsicCallExprAST* intrinsic) {
    if (!intrinsic) {
        return nullptr;
    }

    // ─── LLVM Intrinsics (have an enum from semantic phase) ────────────────
    if (intrinsic->intrinsicID.has_value()) {
        return lowerLLVMIntrinsic(lowerer, intrinsic);
    }

    // ─── Compiler-Handled Intrinsics (no LLVM enum) ──────────────────────
    std::string name = lowerer.internedToString(intrinsic->intrinsicName);

    if (name == "sizeof") {
        return lowerIntrinsicSizeof(lowerer, intrinsic);
    } else if (name == "typeof") {
        return lowerIntrinsicTypeof(lowerer, intrinsic);
    } else if (name == "tostr") {
        return lowerIntrinsicTostr(lowerer, intrinsic);
    } else if (name == "addrof") {
        return lowerIntrinsicAddrof(lowerer, intrinsic);
    } else if (name == "ptrOffset") {
        return lowerIntrinsicPtrOffset(lowerer, intrinsic);
    } else if (name == "ptrDiff") {
        return lowerIntrinsicPtrDiff(lowerer, intrinsic);
    } else if (name == "toRef") {
        return lowerIntrinsicToRef(lowerer, intrinsic);
    } else if (name == "toPtr") {
        return lowerIntrinsicToPtr(lowerer, intrinsic);
    } else if (name == "alignof") {
        return lowerIntrinsicAlignof(lowerer, intrinsic);
    } else if (name == "bitcast") {
        return lowerIntrinsicBitcast(lowerer, intrinsic);
    } else if (name == "likely") {
        return lowerIntrinsicLikely(lowerer, intrinsic);
    } else if (name == "unlikely") {
        return lowerIntrinsicUnlikely(lowerer, intrinsic);
    }

    throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                          "Unknown intrinsic: " + name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRIntrinsicLowering - LLVM Intrinsics
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRIntrinsicLowering::lowerLLVMIntrinsic(IRLowering& lowerer,
                                                      IntrinsicCallExprAST* intrinsic) {
    if (!intrinsic || !intrinsic->intrinsicID.has_value()) {
        return nullptr;
    }

    auto id = intrinsic->intrinsicID.value();

    // Lower all arguments
    std::vector<llvm::Value*> args;
    for (ExprPtr arg : intrinsic->args) {
        auto* value = IRExprLowering::lowerExpr(lowerer, arg);
        if (value) {
            args.push_back(value);
        }
    }

    // Build type parameters from argument types
    std::vector<llvm::Type*> typeParams;
    for (auto* arg : args) {
        if (arg->getType()) {
            typeParams.push_back(arg->getType());
        }
    }

    // Get the intrinsic declaration
    auto* func = llvm::Intrinsic::getDeclaration(
        lowerer.getModule(),
        id,
        typeParams
    );

    if (!func) {
        std::string name = lowerer.internedToString(intrinsic->intrinsicName);
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to get declaration for intrinsic: " + name);
    }

    // Create the call
    std::string name = lowerer.internedToString(intrinsic->intrinsicName);
    return lowerer.getBuilder().CreateCall(func, args, name);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRIntrinsicLowering - Compiler-Handled Intrinsics
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* IRIntrinsicLowering::lowerIntrinsicSizeof(IRLowering& lowerer,
                                                        IntrinsicCallExprAST* intrinsic) {
    // #sizeof(T) - returns the size of a type in bytes
    // The type is passed as a type argument, not a value
    // FIXME: We need to get the type from the intrinsic arguments
    // The semantic phase should have stored the type somewhere
    
    // For now, return the size of the first argument's type
    if (!intrinsic->args.empty()) {
        auto* arg = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
        if (arg) {
            auto* type = arg->getType();
            // Get the size from the data layout
            // FIXME: Use the module's data layout
            uint64_t size = 8; // Default to pointer size
            if (type->isIntegerTy()) {
                size = type->getIntegerBitWidth() / 8;
            } else if (type->isFloatingPointTy()) {
                size = 8; // double
            } else if (type->isPointerTy()) {
                size = 8; // 64-bit pointer
            } else if (type->isStructTy()) {
                // FIXME: Calculate struct size
                size = 8;
            } else if (type->isArrayTy()) {
                auto* arrayType = llvm::cast<llvm::ArrayType>(type);
                size = arrayType->getNumElements() * 
                       (arrayType->getElementType()->getPrimitiveSizeInBits() / 8);
            }
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), size);
        }
    }

    // Fallback: return 8
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 8);
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicTypeof(IRLowering& lowerer,
                                                        IntrinsicCallExprAST* intrinsic) {
    // #typeof(x) - returns the type name as a string
    std::string typeName = "unknown";

    if (!intrinsic->args.empty()) {
        auto* arg = intrinsic->args[0];
        if (arg->resolvedType) {
            // Get type name from resolved type
            // FIXME: Better type name extraction
            if (arg->resolvedType->kind == ASTKind::PrimitiveType) {
                auto* primType = arg->resolvedType->as<PrimitiveTypeAST>();
                // Map primitive kind to name
                switch (primType->primitiveKind) {
                    case PrimitiveKind::Bool: typeName = "bool"; break;
                    case PrimitiveKind::Int: typeName = "int"; break;
                    case PrimitiveKind::Int32: typeName = "int32"; break;
                    case PrimitiveKind::Int64: typeName = "int64"; break;
                    case PrimitiveKind::Float: typeName = "float"; break;
                    case PrimitiveKind::Double: typeName = "double"; break;
                    case PrimitiveKind::String: typeName = "string"; break;
                    default: typeName = "primitive"; break;
                }
            } else if (arg->resolvedType->kind == ASTKind::NamedType) {
                auto* namedType = arg->resolvedType->as<NamedTypeAST>();
                typeName = lowerer.internedToString(namedType->name);
            }
        } else if (arg->kind == ASTKind::IdentifierExpr) {
            auto* ident = arg->as<IdentifierExprAST>();
            typeName = lowerer.internedToString(ident->name);
        }
    }

    return lowerer.getBuilder().CreateGlobalStringPtr(typeName, "typeof");
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicTostr(IRLowering& lowerer,
                                                       IntrinsicCallExprAST* intrinsic) {
    // #tostr(x) - converts a value to a string
    std::string str = "value";

    if (!intrinsic->args.empty()) {
        auto* arg = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
        if (arg) {
            // FIXME: Proper conversion to string based on type
            if (arg->getType()->isIntegerTy()) {
                // Convert integer to string
                // For now, just return a placeholder
                str = "<int>";
            } else if (arg->getType()->isFloatingPointTy()) {
                str = "<float>";
            } else if (arg->getType()->isPointerTy()) {
                str = "<pointer>";
            } else if (arg->getType()->isStructTy()) {
                str = "<struct>";
            }
        }
    }

    return lowerer.getBuilder().CreateGlobalStringPtr(str, "tostr");
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicAddrof(IRLowering& lowerer,
                                                        IntrinsicCallExprAST* intrinsic) {
    // #addrof(x) - returns the address of a value
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "addrof requires one argument");
    }

    auto* value = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower addrof argument");
    }

    // If value is already a pointer, return it directly
    if (value->getType()->isPointerTy()) {
        return value;
    }

    // Otherwise, it's an lvalue - return the address
    // FIXME: Need to handle lvalues properly
    // For now, if it's a load instruction, try to get its pointer operand
    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        return load->getPointerOperand();
    }

    // Default: return the value (which might be wrong)
    return value;
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicPtrOffset(IRLowering& lowerer,
                                                           IntrinsicCallExprAST* intrinsic) {
    // #ptrOffset(ptr, n) - pointer arithmetic
    if (intrinsic->args.size() < 2) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "ptrOffset requires two arguments");
    }

    auto* ptr = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
    auto* offset = IRExprLowering::lowerExpr(lowerer, intrinsic->args[1]);

    if (!ptr || !offset) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower ptrOffset arguments");
    }

    // Ensure ptr is a pointer
    if (!ptr->getType()->isPointerTy()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "ptrOffset requires a pointer as first argument");
    }

    // Convert offset to i64 if needed
    if (offset->getType() != llvm::Type::getInt64Ty(lowerer.getContext())) {
        offset = lowerer.getBuilder().CreateSExtOrTrunc(
            offset,
            llvm::Type::getInt64Ty(lowerer.getContext()),
            "offset.cast"
        );
    }

    // Get the element type
    auto* elemType = ptr->getType()->getNonOpaquePointerElementType();
    if (!elemType) {
        // For opaque pointers, use i8
        elemType = llvm::Type::getInt8Ty(lowerer.getContext());
    }

    // Use GEP for pointer arithmetic
    return IRBuilderHelper::createGEP(lowerer, elemType, ptr, offset, "ptroffset");
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicPtrDiff(IRLowering& lowerer,
                                                         IntrinsicCallExprAST* intrinsic) {
    // #ptrDiff(p1, p2) → sub (ptrtoint %p1), (ptrtoint %p2)
    if (intrinsic->args.size() < 2) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "ptrDiff requires two arguments");
    }

    auto* p1 = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
    auto* p2 = IRExprLowering::lowerExpr(lowerer, intrinsic->args[1]);

    if (!p1 || !p2) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower ptrDiff arguments");
    }

    // Ensure both are pointers
    if (!p1->getType()->isPointerTy() || !p2->getType()->isPointerTy()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "ptrDiff requires pointer arguments");
    }

    // Convert both pointers to integers
    auto* intPtr1 = IRBuilderHelper::createPtrToInt(lowerer, p1, "p1.int");
    auto* intPtr2 = IRBuilderHelper::createPtrToInt(lowerer, p2, "p2.int");

    // Subtract and return
    return IRBuilderHelper::createSub(lowerer, intPtr1, intPtr2, "ptrdiff");
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicToRef(IRLowering& lowerer,
                                                       IntrinsicCallExprAST* intrinsic) {
    // #toRef(ptr) - assert non-null and convert raw pointer to reference
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "toRef requires one argument");
    }

    auto* ptr = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
    if (!ptr) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower toRef argument");
    }

    // Ensure it's a pointer
    if (!ptr->getType()->isPointerTy()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "toRef requires a pointer argument");
    }

    // FIXME: Add non-null assertion
    // For now, just return the pointer as a typed pointer
    // The reference type &T is represented as T* in LLVM
    return ptr;
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicToPtr(IRLowering& lowerer,
                                                       IntrinsicCallExprAST* intrinsic) {
    // #toPtr(ref) - convert reference back to raw pointer
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "toPtr requires one argument");
    }

    auto* ref = IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
    if (!ref) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower toPtr argument");
    }

    // Ensure it's a pointer (reference)
    if (!ref->getType()->isPointerTy()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "toPtr requires a reference (pointer) argument");
    }

    // Raw pointers are represented as i8*
    auto* rawPtrType = llvm::PointerType::getUnqual(lowerer.getContext());
    return lowerer.getBuilder().CreateBitCast(ref, rawPtrType, "toptr");
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicAlignof(IRLowering& lowerer,
                                                         IntrinsicCallExprAST* intrinsic) {
    // #alignof(T) - returns the alignment of a type
    // FIXME: Need proper alignment calculation
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 8);
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicBitcast(IRLowering& lowerer,
                                                         IntrinsicCallExprAST* intrinsic) {
    // #bitcast(T, x) - bitcast value to type T
    if (intrinsic->args.size() < 2) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "bitcast requires two arguments: type and value");
    }

    // FIXME: Need to get the target type from the first argument
    // The type is a type parameter, not a value
    // For now, assume the type is stored in the resolvedType of the first argument

    auto* value = IRExprLowering::lowerExpr(lowerer, intrinsic->args[1]);
    if (!value) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "Failed to lower bitcast value argument");
    }

    // FIXME: Get target type from the first argument
    // For now, just return the value
    return value;
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicLikely(IRLowering& lowerer,
                                                        IntrinsicCallExprAST* intrinsic) {
    // #likely(expr) - hint that expr is usually true
    // This is implemented as branch weight metadata
    // For now, just return the expression value
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "likely requires one argument");
    }

    return IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
}

llvm::Value* IRIntrinsicLowering::lowerIntrinsicUnlikely(IRLowering& lowerer,
                                                          IntrinsicCallExprAST* intrinsic) {
    // #unlikely(expr) - hint that expr is usually false
    // This is implemented as branch weight metadata
    // For now, just return the expression value
    if (intrinsic->args.empty()) {
        throw IRLoweringError(IRLoweringError::Kind::IntrinsicNotFound,
                              "unlikely requires one argument");
    }

    return IRExprLowering::lowerExpr(lowerer, intrinsic->args[0]);
}