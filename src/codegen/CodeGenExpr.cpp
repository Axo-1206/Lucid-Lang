/// @file CodeGenExpr.cpp
/// @brief Implementation of expression lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "CodeGenGeneric.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenHelpers.hpp"
#include "support/CodeGenPanic.hpp"
#include "support/LLVMHelpers.hpp"
#include "support/RuntimeError.hpp"
#include "intrinsic/IntrinsicEmitter.hpp"
#include "../sema/types/SemaCompare.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "closure/CodeGenClosure.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <cmath>
#include <unordered_map>

namespace codegen {

// ─── Helper: Convert Value* vector to Constant* vector ───────────────────

static std::vector<llvm::Constant*> toConstants(const std::vector<llvm::Value*>& values) {
    std::vector<llvm::Constant*> constants;
    constants.reserve(values.size());
    for (llvm::Value* v : values) {
        if (llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(v)) {
            constants.push_back(c);
        } else {
            constants.push_back(nullptr);
        }
    }
    return constants;
}

// ─── Helper: Get the element type from an array type ─────────────────────

static llvm::Type* getArrayElementType(CodeGenContext& ctx, const TypeAST* arrayType) {
    if (!arrayType) return nullptr;
    const ArrayTypeAST* arr = arrayType->as<ArrayTypeAST>();
    if (!arr) return nullptr;
    return getType(ctx, arr->element);
}

// =============================================================================
// Expression Lowering - Dispatch
// =============================================================================

llvm::Value* lowerExpression(ExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteralExpr(expr->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:
            return lowerIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
        case ASTKind::ArrayLiteralExpr:
            return lowerArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
        case ASTKind::StructLiteralExpr:
            return lowerStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
        case ASTKind::BinaryExpr:
            return lowerBinaryExpr(expr->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:
            return lowerUnaryExpr(expr->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:
            return lowerCallExpr(expr->as<CallExprAST>(), ctx);
        case ASTKind::IntrinsicCallExpr:
            return lowerIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx);
        case ASTKind::IndexExpr:
            return lowerIndexExpr(expr->as<IndexExprAST>(), ctx);
        case ASTKind::SliceExpr:
            return lowerSliceExpr(expr->as<SliceExprAST>(), ctx);
        case ASTKind::FieldAccessExpr:
            return lowerFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
        case ASTKind::ModuleAccessExpr:
            return lowerModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx);
        case ASTKind::NullCoalesceExpr:
            return lowerNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
        case ASTKind::AssignExpr:
            return lowerAssignExpr(expr->as<AssignExprAST>(), ctx);
        case ASTKind::PipelineExpr:
            return lowerPipelineExpr(expr->as<PipelineExprAST>(), ctx);
        case ASTKind::ComposeExpr:
            return lowerComposeExpr(expr->as<ComposeExprAST>(), ctx);
        case ASTKind::AnonFuncExpr:
            return lowerAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
        case ASTKind::IfExpr:
            return lowerIfExpr(expr->as<IfExprAST>(), ctx);
        case ASTKind::RangeExpr:
            return lowerRangeExpr(expr->as<RangeExprAST>(), ctx);
        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unsupported expression kind: ",
                                    debug::kindToString(expr->kind));
            return nullptr;
    }
}

// =============================================================================
// Literal Expression
// =============================================================================

llvm::Value* lowerLiteralExpr(LiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Type* type = getType(ctx, expr->resolvedType);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "literal has no type");
        return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->kind) {
        case LiteralKind::True:
            result = llvm::ConstantInt::get(type, 1);
            break;

        case LiteralKind::False:
            result = llvm::ConstantInt::get(type, 0);
            break;

        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string valStr = ctx.pool.lookup(expr->value);
            int64_t val = 0;
            try {
                if (expr->kind == LiteralKind::Hex) {
                    val = std::stoll(valStr, nullptr, 16);
                } else if (expr->kind == LiteralKind::Binary) {
                    val = std::stoll(valStr, nullptr, 2);
                } else {
                    val = std::stoll(valStr, nullptr, 10);
                }
            } catch (const std::exception& e) {
                ctx.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                                        "invalid integer literal: ", valStr);
                return nullptr;
            }

            result = llvm::ConstantInt::get(type, val);
            break;
        }

        case LiteralKind::Float: {
            std::string valStr = ctx.pool.lookup(expr->value);
            double val = 0.0;
            try {
                val = std::stod(valStr);
            } catch (const std::exception& e) {
                ctx.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                                        "invalid float literal: ", valStr);
                return nullptr;
            }

            if (type->isFloatTy()) {
                result = llvm::ConstantFP::get(type, static_cast<float>(val));
            } else if (type->isDoubleTy()) {
                result = llvm::ConstantFP::get(type, val);
            } else {
                result = llvm::ConstantFP::get(type, val);
            }
            break;
        }

        case LiteralKind::String:
        case LiteralKind::RawString: {
            std::string valStr = ctx.pool.lookup(expr->value);
            llvm::Constant* strConst = llvm::ConstantDataArray::getString(
                ctx.llvmCtx,
                valStr,
                true
            );

            llvm::GlobalVariable* global = new llvm::GlobalVariable(
                *ctx.module,
                strConst->getType(),
                true,
                llvm::GlobalValue::PrivateLinkage,
                strConst,
                ".str"
            );

            result = ctx.builder.CreatePointerCast(
                global,
                llvm::PointerType::get(ctx.llvmCtx, 0)
            );
            break;
        }

        case LiteralKind::Char: {
            std::string valStr = ctx.pool.lookup(expr->value);
            if (valStr.empty()) {
                result = llvm::ConstantInt::get(type, 0);
            } else {
                result = llvm::ConstantInt::get(type, valStr[0]);
            }
            break;
        }

        case LiteralKind::Nil:
        case LiteralKind::Err: {
            result = llvm::Constant::getNullValue(type);
            break;
        }

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unknown literal kind");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Identifier Expression
// =============================================================================

llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    if (ctx.pool.lookupView(expr->name) == "_") {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "cannot use '_' as a value");
        return nullptr;
    }

    // ─── TODO: Implement proper identifier lookup ──────────────────────────
    // This should look up the variable/function from the context's symbol table.
    // For now, return a placeholder.
    llvm::Type* llvmType = getType(ctx, expr->resolvedType);
    if (!llvmType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "identifier '", ctx.pool.lookup(expr->name), "' has no type");
        return nullptr;
    }

    expr->llvmValue = llvm::Constant::getNullValue(llvmType);
    return expr->llvmValue;
}

// =============================================================================
// Array Literal Expression
// =============================================================================

llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Type* arrayType = getType(ctx, expr->resolvedType);
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "array literal has no type");
        return nullptr;
    }

    if (expr->elements.empty()) {
        return llvm::Constant::getNullValue(arrayType);
    }

    std::vector<llvm::Value*> elements;
    for (ExprAST* elem : expr->elements) {
        llvm::Value* elemValue = lowerExpression(elem, ctx);
        if (!elemValue) {
            return nullptr;
        }
        elements.push_back(elemValue);
    }

    const ArrayTypeAST* arrayTypeAST = expr->resolvedType->as<ArrayTypeAST>();

    if (arrayTypeAST->isFixed()) {
        std::vector<llvm::Constant*> constantElements;
        for (llvm::Value* elem : elements) {
            if (llvm::Constant* c = llvm::dyn_cast<llvm::Constant>(elem)) {
                constantElements.push_back(c);
            } else {
                constantElements.push_back(llvm::Constant::getNullValue(
                    llvm::cast<llvm::ArrayType>(arrayType)->getElementType()
                ));
            }
        }
        return llvm::ConstantArray::get(
            llvm::cast<llvm::ArrayType>(arrayType),
            llvm::ArrayRef<llvm::Constant*>(constantElements)
        );
    } else {
        // Dynamic array: allocate memory and store elements
        if (elements.empty()) {
            return llvm::Constant::getNullValue(arrayType);
        }
        return elements[0];
    }
}

// =============================================================================
// Struct Literal Expression
// =============================================================================

llvm::Value* lowerStructLiteralExpr(StructLiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "struct literal not fully implemented in CodeGen");
    return nullptr;
}

// =============================================================================
// Binary Expression
// =============================================================================

llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* left = lowerExpression(expr->left, ctx);
    llvm::Value* right = lowerExpression(expr->right, ctx);
    if (!left || !right) {
        return nullptr;
    }

    if (expr->left->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->left->resolvedType);
        if (elemType) {
            left = loadIfNeeded(left, elemType, ctx);
        } else {
            left = loadIfNeeded(left, true, ctx);
        }
    }
    if (expr->right->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->right->resolvedType);
        if (elemType) {
            right = loadIfNeeded(right, elemType, ctx);
        } else {
            right = loadIfNeeded(right, true, ctx);
        }
    }
    if (!left || !right) {
        return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        // ─── Arithmetic Operators ────────────────────────────────────────
        case BinaryOp::Add:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateAdd(left, right, "add");
            } else {
                result = ctx.builder.CreateFAdd(left, right, "fadd");
            }
            break;

        case BinaryOp::Sub:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateSub(left, right, "sub");
            } else {
                result = ctx.builder.CreateFSub(left, right, "fsub");
            }
            break;

        case BinaryOp::Mul:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateMul(left, right, "mul");
            } else {
                result = ctx.builder.CreateFMul(left, right, "fmul");
            }
            break;

        case BinaryOp::Div: {
            if (isIntegerType(left->getType()) && isIntegerType(right->getType())) {
                // Check for division by zero
                RuntimeErrorKind kind = RuntimeErrorKind::DivisionByZero;
                llvm::Value* checkedRight = emitZeroCheck(right, kind, ctx);
                if (!checkedRight) return nullptr;
                result = ctx.builder.CreateSDiv(left, checkedRight, "sdiv");
            } else {
                result = ctx.builder.CreateFDiv(left, right, "fdiv");
            }
            break;
        }

        case BinaryOp::Mod: {
            if (isIntegerType(left->getType()) && isIntegerType(right->getType())) {
                RuntimeErrorKind kind = RuntimeErrorKind::ModuloByZero;
                llvm::Value* checkedRight = emitZeroCheck(right, kind, ctx);
                if (!checkedRight) return nullptr;
                result = ctx.builder.CreateSRem(left, checkedRight, "srem");
            } else {
                result = ctx.builder.CreateFRem(left, right, "frem");
            }
            break;
        }

        case BinaryOp::Pow: {
            llvm::Type* leftType = left->getType();
            llvm::Type* rightType = right->getType();

            if (isIntegerType(leftType) && isIntegerType(rightType)) {
                left = ctx.builder.CreateSIToFP(left, llvm::Type::getDoubleTy(ctx.llvmCtx));
                right = ctx.builder.CreateSIToFP(right, llvm::Type::getDoubleTy(ctx.llvmCtx));
            }

            result = emitIntrinsic("pow", {left, right}, nullptr, ctx);
            break;
        }

        // ─── Comparison Operators ────────────────────────────────────────
        case BinaryOp::Eq:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpEQ(left, right, "eq");
            } else {
                result = ctx.builder.CreateFCmpUEQ(left, right, "feq");
            }
            break;

        case BinaryOp::Ne:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpNE(left, right, "ne");
            } else {
                result = ctx.builder.CreateFCmpUNE(left, right, "fne");
            }
            break;

        case BinaryOp::Lt:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSLT(left, right, "slt");
            } else {
                result = ctx.builder.CreateFCmpULT(left, right, "flt");
            }
            break;

        case BinaryOp::Gt:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSGT(left, right, "sgt");
            } else {
                result = ctx.builder.CreateFCmpUGT(left, right, "fgt");
            }
            break;

        case BinaryOp::Le:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSLE(left, right, "sle");
            } else {
                result = ctx.builder.CreateFCmpULE(left, right, "fle");
            }
            break;

        case BinaryOp::Ge:
            if (isIntegerType(left->getType())) {
                result = ctx.builder.CreateICmpSGE(left, right, "sge");
            } else {
                result = ctx.builder.CreateFCmpUGE(left, right, "fge");
            }
            break;

        // ─── Logical Operators ──────────────────────────────────────────
        case BinaryOp::And:
            if (!isBoolValue(left)) {
                left = ctx.builder.CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()));
            }
            if (!isBoolValue(right)) {
                right = ctx.builder.CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()));
            }
            result = ctx.builder.CreateAnd(left, right, "and");
            break;

        case BinaryOp::Or:
            if (!isBoolValue(left)) {
                left = ctx.builder.CreateICmpNE(left, llvm::Constant::getNullValue(left->getType()));
            }
            if (!isBoolValue(right)) {
                right = ctx.builder.CreateICmpNE(right, llvm::Constant::getNullValue(right->getType()));
            }
            result = ctx.builder.CreateOr(left, right, "or");
            break;

        // ─── Bitwise Operators ──────────────────────────────────────────
        case BinaryOp::BitAnd:
            result = ctx.builder.CreateAnd(left, right, "band");
            break;

        case BinaryOp::BitOr:
            result = ctx.builder.CreateOr(left, right, "bor");
            break;

        case BinaryOp::BitXor:
            result = ctx.builder.CreateXor(left, right, "bxor");
            break;

        case BinaryOp::Shl:
            result = ctx.builder.CreateShl(left, right, "shl");
            break;

        case BinaryOp::Shr:
            result = ctx.builder.CreateAShr(left, right, "ashr");
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidBinary, expr->loc,
                                    "unknown binary operator");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Unary Expression
// =============================================================================

llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* operand = lowerExpression(expr->operand, ctx);
    if (!operand) {
        return nullptr;
    }

    if (expr->operand->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->operand->resolvedType);
        if (elemType) {
            operand = loadIfNeeded(operand, elemType, ctx);
        } else {
            operand = loadIfNeeded(operand, true, ctx);
        }
        if (!operand) return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        case UnaryOp::Neg:
            if (isIntegerType(operand->getType())) {
                result = ctx.builder.CreateNeg(operand, "neg");
            } else {
                result = ctx.builder.CreateFNeg(operand, "fneg");
            }
            break;

        case UnaryOp::Not:
            if (!isBoolValue(operand)) {
                operand = ctx.builder.CreateICmpNE(operand,
                    llvm::Constant::getNullValue(operand->getType()));
            }
            result = ctx.builder.CreateNot(operand, "not");
            break;

        case UnaryOp::BitNot:
            result = ctx.builder.CreateNot(operand, "bnot");
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unknown unary operator");
            return nullptr;
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Call Expression
// =============================================================================

llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Function* callee = nullptr;
    llvm::Value* calleeVal = lowerExpression(expr->callee, ctx);
    if (calleeVal) {
        callee = llvm::dyn_cast<llvm::Function>(calleeVal);
    }

    if (!callee) {
        ctx.diagnostics.errorAt(DiagCode::Sem_NotCallable, expr->callee->loc,
                                "callee is not callable");
        return nullptr;
    }

    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (elemType) {
                argVal = loadIfNeeded(argVal, elemType, ctx);
            } else {
                argVal = loadIfNeeded(argVal, true, ctx);
            }
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    llvm::Value* result = ctx.builder.CreateCall(callee, args, "call");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Intrinsic Call Expression
// =============================================================================

llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    return emitIntrinsicFromAST(expr, ctx);
}

// =============================================================================
// Index Expression
// =============================================================================

llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    llvm::Value* index = lowerExpression(expr->index, ctx);
    if (!index) {
        return nullptr;
    }

    if (expr->index->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->index->resolvedType);
        if (elemType) {
            index = loadIfNeeded(index, elemType, ctx);
        } else {
            index = loadIfNeeded(index, true, ctx);
        }
        if (!index) return nullptr;
    }

    const ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (!elemType) {
        return nullptr;
    }

    // ─── Get pointer to array data ──────────────────────────────────────
    llvm::Value* ptr = target;
    if (arrayType->isFixed()) {
        ptr = ctx.builder.CreateConstGEP2_32(elemType, target, 0, 0);
    }

    // ─── Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(target, arrayType, ctx);
    if (!len) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "could not determine array length");
        return nullptr;
    }

    // ─── Bounds check ──────────────────────────────────────────────────
    llvm::Value* checkedIndex = emitBoundsCheck(index, len, ctx);
    if (!checkedIndex) return nullptr;

    // ─── GEP and load ──────────────────────────────────────────────────
    llvm::Value* gep = ctx.builder.CreateGEP(elemType, ptr, checkedIndex, "array_idx");

    if (expr->isLValue) {
        expr->llvmValue = gep;
        return gep;
    }

    llvm::Value* result = ctx.builder.CreateLoad(elemType, gep, "array_load");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Slice Expression
// =============================================================================

llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    const ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    // ─── Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(target, arrayType, ctx);
    if (!len) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "could not determine array length");
        return nullptr;
    }

    // ─── Resolve start and end bounds ──────────────────────────────────
    llvm::Value* start = nullptr;
    llvm::Value* end = nullptr;

    if (expr->start) {
        start = lowerExpression(expr->start, ctx);
        if (!start) return nullptr;
        if (expr->start->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->start->resolvedType);
            if (elemType) {
                start = loadIfNeeded(start, elemType, ctx);
            } else {
                start = loadIfNeeded(start, true, ctx);
            }
            if (!start) return nullptr;
        }
        if (start->getType() != len->getType()) {
            start = ctx.builder.CreateIntCast(start, len->getType(), true, "start_cast");
        }
    } else {
        start = llvm::ConstantInt::get(len->getType(), 0);
    }

    if (expr->end) {
        end = lowerExpression(expr->end, ctx);
        if (!end) return nullptr;
        if (expr->end->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->end->resolvedType);
            if (elemType) {
                end = loadIfNeeded(end, elemType, ctx);
            } else {
                end = loadIfNeeded(end, true, ctx);
            }
            if (!end) return nullptr;
        }
        if (end->getType() != len->getType()) {
            end = ctx.builder.CreateIntCast(end, len->getType(), true, "end_cast");
        }
    } else {
        end = len;
    }

    // ─── Slice bounds check ──────────────────────────────────────────────
    auto [checkedStart, checkedEnd] = emitSliceBoundsCheck(start, end, len, ctx);
    if (!checkedStart || !checkedEnd) return nullptr;

    // ─── Get data pointer ──────────────────────────────────────────────────
    llvm::Value* dataPtr = target;
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (arrayType->isFixed()) {
        dataPtr = ctx.builder.CreateConstGEP2_32(elemType, target, 0, 0);
    }

    // ─── Offset data pointer by start ──────────────────────────────────────
    llvm::Value* slicePtr = ctx.builder.CreateGEP(elemType, dataPtr, checkedStart, "slice_ptr");

    // ─── Calculate length: end - start ──────────────────────────────────────
    llvm::Value* sliceLen = ctx.builder.CreateSub(checkedEnd, checkedStart, "slice_len");

    // ─── Calculate capacity: len - start ────────────────────────────────────
    llvm::Value* sliceCap = ctx.builder.CreateSub(len, checkedStart, "slice_cap");

    // ─── Build the slice struct ─────────────────────────────────────────────
    llvm::StructType* sliceType = llvm::cast<llvm::StructType>(getType(ctx, expr->resolvedType));
    if (!sliceType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->loc,
                                "could not create slice type");
        return nullptr;
    }

    llvm::Value* slice = llvm::UndefValue::get(sliceType);
    slice = ctx.builder.CreateInsertValue(slice, slicePtr, 0);
    slice = ctx.builder.CreateInsertValue(slice, sliceLen, 1);
    slice = ctx.builder.CreateInsertValue(slice, sliceCap, 2);

    expr->llvmValue = slice;
    return slice;
}

// =============================================================================
// Field Access Expression
// =============================================================================

llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "field access not fully implemented in CodeGen");
    return nullptr;
}

// =============================================================================
// Module Access Expression
// =============================================================================

llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "module access not fully implemented in CodeGen");
    return nullptr;
}

// =============================================================================
// Null Coalesce Expression
// =============================================================================

llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* lhs = lowerExpression(expr->value, ctx);
    if (!lhs) {
        return nullptr;
    }

    llvm::Value* rhs = lowerExpression(expr->fallback, ctx);
    if (!rhs) {
        return nullptr;
    }

    const TypeAST* lhsType = expr->value->resolvedType;
    if (!lhsType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->value->loc,
                                "LHS has no type");
        return nullptr;
    }

    if (!sema::isNullableType(lhsType) && !sema::isFallibleType(lhsType)) {
        expr->llvmValue = lhs;
        return lhs;
    }

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* lhsBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "then", func);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "else", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "merge", func);

    // ─── Check the tag ────────────────────────────────────────────────────
    llvm::Value* tag = ctx.builder.CreateExtractValue(lhs, 0);
    llvm::Value* isNil = ctx.builder.CreateICmpEQ(
        tag,
        llvm::ConstantInt::get(tag->getType(), 0),
        "is_nil"
    );

    ctx.builder.CreateCondBr(isNil, elseBlock, thenBlock);

    // ─── Then block (LHS is valid) ──────────────────────────────────────
    ctx.builder.SetInsertPoint(thenBlock);
    llvm::Value* lhsValid = ctx.builder.CreateExtractValue(lhs, 1);
    ctx.builder.CreateBr(mergeBlock);

    // ─── Else block (LHS is nil/err) ────────────────────────────────────
    ctx.builder.SetInsertPoint(elseBlock);
    llvm::Value* rhsValue = rhs;
    if (expr->fallback->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->fallback->resolvedType);
        if (elemType) {
            rhsValue = loadIfNeeded(rhsValue, elemType, ctx);
        } else {
            rhsValue = loadIfNeeded(rhsValue, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(
        lhsValid->getType(),
        2,
        "coalesce"
    );
    phi->addIncoming(lhsValid, thenBlock);
    phi->addIncoming(rhsValue, elseBlock);

    expr->llvmValue = phi;
    return phi;
}

// =============================================================================
// Assignment Expression
// =============================================================================

llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* lhs = lowerExpression(expr->lhs, ctx);
    if (!lhs) {
        return nullptr;
    }

    if (!expr->lhs->isLValue) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->lhs->loc,
                                "assignment target is not an l-value");
        return nullptr;
    }

    llvm::Value* rhs = lowerExpression(expr->rhs, ctx);
    if (!rhs) {
        return nullptr;
    }

    if (expr->rhs->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->rhs->resolvedType);
        if (elemType) {
            rhs = loadIfNeeded(rhs, elemType, ctx);
        } else {
            rhs = loadIfNeeded(rhs, true, ctx);
        }
        if (!rhs) return nullptr;
    }

    // ─── Handle compound assignment ──────────────────────────────────────
    if (expr->op != AssignOp::Assign) {
        llvm::Type* elemType = getType(ctx, expr->lhs->resolvedType);
        llvm::Value* lhsValue = loadIfNeeded(lhs, elemType, ctx);
        if (!lhsValue) {
            return nullptr;
        }

        switch (expr->op) {
            case AssignOp::AddAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateAdd(lhsValue, rhs, "add");
                } else {
                    rhs = ctx.builder.CreateFAdd(lhsValue, rhs, "fadd");
                }
                break;
            case AssignOp::SubAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateSub(lhsValue, rhs, "sub");
                } else {
                    rhs = ctx.builder.CreateFSub(lhsValue, rhs, "fsub");
                }
                break;
            case AssignOp::MulAssign:
                if (isIntegerType(lhsValue->getType())) {
                    rhs = ctx.builder.CreateMul(lhsValue, rhs, "mul");
                } else {
                    rhs = ctx.builder.CreateFMul(lhsValue, rhs, "fmul");
                }
                break;
            case AssignOp::DivAssign:
                if (isIntegerType(lhsValue->getType())) {
                    // Check for division by zero
                    RuntimeErrorKind kind = RuntimeErrorKind::DivisionByZero;
                    llvm::Value* checkedDivisor = emitZeroCheck(rhs, kind, ctx);
                    if (!checkedDivisor) return nullptr;
                    rhs = ctx.builder.CreateSDiv(lhsValue, checkedDivisor, "sdiv");
                } else {
                    rhs = ctx.builder.CreateFDiv(lhsValue, rhs, "fdiv");
                }
                break;
            default:
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->loc,
                                        "unsupported compound assignment");
                return nullptr;
        }
    }

    ctx.builder.CreateStore(rhs, lhs);
    expr->llvmValue = rhs;
    return rhs;
}

// =============================================================================
// Pipeline Expression
// =============================================================================

llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* currentValue = lowerExpression(expr->seed, ctx);
    if (!currentValue) {
        return nullptr;
    }

    if (expr->seed->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->seed->resolvedType);
        if (elemType) {
            currentValue = loadIfNeeded(currentValue, elemType, ctx);
        } else {
            currentValue = loadIfNeeded(currentValue, true, ctx);
        }
    }

    for (PipelineStepAST* step : expr->steps) {
        currentValue = lowerPipelineStep(step, ctx);
        if (!currentValue) {
            return nullptr;
        }
    }

    expr->llvmValue = currentValue;
    return currentValue;
}

llvm::Value* lowerPipelineStep(PipelineStepAST* step, CodeGenContext& ctx) {
    if (!step) return nullptr;

    llvm::Value* callable = lowerExpression(step->callable, ctx);
    if (!callable) {
        return nullptr;
    }

    if (step->callable->isLValue) {
        llvm::Type* elemType = getType(ctx, step->callable->resolvedType);
        if (elemType) {
            callable = loadIfNeeded(callable, elemType, ctx);
        } else {
            callable = loadIfNeeded(callable, true, ctx);
        }
    }

    std::vector<llvm::Value*> args;
    for (ExprAST* arg : step->packArgs) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (elemType) {
                argVal = loadIfNeeded(argVal, elemType, ctx);
            } else {
                argVal = loadIfNeeded(argVal, true, ctx);
            }
        }
        args.push_back(argVal);
    }

    // ─── Build function type ──────────────────────────────────────────────
    std::vector<llvm::Type*> paramTypes;
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    llvm::Type* returnType = args.empty() ? llvm::Type::getVoidTy(ctx.llvmCtx) : args[0]->getType();
    llvm::FunctionType* fnType = llvm::FunctionType::get(returnType, paramTypes, false);

    llvm::Value* typedFunc = ctx.builder.CreatePointerCast(
        callable,
        llvm::PointerType::get(fnType, 0),
        "pipeline_func_cast"
    );

    llvm::Value* result = ctx.builder.CreateCall(fnType, typedFunc, args, "pipeline_call");
    return result;
}

// =============================================================================
// Composition Expression
// =============================================================================

llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "composition is not fully implemented yet");
    return nullptr;
}

llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx) {
    if (!operand) return nullptr;
    return lowerExpression(operand->callable, ctx);
}

// =============================================================================
// Anonymous Function Expression
// =============================================================================

llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    if (expr->hasClosure || !expr->captures.empty()) {
        return lowerClosure(expr, ctx);
    }

    return llvm::Constant::getNullValue(llvm::PointerType::get(ctx.llvmCtx, 0));
}

// =============================================================================
// If Expression
// =============================================================================

llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* cond = lowerExpression(expr->condition, ctx);
    if (!cond) {
        return nullptr;
    }

    if (!isBoolValue(cond)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_then", func);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_else", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_merge", func);

    ctx.builder.CreateCondBr(cond, thenBlock, elseBlock);

    // ─── Then branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(thenBlock);
    llvm::Value* thenVal = lowerExpression(expr->thenBranch, ctx);
    if (!thenVal) {
        return nullptr;
    }
    if (expr->thenBranch->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->thenBranch->resolvedType);
        if (elemType) {
            thenVal = loadIfNeeded(thenVal, elemType, ctx);
        } else {
            thenVal = loadIfNeeded(thenVal, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Else branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(elseBlock);
    llvm::Value* elseVal = lowerExpression(expr->elseBranch, ctx);
    if (!elseVal) {
        return nullptr;
    }
    if (expr->elseBranch->isLValue) {
        llvm::Type* elemType = getType(ctx, expr->elseBranch->resolvedType);
        if (elemType) {
            elseVal = loadIfNeeded(elseVal, elemType, ctx);
        } else {
            elseVal = loadIfNeeded(elseVal, true, ctx);
        }
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(thenVal->getType(), 2, "if");
    phi->addIncoming(thenVal, thenBlock);
    phi->addIncoming(elseVal, elseBlock);

    expr->llvmValue = phi;
    return phi;
}

// =============================================================================
// Range Expression
// =============================================================================

llvm::Value* lowerRangeExpr(RangeExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "range expression should not be used as a value");
    return nullptr;
}

} // namespace codegen