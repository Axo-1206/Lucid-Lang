/// @file CodeGenExpr.cpp
/// @brief Implementation of expression lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>

#include <cmath>

namespace codegen {

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

    // ─── Get the LLVM type from the semantic type ─────────────────────────
    llvm::Type* type = getType(ctx, expr->semanticType);
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
            // Parse the integer value
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
            // String literals are represented as global constants
            // that are pointer to the string data
            llvm::Constant* strConst = llvm::ConstantDataArray::getString(
                ctx.llvmCtx,
                valStr,
                true // Null terminate
            );

            llvm::GlobalVariable* global = new llvm::GlobalVariable(
                *ctx.module,
                strConst->getType(),
                true, // const
                llvm::GlobalValue::PrivateLinkage,
                strConst,
                ".str"
            );

            // Get a pointer to the string
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
            // Nil/Err are represented as a null pointer or zero value
            // For nullable/fallible types, the tag determines the state
            // For now, just return null
            result = llvm::Constant::getNullValue(type);
            break;
        }

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, expr->loc,
                                    "unknown literal kind");
            return nullptr;
    }

    // ─── Store the result on the expression ──────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Identifier Expression
// =============================================================================

llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Special case: `_` is the discard placeholder ──────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        // `_` is a placeholder, not a real value - it should not be used
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "cannot use '_' as a value");
        return nullptr;
    }

    // ─── Look up the declaration ──────────────────────────────────────────
    const ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        // This should have been caught by Sema
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "undefined value '", ctx.pool.lookup(expr->name), "'");
        return nullptr;
    }

    // ─── Get the LLVM value from the context ──────────────────────────────
    llvm::Value* value = ctx.lookupValue(decl);
    if (!value) {
        // If not found, maybe it's a function or global
        if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
            llvm::Function* func = ctx.lookupFunction(funcDecl);
            if (func) {
                value = func;
            }
        } else if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* varDecl = decl->as<VarDeclAST>();
            if (varDecl->llvmGlobal) {
                value = varDecl->llvmGlobal;
            } else if (varDecl->llvmAlloca) {
                value = varDecl->llvmAlloca;
            }
        }
    }

    if (!value) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "no LLVM value for '", ctx.pool.lookup(expr->name), "'");
        return nullptr;
    }

    // ─── Store the result on the expression ──────────────────────────────
    expr->llvmValue = value;

    // ─── If this is an l-value, return the address ────────────────────────
    // The caller will load it if needed
    if (expr->isLValue) {
        return value;
    }

    // ─── Otherwise, load the value ────────────────────────────────────────
    return loadIfNeeded(value, expr->isLValue, ctx);
}

// =============================================================================
// Array Literal Expression
// =============================================================================

llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Get the array type ──────────────────────────────────────────────────
    llvm::Type* arrayType = getType(ctx, expr->semanticType);
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "array literal has no type");
        return nullptr;
    }

    // ─── Empty array ──────────────────────────────────────────────────────
    if (expr->elements.empty()) {
        return llvm::Constant::getNullValue(arrayType);
    }

    // ─── Lower each element ──────────────────────────────────────────────
    std::vector<llvm::Value*> elements;
    for (ExprAST* elem : expr->elements) {
        llvm::Value* elemValue = lowerExpression(elem, ctx);
        if (!elemValue) {
            return nullptr;
        }
        elements.push_back(elemValue);
    }

    // ─── Create the array constant ────────────────────────────────────────
    // For fixed arrays, we can create a constant array
    // For dynamic arrays, we need to allocate memory
    const ArrayTypeAST* arrayTypeAST = expr->semanticType->as<ArrayTypeAST>();

    if (arrayTypeAST->isFixed()) {
        // Fixed array: create a constant array
        return llvm::ConstantArray::get(
            llvm::cast<llvm::ArrayType>(arrayType),
            elements
        );
    } else {
        // Dynamic array: allocate memory and store elements
        // For now, just use the first element or null
        // TODO: Proper dynamic array allocation
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

    // ─── Get the struct type ──────────────────────────────────────────────────
    llvm::StructType* structType = llvm::dyn_cast<llvm::StructType>(
        getType(ctx, expr->semanticType)
    );
    if (!structType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "struct literal has invalid type");
        return nullptr;
    }

    // ─── Look up the struct declaration ──────────────────────────────────
    const TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl || !typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedType, expr->loc,
                                "struct '", ctx.pool.lookup(expr->typeName), "' not found");
        return nullptr;
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── Build field map ──────────────────────────────────────────────────
    std::unordered_map<InternedString, llvm::Value*> fieldValues;
    for (const FieldInitAST* init : expr->inits) {
        llvm::Value* fieldValue = lowerExpression(init->value, ctx);
        if (!fieldValue) {
            return nullptr;
        }
        fieldValues[init->name] = fieldValue;
    }

    // ─── Create struct value ──────────────────────────────────────────────
    // For each field, use the provided value or default
    std::vector<llvm::Value*> fieldValuesVec;
    fieldValuesVec.reserve(structDecl->fields.size());

    for (const FieldDeclAST* field : structDecl->fields) {
        auto it = fieldValues.find(field->name);
        if (it != fieldValues.end()) {
            fieldValuesVec.push_back(it->second);
        } else if (field->defaultVal) {
            // Lower the default value
            llvm::Value* defaultVal = lowerExpression(field->defaultVal, ctx);
            if (!defaultVal) {
                return nullptr;
            }
            fieldValuesVec.push_back(defaultVal);
        } else {
            // No value provided and no default - use null
            llvm::Type* fieldType = getType(ctx, field->type);
            if (fieldType) {
                fieldValuesVec.push_back(llvm::Constant::getNullValue(fieldType));
            } else {
                fieldValuesVec.push_back(llvm::Constant::getNullValue(
                    llvm::Type::getInt8Ty(ctx.llvmCtx)
                ));
            }
        }
    }

    // ─── Create the struct constant ──────────────────────────────────────
    return llvm::ConstantStruct::get(structType, fieldValuesVec);
}

// =============================================================================
// Binary Expression
// =============================================================================

llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower operands ────────────────────────────────────────────────────
    llvm::Value* left = lowerExpression(expr->left, ctx);
    llvm::Value* right = lowerExpression(expr->right, ctx);
    if (!left || !right) {
        return nullptr;
    }

    // ─── If operands are l-values, load them ─────────────────────────────
    if (expr->left->isLValue) {
        left = loadIfNeeded(left, expr->left->isLValue, ctx);
        if (!left) return nullptr;
    }
    if (expr->right->isLValue) {
        right = loadIfNeeded(right, expr->right->isLValue, ctx);
        if (!right) return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        // ─── Arithmetic Operators ────────────────────────────────────────
        case BinaryOp::Add:
            if (left->getType()->isIntegerTy()) {
                result = ctx.builder.CreateAdd(left, right, "add");
            } else {
                result = ctx.builder.CreateFAdd(left, right, "fadd");
            }
            break;

        case BinaryOp::Sub:
            if (left->getType()->isIntegerTy()) {
                result = ctx.builder.CreateSub(left, right, "sub");
            } else {
                result = ctx.builder.CreateFSub(left, right, "fsub");
            }
            break;

        case BinaryOp::Mul:
            if (left->getType()->isIntegerTy()) {
                result = ctx.builder.CreateMul(left, right, "mul");
            } else {
                result = ctx.builder.CreateFMul(left, right, "fmul");
            }
            break;

        case BinaryOp::Div:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateSDiv(left, right, "sdiv");
                } else {
                    result = ctx.builder.CreateUDiv(left, right, "udiv");
                }
            } else {
                result = ctx.builder.CreateFDiv(left, right, "fdiv");
            }
            break;

        case BinaryOp::Mod:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateSRem(left, right, "srem");
                } else {
                    result = ctx.builder.CreateURem(left, right, "urem");
                }
            } else {
                result = ctx.builder.CreateFRem(left, right, "frem");
            }
            break;

        case BinaryOp::Pow: {
            // Power operator: use libm's pow function
            // For integers, convert to float
            llvm::Type* leftType = left->getType();
            llvm::Type* rightType = right->getType();

            if (leftType->isIntegerTy() && rightType->isIntegerTy()) {
                // Convert to double
                left = ctx.builder.CreateSIToFP(left, llvm::Type::getDoubleTy(ctx.llvmCtx));
                right = ctx.builder.CreateSIToFP(right, llvm::Type::getDoubleTy(ctx.llvmCtx));
            }

            // Call pow
            result = emitIntrinsicCall("pow", {left, right}, ctx);
            break;
        }

        // ─── Comparison Operators ────────────────────────────────────────
        case BinaryOp::Eq:
            if (left->getType()->isIntegerTy()) {
                result = ctx.builder.CreateICmpEQ(left, right, "eq");
            } else {
                result = ctx.builder.CreateFCmpUEQ(left, right, "feq");
            }
            break;

        case BinaryOp::Ne:
            if (left->getType()->isIntegerTy()) {
                result = ctx.builder.CreateICmpNE(left, right, "ne");
            } else {
                result = ctx.builder.CreateFCmpUNE(left, right, "fne");
            }
            break;

        case BinaryOp::Lt:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateICmpSLT(left, right, "slt");
                } else {
                    result = ctx.builder.CreateICmpULT(left, right, "ult");
                }
            } else {
                result = ctx.builder.CreateFCmpULT(left, right, "flt");
            }
            break;

        case BinaryOp::Gt:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateICmpSGT(left, right, "sgt");
                } else {
                    result = ctx.builder.CreateICmpUGT(left, right, "ugt");
                }
            } else {
                result = ctx.builder.CreateFCmpUGT(left, right, "fgt");
            }
            break;

        case BinaryOp::Le:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateICmpSLE(left, right, "sle");
                } else {
                    result = ctx.builder.CreateICmpULE(left, right, "ule");
                }
            } else {
                result = ctx.builder.CreateFCmpULE(left, right, "fle");
            }
            break;

        case BinaryOp::Ge:
            if (left->getType()->isIntegerTy()) {
                if (left->getType()->isSigned()) {
                    result = ctx.builder.CreateICmpSGE(left, right, "sge");
                } else {
                    result = ctx.builder.CreateICmpUGE(left, right, "uge");
                }
            } else {
                result = ctx.builder.CreateFCmpUGE(left, right, "fge");
            }
            break;

        // ─── Logical Operators ──────────────────────────────────────────
        case BinaryOp::And:
            // Logical AND: both operands are bools
            // Compare to 0 to get bool if needed
            if (!left->getType()->isIntegerTy(1)) {
                left = ctx.builder.CreateICmpNE(left, 
                    llvm::Constant::getNullValue(left->getType()));
            }
            if (!right->getType()->isIntegerTy(1)) {
                right = ctx.builder.CreateICmpNE(right,
                    llvm::Constant::getNullValue(right->getType()));
            }
            result = ctx.builder.CreateAnd(left, right, "and");
            break;

        case BinaryOp::Or:
            // Logical OR
            if (!left->getType()->isIntegerTy(1)) {
                left = ctx.builder.CreateICmpNE(left,
                    llvm::Constant::getNullValue(left->getType()));
            }
            if (!right->getType()->isIntegerTy(1)) {
                right = ctx.builder.CreateICmpNE(right,
                    llvm::Constant::getNullValue(right->getType()));
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
            if (left->getType()->isSigned()) {
                result = ctx.builder.CreateAShr(left, right, "ashr");
            } else {
                result = ctx.builder.CreateLShr(left, right, "lshr");
            }
            break;

        default:
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidBinary, expr->loc,
                                    "unknown binary operator");
            return nullptr;
    }

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Unary Expression
// =============================================================================

llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower operand ────────────────────────────────────────────────────
    llvm::Value* operand = lowerExpression(expr->operand, ctx);
    if (!operand) {
        return nullptr;
    }

    // ─── If operand is an l-value, load it ──────────────────────────────
    if (expr->operand->isLValue) {
        operand = loadIfNeeded(operand, expr->operand->isLValue, ctx);
        if (!operand) return nullptr;
    }

    llvm::Value* result = nullptr;

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand->getType()->isIntegerTy()) {
                result = ctx.builder.CreateNeg(operand, "neg");
            } else {
                result = ctx.builder.CreateFNeg(operand, "fneg");
            }
            break;

        case UnaryOp::Not:
            // Logical NOT: convert to bool if needed, then negate
            if (!operand->getType()->isIntegerTy(1)) {
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

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Call Expression
// =============================================================================

llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Resolve the callee ──────────────────────────────────────────────
    const FuncDeclAST* funcDecl = nullptr;
    llvm::Value* callee = nullptr;

    // ─── Check if callee is an identifier (direct function call) ────────
    if (expr->callee->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = expr->callee->as<IdentifierExprAST>();
        const ValueDeclAST* decl = ctx.lookupValue(id->name);
        if (decl && decl->isa<FuncDeclAST>()) {
            funcDecl = decl->as<FuncDeclAST>();
            callee = ctx.lookupFunction(funcDecl);
            if (!callee) {
                // Try to get from the function declaration's llvmFunction
                callee = funcDecl->llvmFunction;
            }
        }
    }
    // ─── Check if callee is a module access ──────────────────────────────
    else if (expr->callee->isa<ModuleAccessExprAST>()) {
        const ModuleAccessExprAST* access = expr->callee->as<ModuleAccessExprAST>();
        const ValueDeclAST* decl = ctx.lookupValueByAlias(access->moduleName, access->memberName);
        if (decl && decl->isa<FuncDeclAST>()) {
            funcDecl = decl->as<FuncDeclAST>();
            callee = ctx.lookupFunction(funcDecl);
            if (!callee) {
                callee = funcDecl->llvmFunction;
            }
        }
    }
    // ─── Check if callee is an expression that returns a function ──────
    else {
        callee = lowerExpression(expr->callee, ctx);
        if (callee) {
            // If the callee is a pointer to a function, load it
            if (callee->getType()->isPointerTy()) {
                callee = loadIfNeeded(callee, true, ctx);
            }
        }
    }

    if (!callee || !funcDecl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_NotCallable, expr->callee->loc,
                                "callee is not callable");
        return nullptr;
    }

    // ─── Get function type ──────────────────────────────────────────────────
    const FuncTypeAST* funcType = funcDecl->funcType;
    if (!funcType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "function has no type");
        return nullptr;
    }

    // ─── Lower arguments ──────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    bool hasVariadic = false;
    size_t variadicIndex = 0;

    // Check if the function has variadic parameters
    for (size_t i = 0; i < funcType->params.size(); ++i) {
        if (funcType->params[i]->isVariadic) {
            hasVariadic = true;
            variadicIndex = i;
            break;
        }
    }

    // Lower each argument
    for (size_t i = 0; i < expr->args.size(); ++i) {
        llvm::Value* arg = lowerExpression(expr->args[i], ctx);
        if (!arg) {
            return nullptr;
        }

        // If argument is an l-value, load it
        if (expr->args[i]->isLValue) {
            arg = loadIfNeeded(arg, expr->args[i]->isLValue, ctx);
        }

        args.push_back(arg);
    }

    // ─── Handle argument pack (!) ──────────────────────────────────────────
    // For argument pack, we already have the arguments from the call site
    // No special handling needed

    // ─── If callee is a closure, pass the environment pointer ────────────
    if (funcDecl->hasClosure) {
        // Get the environment pointer from the closure
        // This is stored when the closure is created
        // For now, we pass null
        // TODO: Get the actual environment pointer
        args.insert(args.begin(), llvm::Constant::getNullValue(
            llvm::PointerType::get(ctx.llvmCtx, 0)
        ));
    }

    // ─── Create the call ──────────────────────────────────────────────────
    llvm::Value* result = ctx.builder.CreateCall(
        llvm::dyn_cast<llvm::Function>(callee),
        args,
        "call"
    );

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Intrinsic Call Expression
// =============================================================================

llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Lower arguments ──────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        // If argument is an l-value, load it
        if (arg->isLValue) {
            argVal = loadIfNeeded(argVal, arg->isLValue, ctx);
        }
        args.push_back(argVal);
    }

    // ─── Dispatch to specific intrinsic handlers ─────────────────────────
    llvm::Value* result = nullptr;

    // ─── Floating-Point Math ──────────────────────────────────────────────
    if (name == "sqrt" || name == "abs" || name == "fma" ||
        name == "ceil" || name == "floor" || name == "round") {
        result = emitLLVMIntrinsic(name, args, ctx);
    }
    // ─── Memory Operations ────────────────────────────────────────────────
    else if (name == "memcpy" || name == "memmove" || name == "memset") {
        result = emitLLVMIntrinsic(name, args, ctx);
    }
    // ─── Bit Manipulation ──────────────────────────────────────────────────
    else if (name == "clz" || name == "ctz" || name == "popcount" || name == "bswap") {
        result = emitLLVMIntrinsic(name, args, ctx);
    }
    // ─── CPU Hints ──────────────────────────────────────────────────────────
    else if (name == "prefetch" || name == "prefetch_r" || name == "prefetch_w") {
        result = emitPrefetch(name, args, ctx);
    }
    else if (name == "fence") {
        result = emitFence(args, ctx);
    }
    else if (name == "pause") {
        result = emitPause(ctx);
    }
    // ─── Atomics ────────────────────────────────────────────────────────────
    else if (name.find("atomic_") == 0) {
        result = emitAtomic(name, args, ctx);
    }
    // ─── Type & Value Inspection ──────────────────────────────────────────
    else if (name == "sizeof") {
        result = emitSizeof(args, ctx);
    }
    else if (name == "alignof") {
        result = emitAlignof(args, ctx);
    }
    else if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr") {
        result = emitTypeString(name, args, ctx);
    }
    else if (name == "addrof") {
        result = emitAddrOf(args, ctx);
    }
    // ─── Pointer Operations ──────────────────────────────────────────────────
    else if (name == "toRef") {
        result = emitToRef(args, ctx);
    }
    else if (name == "toPtr") {
        result = emitToPtr(args, ctx);
    }
    else if (name == "ptrOffset") {
        result = emitPtrOffset(args, ctx);
    }
    else if (name == "ptrDiff") {
        result = emitPtrDiff(args, ctx);
    }
    // ─── Bit Manipulation ──────────────────────────────────────────────────
    else if (name == "bitcast") {
        result = emitBitcast(args, ctx);
    }
    // ─── Branch Prediction ──────────────────────────────────────────────────
    else if (name == "likely" || name == "unlikely") {
        result = emitLikely(name, args, ctx);
    }
    // ─── String Operations ──────────────────────────────────────────────────
    else if (name == "str_len" || name == "str_ptr" || name == "str_from_ptr" ||
             name == "str_concat" || name == "str_slice" || name == "str_eq" ||
             name == "str_byte_at") {
        result = emitStringOp(name, args, ctx);
    }
    // ─── Memory Management ──────────────────────────────────────────────────
    else if (name == "alloc") {
        result = emitAlloc(args, ctx);
    }
    else if (name == "free") {
        result = emitFree(args, ctx);
    }
    else if (name == "arena_create") {
        result = emitArenaCreate(args, ctx);
    }
    else if (name == "arena_alloc") {
        result = emitArenaAlloc(args, ctx);
    }
    else if (name == "arena_reset" || name == "arena_free") {
        result = emitArenaFree(name, args, ctx);
    }
    // ─── Scope Exit ──────────────────────────────────────────────────────────
    else if (name == "scope_exit") {
        result = emitScopeExit(expr, ctx);
    }
    // ─── SIMD ────────────────────────────────────────────────────────────────
    else if (name.find("simd_") == 0) {
        result = emitSIMD(name, args, ctx);
    }
    else {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, expr->loc,
                                "unknown intrinsic '#", name, "'");
        return nullptr;
    }

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Index Expression
// =============================================================================

llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower target ─────────────────────────────────────────────────────
    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    // ─── Lower index ──────────────────────────────────────────────────────
    llvm::Value* index = lowerExpression(expr->index, ctx);
    if (!index) {
        return nullptr;
    }

    // ─── If index is an l-value, load it ────────────────────────────────
    if (expr->index->isLValue) {
        index = loadIfNeeded(index, expr->index->isLValue, ctx);
        if (!index) return nullptr;
    }

    // ─── Get the array type ──────────────────────────────────────────────
    const ArrayTypeAST* arrayType = expr->target->semanticType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    // ─── Get the element type ─────────────────────────────────────────────
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (!elemType) {
        return nullptr;
    }

    // ─── Get the pointer to the array data ────────────────────────────────
    llvm::Value* ptr = target;

    // If target is a pointer to the array, load it
    if (target->getType()->isPointerTy()) {
        // For dynamic arrays, target is already a pointer to the data
        // For fixed arrays, target is a pointer to the array
        // We need to handle both cases
        if (target->getType()->getPointerElementType()->isArrayTy()) {
            // Fixed array: get the first element
            ptr = ctx.builder.CreateConstGEP2_32(
                target->getType()->getPointerElementType(),
                target,
                0, 0
            );
        }
    }

    // ─── Create the GEP ────────────────────────────────────────────────────
    llvm::Value* gep = ctx.builder.CreateGEP(
        elemType,
        ptr,
        index,
        "array_idx"
    );

    // ─── Load the value ────────────────────────────────────────────────────
    // If this is used as an l-value, return the pointer
    if (expr->isLValue) {
        expr->llvmValue = gep;
        return gep;
    }

    // Otherwise, load the value
    llvm::Value* result = ctx.builder.CreateLoad(elemType, gep, "array_load");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Slice Expression
// =============================================================================

llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower target ─────────────────────────────────────────────────────
    llvm::Value* target = lowerExpression(expr->target, ctx);
    if (!target) {
        return nullptr;
    }

    // ─── Lower start and end bounds if present ────────────────────────────
    llvm::Value* start = nullptr;
    llvm::Value* end = nullptr;

    if (expr->start) {
        start = lowerExpression(expr->start, ctx);
        if (start) {
            if (expr->start->isLValue) {
                start = loadIfNeeded(start, expr->start->isLValue, ctx);
            }
        }
    }

    if (expr->end) {
        end = lowerExpression(expr->end, ctx);
        if (end) {
            if (expr->end->isLValue) {
                end = loadIfNeeded(end, expr->end->isLValue, ctx);
            }
        }
    }

    // ─── Get the array type ──────────────────────────────────────────────
    const ArrayTypeAST* arrayType = expr->target->semanticType->as<ArrayTypeAST>();
    if (!arrayType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, expr->target->loc,
                                "target is not an array type");
        return nullptr;
    }

    // ─── Get the element type ─────────────────────────────────────────────
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (!elemType) {
        return nullptr;
    }

    // ─── Get the pointer to the array data ────────────────────────────────
    llvm::Value* ptr = target;
    if (target->getType()->isPointerTy()) {
        if (target->getType()->getPointerElementType()->isArrayTy()) {
            ptr = ctx.builder.CreateConstGEP2_32(
                target->getType()->getPointerElementType(),
                target,
                0, 0
            );
        }
    }

    // ─── Calculate the start offset ──────────────────────────────────────
    if (!start) {
        // Default start = 0
        start = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
    }

    // ─── Calculate the end offset ────────────────────────────────────────
    if (!end) {
        // Default end = array length
        // For fixed arrays, we can get the length at compile time
        if (arrayType->isFixed()) {
            end = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                arrayType->size
            );
        } else {
            // For dynamic arrays, we need to get the length at runtime
            // TODO: Get the length from the dynamic array
            end = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
        }
    }

    // ─── Create the slice ──────────────────────────────────────────────────
    // Slice is { ptr, len, cap }
    llvm::StructType* sliceType = llvm::dyn_cast<llvm::StructType>(
        getType(ctx, expr->semanticType)
    );
    if (!sliceType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->loc,
                                "slice type is not a struct");
        return nullptr;
    }

    // ─── Calculate the slice pointer ──────────────────────────────────────
    llvm::Value* slicePtr = ctx.builder.CreateGEP(
        elemType,
        ptr,
        start,
        "slice_ptr"
    );

    // ─── Calculate the slice length ──────────────────────────────────────
    llvm::Value* sliceLen = ctx.builder.CreateSub(end, start, "slice_len");

    // ─── Create the slice value ──────────────────────────────────────────
    llvm::Value* result = llvm::UndefValue::get(sliceType);
    result = ctx.builder.CreateInsertValue(result, slicePtr, 0);
    result = ctx.builder.CreateInsertValue(result, sliceLen, 1);
    result = ctx.builder.CreateInsertValue(result, sliceLen, 2);

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Field Access Expression
// =============================================================================

llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower object ─────────────────────────────────────────────────────
    llvm::Value* object = lowerExpression(expr->object, ctx);
    if (!object) {
        return nullptr;
    }

    // ─── Get the object type ──────────────────────────────────────────────
    const TypeAST* objectType = expr->object->semanticType;
    if (!objectType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->object->loc,
                                "object has no type");
        return nullptr;
    }

    // ─── If object is a pointer, load it ──────────────────────────────────
    if (object->getType()->isPointerTy()) {
        // For structs, object is a pointer to the struct
        // For references, it's already a pointer
    }

    // ─── Get the struct type ──────────────────────────────────────────────
    llvm::StructType* structType = nullptr;

    if (objectType->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = objectType->as<NamedTypeAST>();
        const TypeDeclAST* decl = ctx.lookupType(named->name);
        if (decl && decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            structType = ctx.lookupStruct(structDecl);
            if (!structType) {
                // Lower the struct if not already lowered
                lowerStructDecl(const_cast<StructDeclAST*>(structDecl), ctx);
                structType = ctx.lookupStruct(structDecl);
            }
        }
    } else if (objectType->isa<PtrTypeAST>()) {
        // If object is a pointer to a struct, get the pointee type
        const PtrTypeAST* ptr = objectType->as<PtrTypeAST>();
        // TODO: Handle pointer to struct
    }

    if (!structType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->object->loc,
                                "object is not a struct");
        return nullptr;
    }

    // ─── Find the field index ──────────────────────────────────────────────
    // We need to get the struct declaration to find the field index
    // Use the semantic type to get the field name and index
    // For now, we'll use a simple approach: look up the field by name
    // and use its fieldIndex
    size_t fieldIndex = 0;
    bool found = false;

    // Get the struct declaration
    const TypeDeclAST* typeDecl = nullptr;
    if (objectType->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = objectType->as<NamedTypeAST>();
        typeDecl = ctx.lookupType(named->name);
    }

    if (typeDecl && typeDecl->isa<StructDeclAST>()) {
        const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
        for (size_t i = 0; i < structDecl->fields.size(); ++i) {
            if (structDecl->fields[i]->name == expr->fieldName) {
                fieldIndex = i;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        ctx.diagnostics.errorAt(DiagCode::Sem_FieldNotFound, expr->loc,
                                "field '", ctx.pool.lookup(expr->fieldName),
                                "' not found in struct");
        return nullptr;
    }

    // ─── Get the field pointer ──────────────────────────────────────────────
    llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
        structType,
        object,
        fieldIndex,
        ctx.pool.lookup(expr->fieldName)
    );

    // ─── If this is used as an l-value, return the pointer ──────────────
    if (expr->isLValue) {
        expr->llvmValue = fieldPtr;
        return fieldPtr;
    }

    // ─── Load the field value ────────────────────────────────────────────
    llvm::Type* fieldType = structType->getElementType(fieldIndex);
    llvm::Value* result = ctx.builder.CreateLoad(fieldType, fieldPtr, "field_load");
    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Module Access Expression
// =============================================================================

llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Look up the member ──────────────────────────────────────────────
    const ValueDeclAST* decl = ctx.lookupValueByAlias(expr->moduleName, expr->memberName);
    if (!decl) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "undefined member '", ctx.pool.lookup(expr->moduleName),
                                ":", ctx.pool.lookup(expr->memberName), "'");
        return nullptr;
    }

    // ─── Get the LLVM value ──────────────────────────────────────────────
    llvm::Value* result = nullptr;

    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        result = ctx.lookupFunction(funcDecl);
        if (!result) {
            // Try to get from llvmFunction
            result = funcDecl->llvmFunction;
        }
    } else if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* varDecl = decl->as<VarDeclAST>();
        if (varDecl->llvmGlobal) {
            result = varDecl->llvmGlobal;
        } else {
            result = ctx.lookupValue(decl);
        }
    } else {
        result = ctx.lookupValue(decl);
    }

    if (!result) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, expr->loc,
                                "no LLVM value for member '",
                                ctx.pool.lookup(expr->moduleName), ":",
                                ctx.pool.lookup(expr->memberName), "'");
        return nullptr;
    }

    // ─── If this is an l-value, return the address ──────────────────────
    if (expr->isLValue) {
        expr->llvmValue = result;
        return result;
    }

    // ─── Otherwise, load the value ──────────────────────────────────────
    if (result->getType()->isPointerTy()) {
        llvm::Type* pointeeType = result->getType()->getPointerElementType();
        if (pointeeType && !pointeeType->isFunctionTy()) {
            result = ctx.builder.CreateLoad(pointeeType, result, "module_load");
        }
    }

    expr->llvmValue = result;
    return result;
}

// =============================================================================
// Null Coalesce Expression
// =============================================================================

llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower LHS ────────────────────────────────────────────────────────
    llvm::Value* lhs = lowerExpression(expr->value, ctx);
    if (!lhs) {
        return nullptr;
    }

    // ─── Lower RHS ────────────────────────────────────────────────────────
    llvm::Value* rhs = lowerExpression(expr->fallback, ctx);
    if (!rhs) {
        return nullptr;
    }

    // ─── Get the LHS type ──────────────────────────────────────────────────
    const TypeAST* lhsType = expr->value->semanticType;
    if (!lhsType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->value->loc,
                                "LHS has no type");
        return nullptr;
    }

    // ─── Check if LHS is nullable or fallible ────────────────────────────
    if (!isNullableType(lhsType) && !isFallibleType(lhsType)) {
        // If LHS is not nullable/fallible, just return the LHS
        expr->llvmValue = lhs;
        return lhs;
    }

    // ─── Create blocks ────────────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* lhsBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "then", func);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "else", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "merge", func);

    // ─── Check the tag ────────────────────────────────────────────────────
    // For nullable/fallible types, the tag is at offset 0
    llvm::Value* tag = ctx.builder.CreateExtractValue(lhs, 0);

    // Tag value 0 = nil/err, Tag value 1 = valid
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
    // If RHS is an l-value, load it
    if (expr->fallback->isLValue) {
        rhsValue = loadIfNeeded(rhsValue, expr->fallback->isLValue, ctx);
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

    // ─── Store the result ─────────────────────────────────────────────────
    expr->llvmValue = phi;
    return phi;
}

// =============================================================================
// Assignment Expression
// =============================================================================

llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower LHS ────────────────────────────────────────────────────────
    llvm::Value* lhs = lowerExpression(expr->lhs, ctx);
    if (!lhs) {
        return nullptr;
    }

    // ─── LHS must be an l-value ──────────────────────────────────────────
    if (!expr->lhs->isLValue) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->lhs->loc,
                                "assignment target is not an l-value");
        return nullptr;
    }

    // ─── Lower RHS ────────────────────────────────────────────────────────
    llvm::Value* rhs = lowerExpression(expr->rhs, ctx);
    if (!rhs) {
        return nullptr;
    }

    // ─── If RHS is an l-value, load it ──────────────────────────────────
    if (expr->rhs->isLValue) {
        rhs = loadIfNeeded(rhs, expr->rhs->isLValue, ctx);
    }

    // ─── Handle compound assignment ──────────────────────────────────────
    if (expr->op != AssignOp::Assign) {
        // Load the current value from LHS
        llvm::Value* lhsValue = loadIfNeeded(lhs, true, ctx);
        if (!lhsValue) {
            return nullptr;
        }

        // Perform the operation
        // For now, only handle basic arithmetic
        switch (expr->op) {
            case AssignOp::AddAssign:
                if (lhsValue->getType()->isIntegerTy()) {
                    rhs = ctx.builder.CreateAdd(lhsValue, rhs, "add");
                } else {
                    rhs = ctx.builder.CreateFAdd(lhsValue, rhs, "fadd");
                }
                break;
            case AssignOp::SubAssign:
                if (lhsValue->getType()->isIntegerTy()) {
                    rhs = ctx.builder.CreateSub(lhsValue, rhs, "sub");
                } else {
                    rhs = ctx.builder.CreateFSub(lhsValue, rhs, "fsub");
                }
                break;
            case AssignOp::MulAssign:
                if (lhsValue->getType()->isIntegerTy()) {
                    rhs = ctx.builder.CreateMul(lhsValue, rhs, "mul");
                } else {
                    rhs = ctx.builder.CreateFMul(lhsValue, rhs, "fmul");
                }
                break;
            case AssignOp::DivAssign:
                if (lhsValue->getType()->isIntegerTy()) {
                    if (lhsValue->getType()->isSigned()) {
                        rhs = ctx.builder.CreateSDiv(lhsValue, rhs, "sdiv");
                    } else {
                        rhs = ctx.builder.CreateUDiv(lhsValue, rhs, "udiv");
                    }
                } else {
                    rhs = ctx.builder.CreateFDiv(lhsValue, rhs, "fdiv");
                }
                break;
            // ... handle other compound operators
            default:
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, expr->loc,
                                        "unsupported compound assignment");
                return nullptr;
        }
    }

    // ─── Store the RHS into the LHS ──────────────────────────────────────
    ctx.builder.CreateStore(rhs, lhs);

    // ─── Return the stored value ─────────────────────────────────────────
    expr->llvmValue = rhs;
    return rhs;
}

// =============================================================================
// Pipeline Expression
// =============================================================================

llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower the seed ──────────────────────────────────────────────────
    llvm::Value* currentValue = lowerExpression(expr->seed, ctx);
    if (!currentValue) {
        return nullptr;
    }

    // ─── If seed is an l-value, load it ──────────────────────────────────
    if (expr->seed->isLValue) {
        currentValue = loadIfNeeded(currentValue, expr->seed->isLValue, ctx);
    }

    // ─── Apply each step ──────────────────────────────────────────────────
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

    // ─── Lower the callable ──────────────────────────────────────────────
    llvm::Value* callable = lowerExpression(step->callable, ctx);
    if (!callable) {
        return nullptr;
    }

    // ─── If callable is an l-value, load it ──────────────────────────────
    if (step->callable->isLValue) {
        callable = loadIfNeeded(callable, step->callable->isLValue, ctx);
    }

    // ─── Build arguments ──────────────────────────────────────────────────
    std::vector<llvm::Value*> args;

    // Add the current value as the first argument
    args.push_back(callable);

    // Add pack arguments if present
    for (ExprAST* arg : step->packArgs) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            argVal = loadIfNeeded(argVal, arg->isLValue, ctx);
        }
        args.push_back(argVal);
    }

    // ─── Create the call ──────────────────────────────────────────────────
    // The callable should be a function pointer
    // For now, assume it's a function pointer
    llvm::Value* result = ctx.builder.CreateCall(
        llvm::dyn_cast<llvm::Function>(callable),
        args,
        "pipeline_call"
    );

    return result;
}

// =============================================================================
// Composition Expression
// =============================================================================

llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Composition is compile-time, not runtime ──────────────────────
    // The result of composition is a new function that is created at
    // compile time. This function is a combination of the operands.
    // For now, we'll just return the result of the last operand
    // TODO: Implement proper composition lowering

    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "composition is not fully implemented yet");
    return nullptr;
}

llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx) {
    if (!operand) return nullptr;

    // ─── Lower the callable ──────────────────────────────────────────────
    llvm::Value* callable = lowerExpression(operand->callable, ctx);
    if (!callable) {
        return nullptr;
    }

    return callable;
}

// =============================================================================
// Anonymous Function Expression
// =============================================================================

llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Check if this is a closure ──────────────────────────────────────
    if (expr->hasClosure) {
        // Lower as a closure
        return lowerClosure(expr, ctx);
    }

    // ─── Not a closure - just return the function pointer ────────────────
    // Create the function for the anonymous expression
    // This is a named function that is created at compile time
    // For now, we'll just return a placeholder
    // TODO: Implement non-closure anonymous functions

    return llvm::Constant::getNullValue(
        llvm::PointerType::get(ctx.llvmCtx, 0)
    );
}

// =============================================================================
// If Expression
// =============================================================================

llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Lower condition ──────────────────────────────────────────────────
    llvm::Value* cond = lowerExpression(expr->condition, ctx);
    if (!cond) {
        return nullptr;
    }

    // ─── Get the condition as a bool ──────────────────────────────────────
    if (!cond->getType()->isIntegerTy(1)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    // ─── Create blocks ────────────────────────────────────────────────────
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
    // If then is an l-value, load it
    if (expr->thenBranch->isLValue) {
        thenVal = loadIfNeeded(thenVal, expr->thenBranch->isLValue, ctx);
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Else branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(elseBlock);
    llvm::Value* elseVal = lowerExpression(expr->elseBranch, ctx);
    if (!elseVal) {
        return nullptr;
    }
    // If else is an l-value, load it
    if (expr->elseBranch->isLValue) {
        elseVal = loadIfNeeded(elseVal, expr->elseBranch->isLValue, ctx);
    }
    ctx.builder.CreateBr(mergeBlock);

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(
        thenVal->getType(),
        2,
        "if"
    );
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

    // ─── Range is not a runtime value ──────────────────────────────────
    // Ranges are only used in for loops and slices
    // For now, we'll just return null
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, expr->loc,
                              "range expression should not be used as a value");
    return nullptr;
}

// =============================================================================
// Intrinsic Helpers
// =============================================================================

llvm::Value* emitLLVMIntrinsic(const std::string& name,
                               const std::vector<llvm::Value*>& args,
                               CodeGenContext& ctx) {
    // ─── Get the LLVM intrinsic ID ──────────────────────────────────────
    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;

    if (name == "sqrt") id = llvm::Intrinsic::sqrt;
    else if (name == "abs") id = llvm::Intrinsic::fabs;
    else if (name == "fma") id = llvm::Intrinsic::fma;
    else if (name == "ceil") id = llvm::Intrinsic::ceil;
    else if (name == "floor") id = llvm::Intrinsic::floor;
    else if (name == "round") id = llvm::Intrinsic::round;
    else if (name == "memcpy") id = llvm::Intrinsic::memcpy;
    else if (name == "memmove") id = llvm::Intrinsic::memmove;
    else if (name == "memset") id = llvm::Intrinsic::memset;
    else if (name == "clz") id = llvm::Intrinsic::ctlz;
    else if (name == "ctz") id = llvm::Intrinsic::cttz;
    else if (name == "popcount") id = llvm::Intrinsic::ctpop;
    else if (name == "bswap") id = llvm::Intrinsic::bswap;

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, SourceLocation(),
                                "unknown LLVM intrinsic: ", name);
        return nullptr;
    }

    // ─── Create the intrinsic function ──────────────────────────────────
    llvm::Function* intrinsic = llvm::Intrinsic::getDeclaration(
        ctx.module,
        id,
        {args.empty() ? nullptr : args[0]->getType()}
    );

    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, SourceLocation(),
                                "failed to get LLVM intrinsic: ", name);
        return nullptr;
    }

    // ─── Call the intrinsic ──────────────────────────────────────────────
    return ctx.builder.CreateCall(intrinsic, args);
}

llvm::Value* emitPrefetch(const std::string& name,
                          const std::vector<llvm::Value*>& args,
                          CodeGenContext& ctx) {
    // prefetch(ptr) -> LLVM prefetch intrinsic
    // The LLVM prefetch intrinsic has 4 arguments: ptr, rw, locality, cache_type
    // rw: 0 = read, 1 = write
    // locality: 0-3 (0 = no locality, 3 = high locality)
    // cache_type: 0 = data, 1 = instruction

    if (args.empty()) {
        return nullptr;
    }

    llvm::Value* ptr = args[0];

    // Determine read/write mode
    int rw = 0;
    if (name == "prefetch_w") {
        rw = 1;
    }

    // Use default locality
    int locality = 3;
    int cacheType = 0;

    // Get the prefetch intrinsic
    llvm::Function* prefetch = llvm::Intrinsic::getDeclaration(
        ctx.module,
        llvm::Intrinsic::prefetch,
        {ptr->getType()}
    );

    std::vector<llvm::Value*> prefetchArgs = {
        ptr,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), rw),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), locality),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), cacheType)
    };

    return ctx.builder.CreateCall(prefetch, prefetchArgs);
}

llvm::Value* emitFence(const std::vector<llvm::Value*>& args,
                       CodeGenContext& ctx) {
    // fence(ordering) -> LLVM fence instruction
    // ordering: relaxed, acquire, release, acq_rel, seq_cst

    if (args.empty()) {
        return nullptr;
    }

    // The ordering should be a string literal
    // For now, use seq_cst as default
    llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;

    ctx.builder.CreateFence(ordering);
    return nullptr;
}

llvm::Value* emitPause(CodeGenContext& ctx) {
    // pause() -> LLVM x86 pause instruction or equivalent
    // For now, just return null
    return nullptr;
}

llvm::Value* emitAtomic(const std::string& name,
                        const std::vector<llvm::Value*>& args,
                        CodeGenContext& ctx) {
    // Atomic operations
    // TODO: Implement atomic operations
    return nullptr;
}

llvm::Value* emitSizeof(const std::vector<llvm::Value*>& args,
                        CodeGenContext& ctx) {
    // sizeof(T) -> size of the type in bytes
    // This is a compile-time constant
    // For now, just return 0
    // TODO: Implement sizeof
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
}

llvm::Value* emitAlignof(const std::vector<llvm::Value*>& args,
                         CodeGenContext& ctx) {
    // alignof(T) -> alignment of the type in bytes
    // This is a compile-time constant
    // For now, just return 0
    // TODO: Implement alignof
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
}

llvm::Value* emitTypeString(const std::string& name,
                            const std::vector<llvm::Value*>& args,
                            CodeGenContext& ctx) {
    // typeof, nameof, tostr, ptrstr -> string representation
    // For now, just return an empty string
    // TODO: Implement type inspection intrinsics
    return nullptr;
}

llvm::Value* emitAddrOf(const std::vector<llvm::Value*>& args,
                        CodeGenContext& ctx) {
    // addrof(x) -> pointer to x
    // x must be an l-value
    if (args.empty()) {
        return nullptr;
    }

    // args[0] is the address of the l-value
    return args[0];
}

llvm::Value* emitToRef(const std::vector<llvm::Value*>& args,
                       CodeGenContext& ctx) {
    // toRef(ptr) -> assert non-null and convert to reference
    if (args.empty()) {
        return nullptr;
    }

    // Just cast the pointer to a reference
    // TODO: Add null check assertion
    return args[0];
}

llvm::Value* emitToPtr(const std::vector<llvm::Value*>& args,
                       CodeGenContext& ctx) {
    // toPtr(ref) -> convert reference to pointer
    if (args.empty()) {
        return nullptr;
    }

    // Just cast the reference to a pointer
    return args[0];
}

llvm::Value* emitPtrOffset(const std::vector<llvm::Value*>& args,
                           CodeGenContext& ctx) {
    // ptrOffset(ptr, n) -> pointer arithmetic
    if (args.size() < 2) {
        return nullptr;
    }

    llvm::Value* ptr = args[0];
    llvm::Value* offset = args[1];

    // Create GEP
    return ctx.builder.CreateGEP(
        ptr->getType()->getPointerElementType(),
        ptr,
        offset,
        "ptr_offset"
    );
}

llvm::Value* emitPtrDiff(const std::vector<llvm::Value*>& args,
                         CodeGenContext& ctx) {
    // ptrDiff(p1, p2) -> distance between pointers
    if (args.size() < 2) {
        return nullptr;
    }

    // Convert pointers to integers and subtract
    llvm::Value* p1 = ctx.builder.CreatePtrToInt(
        args[0],
        llvm::Type::getInt64Ty(ctx.llvmCtx)
    );
    llvm::Value* p2 = ctx.builder.CreatePtrToInt(
        args[1],
        llvm::Type::getInt64Ty(ctx.llvmCtx)
    );

    return ctx.builder.CreateSub(p1, p2, "ptr_diff");
}

llvm::Value* emitBitcast(const std::vector<llvm::Value*>& args,
                         CodeGenContext& ctx) {
    // bitcast(T, x) -> reinterpret bits of x as type T
    // T is the type, x is the value
    // For now, just return the value
    // TODO: Implement bitcast
    return args.empty() ? nullptr : args[0];
}

llvm::Value* emitLikely(const std::string& name,
                        const std::vector<llvm::Value*>& args,
                        CodeGenContext& ctx) {
    // likely(expr) / unlikely(expr) -> branch prediction hint
    // For now, just return the expression value
    return args.empty() ? nullptr : args[0];
}

llvm::Value* emitStringOp(const std::string& name,
                          const std::vector<llvm::Value*>& args,
                          CodeGenContext& ctx) {
    // String operations
    // TODO: Implement string operations
    return nullptr;
}

llvm::Value* emitAlloc(const std::vector<llvm::Value*>& args,
                       CodeGenContext& ctx) {
    // alloc(T, count) -> allocate heap memory
    // For now, just return null
    // TODO: Implement heap allocation
    return nullptr;
}

llvm::Value* emitFree(const std::vector<llvm::Value*>& args,
                      CodeGenContext& ctx) {
    // free(ptr) -> free heap memory
    // For now, just return null
    // TODO: Implement heap free
    return nullptr;
}

llvm::Value* emitArenaCreate(const std::vector<llvm::Value*>& args,
                             CodeGenContext& ctx) {
    // arena_create(size) -> create arena descriptor
    // For now, just return null
    // TODO: Implement arena creation
    return nullptr;
}

llvm::Value* emitArenaAlloc(const std::vector<llvm::Value*>& args,
                            CodeGenContext& ctx) {
    // arena_alloc(arena, T, count) -> allocate from arena
    // For now, just return null
    // TODO: Implement arena allocation
    return nullptr;
}

llvm::Value* emitArenaFree(const std::string& name,
                           const std::vector<llvm::Value*>& args,
                           CodeGenContext& ctx) {
    // arena_reset(arena) / arena_free(arena) -> free arena
    // For now, just return null
    // TODO: Implement arena free
    return nullptr;
}

llvm::Value* emitScopeExit(IntrinsicCallExprAST* expr,
                           CodeGenContext& ctx) {
    // scope_exit(fn, args...) -> register scope exit callback
    // This is handled in Sema and stored on the BlockStmtAST
    // For now, just return null
    return nullptr;
}

llvm::Value* emitSIMD(const std::string& name,
                      const std::vector<llvm::Value*>& args,
                      CodeGenContext& ctx) {
    // SIMD operations
    // TODO: Implement SIMD operations
    return nullptr;
}

} // namespace codegen