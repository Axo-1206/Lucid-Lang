/**
 * @file BinaryChecker.cpp
 * @brief Semantic checking for binary and unary operators.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

static bool bothNumeric(TypeAST* left, TypeAST* right) {
    if (!left->isa<PrimitiveTypeAST>() || !right->isa<PrimitiveTypeAST>())
        return false;
    auto lp = left->as<PrimitiveTypeAST>()->primitiveKind;
    auto rp = right->as<PrimitiveTypeAST>()->primitiveKind;
    return (lp == PrimitiveKind::Int || lp == PrimitiveKind::Float ||
            lp == PrimitiveKind::Double || lp == PrimitiveKind::Decimal) &&
           (rp == PrimitiveKind::Int || rp == PrimitiveKind::Float ||
            rp == PrimitiveKind::Double || rp == PrimitiveKind::Decimal);
}

static bool isIntegerOrFloat(TypeAST* type) {
    if (!type->isa<PrimitiveTypeAST>()) return false;
    auto pk = type->as<PrimitiveTypeAST>()->primitiveKind;
    return pk == PrimitiveKind::Int || pk == PrimitiveKind::Float ||
           pk == PrimitiveKind::Double || pk == PrimitiveKind::Decimal;
}

TypeAST* checkBinaryExpr(BinaryExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.left.get(), ctx);
    TypeAST* rightType = checkExpr(node.right.get(), ctx);
    if (!leftType || !rightType) return nullptr;

    // Arithmetic operators (+, -, *, /, ^, %)
    if (node.op == BinaryOp::Add || node.op == BinaryOp::Sub ||
        node.op == BinaryOp::Mul || node.op == BinaryOp::Div ||
        node.op == BinaryOp::Pow || node.op == BinaryOp::Mod) {
        
        if (!bothNumeric(leftType, rightType)) {
            ctx.error(node.loc, DiagCode::E2002, "arithmetic operator requires numeric operands");
            return nullptr;
        }
        
        // Check for division by zero in constant expressions
        if (node.op == BinaryOp::Div || node.op == BinaryOp::Mod) {
            int64_t rightVal;
            if (TypeChecker::getConstantIntValue(node.right.get(), rightVal, ctx) && rightVal == 0) {
                ctx.error(node.right->loc, DiagCode::E2002, "division by zero");
                return nullptr;
            }
        }
        
        TypeAST* unified = TypeChecker::unify(leftType, rightType, ctx);
        if (!unified) {
            ctx.error(node.loc, DiagCode::E2002, "incompatible numeric types for arithmetic operation");
            return nullptr;
        }
        
        node.isConst = node.left->isConst && node.right->isConst;
        return unified;
    }

    // Comparison operators (==, !=, <, >, <=, >=)
    if (node.op == BinaryOp::Eq || node.op == BinaryOp::Ne ||
        node.op == BinaryOp::Lt || node.op == BinaryOp::Gt ||
        node.op == BinaryOp::Le || node.op == BinaryOp::Ge) {
        
        if (!TypeChecker::isValueComparable(leftType, ctx) ||
            !TypeChecker::isValueComparable(rightType, ctx)) {
            ctx.error(node.loc, DiagCode::E2009, "value comparison not allowed on this type");
            return nullptr;
        }
        
        if (!TypeChecker::isAssignable(leftType, rightType, ctx) &&
            !TypeChecker::isAssignable(rightType, leftType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002, "operands of comparison must be compatible types");
            return nullptr;
        }
        
        node.isConst = node.left->isConst && node.right->isConst;
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();
    }

    // Reference equality (===, !==)
    if (node.op == BinaryOp::RefEq) {
        if (!TypeChecker::isReferenceComparable(leftType, ctx) ||
            !TypeChecker::isReferenceComparable(rightType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002, "reference equality (===) only allowed on structs and references");
            return nullptr;
        }
        
        node.isConst = node.left->isConst && node.right->isConst;
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();
    }

    // Logical operators (and, or)
    if (node.op == BinaryOp::And || node.op == BinaryOp::Or) {
        if (!TypeChecker::isBoolOrNullable(leftType, ctx) ||
            !TypeChecker::isBoolOrNullable(rightType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002, "logical operators require bool or nullable operands");
            return nullptr;
        }
        
        // Logical operators short-circuit, so const only if both are const
        node.isConst = node.left->isConst && node.right->isConst;
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();
    }

    // Bitwise operators (&&, ||, ~^, <<, >>)
    if (node.op == BinaryOp::BitAnd || node.op == BinaryOp::BitOr ||
        node.op == BinaryOp::BitXor || node.op == BinaryOp::Shl ||
        node.op == BinaryOp::Shr) {
        
        if (!TypeChecker::isIntegerType(leftType, ctx) || !TypeChecker::isIntegerType(rightType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002, "bitwise operators require integer operands");
            return nullptr;
        }
        
        node.isConst = node.left->isConst && node.right->isConst;
        return leftType;
    }

    ctx.error(node.loc, DiagCode::E2002, "unsupported binary operator");
    return nullptr;
}

TypeAST* checkUnaryExpr(UnaryExprAST& node, SemanticContext& ctx) {
    TypeAST* operandType = checkExpr(node.operand.get(), ctx);
    if (!operandType) return nullptr;

    switch (node.op) {
        case UnaryOp::Neg:
            if (!isIntegerOrFloat(operandType)) {
                ctx.error(node.loc, DiagCode::E2002, "negation (-) requires numeric operand");
                return nullptr;
            }
            node.isConst = node.operand->isConst;
            return operandType;

        case UnaryOp::Not:
            if (!TypeChecker::isBoolOrNullable(operandType, ctx)) {
                ctx.error(node.loc, DiagCode::E2002, "logical not requires bool or nullable operand");
                return nullptr;
            }
            node.isConst = node.operand->isConst;
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();

        case UnaryOp::BitNot:
            if (!TypeChecker::isIntegerType(operandType, ctx)) {
                ctx.error(node.loc, DiagCode::E2002, "bitwise not (~) requires integer operand");
                return nullptr;
            }
            node.isConst = node.operand->isConst;
            return operandType;

        case UnaryOp::Ref:
            // Reference operator is never const (takes address of runtime value)
            node.isConst = false;
            return ctx.arena.make<RefTypeAST>(TypePtr(operandType)).release();

        default:
            ctx.error(node.loc, DiagCode::E2002, "unsupported unary operator");
            return nullptr;
    }
}