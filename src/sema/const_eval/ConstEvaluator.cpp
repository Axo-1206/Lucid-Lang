/// @file const_eval/ConstEvaluator.cpp
/// @brief Implementation of ConstEvaluator.

#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"
#include "sema/types/SemaResolve.hpp"
#include "sema/Sema.hpp"

#include <cmath>
#include <queue>
#include <algorithm>

namespace sema {

// ─── Main Entry Points ───────────────────────────────────────────────────

ConstantValue ConstEvaluator::evaluateDecl(SemaContext& ctx, const VarDeclAST* decl) {
    if (!decl || !decl->init) {
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "const variable '", ctx.pool.lookup(decl->name),
                              "' has no initializer");
        return ConstantValue::error();
    }

    if (m_recursionDepth > MAX_RECURSION) {
        ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, decl,
                              "const evaluation recursion limit exceeded (",
                              MAX_RECURSION, ")");
        return ConstantValue::error();
    }

    if (m_evaluating.find(decl) != m_evaluating.end()) {
        ctx.diagnostics.error(DiagCode::Sem_CircularDependency, decl,
                              "circular dependency detected in const declaration '",
                              ctx.pool.lookup(decl->name), "'");
        return ConstantValue::error();
    }

    EvaluationGuard guard(m_evaluating, decl);
    m_recursionDepth++;

    // ─── Push a scope for the declaration ──────────────────────────────
    // This allows the initializer to look up the variable if needed
    // (though self-reference should be caught separately)
    ctx.pushScope();
    ctx.insertValue(decl);
    
    ConstantValue result = evaluate(ctx, decl->init, decl->type);
    
    ctx.popScope();
    m_recursionDepth--;

    return result;
}

ConstantValue ConstEvaluator::evaluate(SemaContext& ctx, const ExprAST* expr,
                                        const TypeAST* targetType) {
    if (!expr) return ConstantValue::error();

    if (m_evaluatedExprs.find(expr) != m_evaluatedExprs.end()) {
        return expr->constValue;
    }

    ConstantValue result;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = evalLiteral(ctx, expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr:
            result = evalIdentifier(ctx, expr->as<IdentifierExprAST>());
            break;
        case ASTKind::BinaryExpr:
            result = evalBinary(ctx, expr->as<BinaryExprAST>(), targetType);
            break;
        case ASTKind::UnaryExpr:
            result = evalUnary(ctx, expr->as<UnaryExprAST>(), targetType);
            break;
        case ASTKind::CallExpr:
            result = evalCall(ctx, expr->as<CallExprAST>());
            break;
        case ASTKind::StructLiteralExpr:
            result = evalStructLiteral(ctx, expr->as<StructLiteralExprAST>());
            break;
        case ASTKind::ArrayLiteralExpr:
            result = evalArrayLiteral(ctx, expr->as<ArrayLiteralExprAST>());
            break;
        case ASTKind::FieldAccessExpr:
            result = evalFieldAccess(ctx, expr->as<FieldAccessExprAST>());
            break;
        case ASTKind::NullCoalesceExpr:
            result = evalNullCoalesce(ctx, expr->as<NullCoalesceExprAST>());
            break;
        case ASTKind::IfExpr:
            result = evalIfExpr(ctx, expr->as<IfExprAST>());
            break;
        default:
            return ConstantValue::unknown();
    }

    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(ctx, result);
        const_cast<ExprAST*>(expr)->valueState = result.isErr() ? ValueState::Err : ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

void ConstEvaluator::reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle) {
    if (cycle.empty()) return;
    
    std::string msg = "circular dependency in const declarations: ";
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) msg += " → ";
        msg += ctx.pool.lookup(cycle[i]->name);
    }
    ctx.diagnostics.error(DiagCode::Sem_CircularDependency, cycle[0], msg);
}

ConstantValue ConstEvaluator::getConstValue(const VarDeclAST* decl) {
    if (!decl || decl->keyword != DeclKeyword::Const || !decl->init) {
        return ConstantValue::unknown();
    }
    if (decl->init->isConst) {
        return decl->init->constValue;
    }
    return ConstantValue::unknown();
}

// ─── Literal Evaluation ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalLiteral(SemaContext& ctx, const LiteralExprAST* expr) {
    switch (expr->kind) {
        case LiteralKind::True:   return ConstantValue(true);
        case LiteralKind::False:  return ConstantValue(false);
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                return ConstantValue(std::stoll(str, nullptr, 0));
            } catch (const std::exception&) {
                ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                      "invalid integer literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::Float: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                return ConstantValue(std::stod(str));
            } catch (const std::exception&) {
                ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                      "invalid float literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::String:
        case LiteralKind::RawString:
            return ConstantValue(expr->value);
        case LiteralKind::Char:
            return ConstantValue(expr->value);
        case LiteralKind::Nil:   return ConstantValue::nil();
        case LiteralKind::Err:   return ConstantValue::err();
        default:                 return ConstantValue::unknown();
    }
}

// ─── Identifier Evaluation ──────────────────────────────────────────────

ConstantValue ConstEvaluator::evalIdentifier(SemaContext& ctx, const IdentifierExprAST* expr) {
    // Use existing lookup infrastructure
    const ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) return ConstantValue::error();

    // ─── Variable ──────────────────────────────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        
        // Check if this variable has a const value already computed
        if (var->init && var->init->isConst) {
            return var->init->constValue;
        }

        // If it's a const variable, evaluate it now
        if (var->keyword == DeclKeyword::Const && var->init) {
            if (m_evaluating.find(var) != m_evaluating.end()) {
                ctx.diagnostics.error(DiagCode::Sem_CircularDependency, expr,
                                      "cycle detected in const declaration '",
                                      ctx.pool.lookup(expr->name), "'");
                return ConstantValue::error();
            }
            return evaluate(ctx, var->init, var->type);
        }

        // Non-const variable cannot be evaluated
        return ConstantValue::unknown();
    }

    // ─── Function ──────────────────────────────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            return ConstantValue::unknown();
        }
        return ConstantValue(func);
    }

    // ─── Enum Variant ──────────────────────────────────────────────────────
    if (decl->isa<EnumVariantAST>()) {
        const EnumVariantAST* variant = decl->as<EnumVariantAST>();
        return ConstantValue(variant->value);
    }

    return ConstantValue::unknown();
}

// ─── Binary Expression Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalBinary(SemaContext& ctx, const BinaryExprAST* expr,
                                          const TypeAST* targetType) {
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
        }
    }

    ConstantValue left = evaluate(ctx, expr->left, targetType);
    if (left.isError()) return left;
    if (left.isUnknown()) return ConstantValue::unknown();

    // Short-circuit for logical operators
    if (expr->op == BinaryOp::And) {
        if (left.isBool() && !left.asBool()) {
            return ConstantValue(false);
        }
        if (left.isUnknown()) return ConstantValue::unknown();
    }
    if (expr->op == BinaryOp::Or) {
        if (left.isBool() && left.asBool()) {
            return ConstantValue(true);
        }
        if (left.isUnknown()) return ConstantValue::unknown();
    }

    ConstantValue right = evaluate(ctx, expr->right, targetType);
    if (right.isError()) return right;
    if (right.isUnknown()) return ConstantValue::unknown();

    return evalBinaryOp(ctx, expr->op, left, right, expr, targetType);
}

// ─── Unary Expression Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalUnary(SemaContext& ctx, const UnaryExprAST* expr,
                                         const TypeAST* targetType) {
    ConstantValue operand = evaluate(ctx, expr->operand, targetType);
    if (operand.isError()) return operand;
    if (operand.isUnknown()) return ConstantValue::unknown();

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand.isInt()) {
                // Check for overflow: INT64_MIN cannot be negated
                if (operand.asInt() == INT64_MIN) {
                    ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, expr,
                                          "integer overflow in const negation (INT64_MIN)");
                    return ConstantValue::error();
                }
                return ConstantValue(-operand.asInt());
            }
            if (operand.isFloat()) {
                return ConstantValue(-operand.asFloat());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "negation requires numeric operand");
            return ConstantValue::error();

        case UnaryOp::Not:
            if (operand.isBool()) {
                return ConstantValue(!operand.asBool());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "logical not requires bool operand");
            return ConstantValue::error();

        case UnaryOp::BitNot:
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "bitwise not requires integer operand");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Binary Operation Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                            const ConstantValue& left,
                                            const ConstantValue& right,
                                            const BaseAST* node,
                                            const TypeAST* targetType) {
    auto bothNumeric = [](const ConstantValue& a, const ConstantValue& b) {
        return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
    };

    auto toDouble = [](const ConstantValue& v) -> double {
        if (v.isInt()) return static_cast<double>(v.asInt());
        if (v.isFloat()) return v.asFloat();
        return 0.0;
    };

    auto handleArithmeticError = [&](const char* op, const std::string& reason) -> ConstantValue {
        if (targetType && isFallibleType(targetType)) {
            return ConstantValue::err();
        }
        ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                              reason, " in const expression");
        return ConstantValue::error();
    };

    switch (op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
        case BinaryOp::Add:
            // ─── Integer + Integer ──────────────────────────────────────
            if (left.isInt() && right.isInt()) {
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r)) {
                    ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                          "integer overflow in const addition");
                    return ConstantValue::error();
                }
                return ConstantValue(l + r);
            }
            
            // ─── Float + Float ──────────────────────────────────────────
            if (left.isFloat() && right.isFloat()) {
                return ConstantValue(left.asFloat() + right.asFloat());
            }
            
            // ─── Integer + Float (promote int to float) ────────────────
            if (left.isInt() && right.isFloat()) {
                double l = static_cast<double>(left.asInt());
                return ConstantValue(l + right.asFloat());
            }
            if (left.isFloat() && right.isInt()) {
                double r = static_cast<double>(right.asInt());
                return ConstantValue(left.asFloat() + r);
            }
            
            // ─── String concatenation ──────────────────────────────────
            if (left.isString() && right.isString()) {
                std::string result = ctx.pool.lookup(left.asString());
                result += ctx.pool.lookup(right.asString());
                return ConstantValue(ctx.pool.intern(result));
            }
            
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "invalid operands for '+'");
            return ConstantValue::error();

        case BinaryOp::Sub:
            // ─── Integer - Integer ──────────────────────────────────────
            if (left.isInt() && right.isInt()) {
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if ((r > 0 && l < INT64_MIN + r) || (r < 0 && l > INT64_MAX + r)) {
                    ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                          "integer overflow in const subtraction");
                    return ConstantValue::error();
                }
                return ConstantValue(l - r);
            }
            
            // ─── Float - Float ──────────────────────────────────────────
            if (left.isFloat() && right.isFloat()) {
                return ConstantValue(left.asFloat() - right.asFloat());
            }
            
            // ─── Integer - Float (promote int to float) ────────────────
            if (left.isInt() && right.isFloat()) {
                double l = static_cast<double>(left.asInt());
                return ConstantValue(l - right.asFloat());
            }
            if (left.isFloat() && right.isInt()) {
                double r = static_cast<double>(right.asInt());
                return ConstantValue(left.asFloat() - r);
            }
            
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "invalid operands for '-'");
            return ConstantValue::error();

        case BinaryOp::Mul:
            // ─── Integer * Integer ──────────────────────────────────────
            if (left.isInt() && right.isInt()) {
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if (l != 0 && r != 0) {
                    if ((r > 0 && (l > INT64_MAX / r || l < INT64_MIN / r)) ||
                        (r < 0 && (l > INT64_MIN / -r || l < INT64_MAX / -r))) {
                        ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                              "integer overflow in const multiplication");
                        return ConstantValue::error();
                    }
                }
                return ConstantValue(l * r);
            }
            
            // ─── Float * Float ──────────────────────────────────────────
            if (left.isFloat() && right.isFloat()) {
                return ConstantValue(left.asFloat() * right.asFloat());
            }
            
            // ─── Integer * Float (promote int to float) ────────────────
            if (left.isInt() && right.isFloat()) {
                double l = static_cast<double>(left.asInt());
                return ConstantValue(l * right.asFloat());
            }
            if (left.isFloat() && right.isInt()) {
                double r = static_cast<double>(right.asInt());
                return ConstantValue(left.asFloat() * r);
            }
            
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "invalid operands for '*'");
            return ConstantValue::error();

        case BinaryOp::Div:
            // ─── Integer / Integer ──────────────────────────────────────
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return handleArithmeticError("/", "division by zero");
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            
            // ─── Float / Float ──────────────────────────────────────────
            if (left.isFloat() && right.isFloat()) {
                if (right.asFloat() == 0.0) {
                    return handleArithmeticError("/", "division by zero");
                }
                return ConstantValue(left.asFloat() / right.asFloat());
            }
            
            // ─── Integer / Float (promote int to float) ────────────────
            if (left.isInt() && right.isFloat()) {
                if (right.asFloat() == 0.0) {
                    return handleArithmeticError("/", "division by zero");
                }
                return ConstantValue(static_cast<double>(left.asInt()) / right.asFloat());
            }
            if (left.isFloat() && right.isInt()) {
                if (right.asInt() == 0) {
                    return handleArithmeticError("/", "division by zero");
                }
                return ConstantValue(left.asFloat() / static_cast<double>(right.asInt()));
            }
            
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "invalid operands for '/'");
            return ConstantValue::error();

        case BinaryOp::Mod:
            // ─── Integer % Integer ──────────────────────────────────────
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return handleArithmeticError("%", "modulo by zero");
                }
                return ConstantValue(left.asInt() % right.asInt());
            }
            
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "modulo requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Pow:
            // ─── Numeric power ──────────────────────────────────────────
            if (bothNumeric(left, right)) {
                double base = toDouble(left);
                double exp = toDouble(right);
                if (base == 0.0 && exp < 0) {
                    return handleArithmeticError("**", "0 raised to negative power");
                }
                return ConstantValue(std::pow(base, exp));
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "pow requires numeric operands");
            return ConstantValue::error();

        // ─── Comparison ──────────────────────────────────────────────────
        case BinaryOp::Eq:
            return ConstantValue(compareEqual(ctx, left, right));
        case BinaryOp::Ne:
            return ConstantValue(!compareEqual(ctx, left, right));
        case BinaryOp::Lt:
            return ConstantValue(compareOrder(ctx, left, right) < 0);
        case BinaryOp::Gt:
            return ConstantValue(compareOrder(ctx, left, right) > 0);
        case BinaryOp::Le:
            return ConstantValue(compareOrder(ctx, left, right) <= 0);
        case BinaryOp::Ge:
            return ConstantValue(compareOrder(ctx, left, right) >= 0);

        // ─── Logical ──────────────────────────────────────────────────────
        case BinaryOp::And:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() && right.asBool());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                  "'and' requires bool operands");
            return ConstantValue::error();

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                  "'or' requires bool operands");
            return ConstantValue::error();

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(
                    op == BinaryOp::BitAnd ? left.asInt() & right.asInt() :
                    op == BinaryOp::BitOr  ? left.asInt() | right.asInt() :
                                              left.asInt() ^ right.asInt()
                );
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                  "bitwise operator requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shl:
        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    ctx.diagnostics.error(DiagCode::Sem_NegativeShift, node,
                                          "negative shift amount in const expression");
                    return ConstantValue::error();
                }
                if (right.asInt() >= 64) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                          "shift amount exceeds bit width");
                    return ConstantValue::error();
                }
                return ConstantValue(
                    op == BinaryOp::Shl ? left.asInt() << right.asInt() :
                                          left.asInt() >> right.asInt()
                );
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                  "shift requires integer operands");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Comparison Helpers ─────────────────────────────────────────────────

bool ConstEvaluator::compareEqual(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b) {
    if (a.kind != b.kind) return false;

    switch (a.kind) {
        case ConstantValue::Kind::Bool:
            return a.asBool() == b.asBool();
        case ConstantValue::Kind::Int:
            return a.asInt() == b.asInt();
        case ConstantValue::Kind::Float:
            return a.asFloat() == b.asFloat();
        case ConstantValue::Kind::String:
        case ConstantValue::Kind::Char:
            return ctx.pool.lookup(a.asString()) == ctx.pool.lookup(b.asString());
        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err:
            return true;
        default:
            return false;
    }
}

int ConstEvaluator::compareOrder(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b) {
    if (a.kind != b.kind) return 0;

    switch (a.kind) {
        case ConstantValue::Kind::Int: {
            int64_t diff = a.asInt() - b.asInt();
            return (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
        }
        case ConstantValue::Kind::Float: {
            double diff = a.asFloat() - b.asFloat();
            return (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
        }
        case ConstantValue::Kind::String:
        case ConstantValue::Kind::Char:
            return ctx.pool.lookup(a.asString()).compare(ctx.pool.lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Type Helpers ──────────────────────────────────────────────────────

TypeAST* ConstEvaluator::getConstantType(SemaContext& ctx, const ConstantValue& val) {
    if (val.type) return val.type;

    switch (val.kind) {
        case ConstantValue::Kind::Bool:   return ctx.getBoolType();
        case ConstantValue::Kind::Int:    return ctx.getIntType();
        case ConstantValue::Kind::Float:  return ctx.getFloatType();
        case ConstantValue::Kind::String: return ctx.getStringType();
        case ConstantValue::Kind::Char:   return ctx.getCharType();
        default:                          return ctx.getUnknownType();
    }
}

// ─── Call Evaluation ────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalCall(SemaContext& ctx, const CallExprAST* expr) {
    // ─── 1. Resolve the callee ──────────────────────────────────────────
    const FuncDeclAST* func = resolveCalleeOrError(expr->callee, ctx);
    if (!func) {
        // Error already reported by resolveCalleeOrError
        return ConstantValue::error();
    }

    // ─── 2. Check if it's const ──────────────────────────────────────────
    if (func->keyword != DeclKeyword::Const) {
        return ConstantValue::unknown();
    }

    // ─── 3. Collect parameter count ──────────────────────────────────────
    size_t paramCount = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        paramCount += group->params.size();
    }

    if (expr->args.size() != paramCount) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "argument count mismatch: expected ",
                              paramCount, ", got ", expr->args.size());
        return ConstantValue::error();
    }

    // ─── 4. Evaluate arguments ──────────────────────────────────────────
    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evaluate(ctx, arg);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        args.push_back(val);
    }

    // ─── 5. Execute the function ────────────────────────────────────────
    return executeFunction(ctx, func, args);
}

// ─── Struct Literal Evaluation ──────────────────────────────────────────

ConstantValue ConstEvaluator::evalStructLiteral(SemaContext& ctx, const StructLiteralExprAST* expr) {
    const TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        return ConstantValue::error();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "'", ctx.pool.lookup(expr->typeName), "' is not a struct");
        return ConstantValue::error();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    std::unordered_map<InternedString, ConstantValue> fields;

    // ─── Initialize with default values ─────────────────────────────────
    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->defaultVal) {
            ConstantValue val = evaluate(ctx, field->defaultVal, field->type);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            fields[field->name] = val;
        }
    }

    // ─── Override with explicit initializers ────────────────────────────
    for (const FieldInitAST* init : expr->inits) {
        ConstantValue val = evaluate(ctx, init->value);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        fields[init->name] = val;
    }

    // ─── Check missing required fields ──────────────────────────────────
    for (const FieldDeclAST* field : structDecl->fields) {
        if (fields.find(field->name) == fields.end()) {
            // Check if field has a default
            if (field->defaultVal) continue;
            // Check if field is nullable/fallible (optional)
            if (isNullableType(field->type) || isFallibleType(field->type)) continue;
            // Check if field is combined (T?!) - must be explicit
            if (field->type->isa<CombinedTypeAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                      "combined field '", ctx.pool.lookup(field->name),
                                      "' (T?!) must be explicitly initialized");
                return ConstantValue::error();
            }
            
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                  "missing initializer for struct field '",
                                  ctx.pool.lookup(field->name), "'");
            return ConstantValue::error();
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Struct;
    result.value = fields;
    result.type = ctx.getNamedType(structDecl->name);
    return result;
}

// ─── Array Literal Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalArrayLiteral(SemaContext& ctx, const ArrayLiteralExprAST* expr) {
    std::vector<ConstantValue> elements;

    for (const ExprAST* elem : expr->elements) {
        ConstantValue val = evaluate(ctx, elem);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        elements.push_back(val);
    }

    if (!elements.empty()) {
        const TypeAST* firstType = elements[0].type;
        for (size_t i = 1; i < elements.size(); ++i) {
            if (!typesEqual(elements[i].type, firstType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr,
                                      "array elements must have the same type");
                return ConstantValue::error();
            }
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Array;
    result.value = elements;
    return result;
}

// ─── Field Access Evaluation ────────────────────────────────────────────

ConstantValue ConstEvaluator::evalFieldAccess(SemaContext& ctx, const FieldAccessExprAST* expr) {
    ConstantValue obj = evaluate(ctx, expr->object);
    if (obj.isError()) return obj;
    if (obj.isUnknown()) return ConstantValue::unknown();

    if (!obj.isStruct()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr->object,
                              "field access on non-struct value");
        return ConstantValue::error();
    }

    const auto& structFields = obj.asStruct();
    auto it = structFields.find(expr->fieldName);
    if (it == structFields.end()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "struct has no field '",
                              ctx.pool.lookup(expr->fieldName), "'");
        return ConstantValue::error();
    }

    return it->second;
}

// ─── Null Coalesce Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalNullCoalesce(SemaContext& ctx, const NullCoalesceExprAST* expr) {
    ConstantValue val = evaluate(ctx, expr->value);
    if (val.isError()) return val;

    if (val.isNil() || val.isErr()) {
        return evaluate(ctx, expr->fallback);
    }

    if (val.isUnknown()) return ConstantValue::unknown();
    return val;
}

// ─── If Expression Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalIfExpr(SemaContext& ctx, const IfExprAST* expr) {
    ConstantValue cond = evaluate(ctx, expr->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->condition,
                              "if condition must be bool");
        return ConstantValue::error();
    }

    if (cond.asBool()) {
        return evaluate(ctx, expr->thenBranch);
    } else {
        return evaluate(ctx, expr->elseBranch);
    }
}

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(SemaContext& ctx, const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            return executeBlock(ctx, stmt->as<BlockStmtAST>());
        case ASTKind::ReturnStmt:
            return executeReturn(ctx, stmt->as<ReturnStmtAST>());
        case ASTKind::IfStmt:
            return executeIf(ctx, stmt->as<IfStmtAST>());
        case ASTKind::WhileStmt:
            return executeWhile(ctx, stmt->as<WhileStmtAST>());
        case ASTKind::ExprStmt:
            return executeExprStmt(ctx, stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:
            return executeDeclStmt(ctx, stmt->as<DeclStmtAST>());
        default:
            return ConstantValue::unknown();
    }
}

ConstantValue ConstEvaluator::executeBlock(SemaContext& ctx, const BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    // Push a scope for the block
    ctx.pushScope();
    ConstantValue result = ConstantValue::voidValue();

    for (const StmtPtr stmt : block->stmts) {
        result = executeStmt(ctx, stmt);
        if (result.isError()) break;
        if (result.isUnknown()) break;
    }

    ctx.popScope();
    return result;
}

ConstantValue ConstEvaluator::executeReturn(SemaContext& ctx, const ReturnStmtAST* stmt) {
    if (stmt->value) {
        ConstantValue result = evaluate(ctx, stmt->value);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        return result;
    }
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeIf(SemaContext& ctx, const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // ─── 1. Push if context for type narrowing ──────────────────────────
    ScopedIfCondition ifContext(ctx, stmt->elseBranch != nullptr);

    // ─── 2. Evaluate condition ───────────────────────────────────────────
    ConstantValue cond = evaluate(ctx, stmt->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                              "if condition must be bool");
        return ConstantValue::error();
    }

    // ─── 3. Get narrowing info detected during condition evaluation ────
    NarrowingInfo info = ctx.stack.getPendingNarrowing();
    ctx.stack.clearPendingNarrowing();

    // ─── 4. Execute the appropriate branch ──────────────────────────────
    if (cond.asBool()) {
        // ─── Then branch ──────────────────────────────────────────────────
        if (stmt->thenBranch) {
            // Apply normal narrowing for inequality conditions (x != nil)
            // When x != nil is true, x is non-nullable
            if (info.hasNarrowing && !info.isEquality) {
                ctx.stack.pushNarrowingLevel(false);
                for (const auto& [name, type] : info.narrowings) {
                    ctx.stack.narrowVariable(name, type);
                }
                ConstantValue result = executeStmt(ctx, stmt->thenBranch);
                ctx.stack.popNarrowingLevel();
                return result;
            }
            return executeStmt(ctx, stmt->thenBranch);
        }
    } else {
        // ─── Else branch ──────────────────────────────────────────────────
        if (stmt->elseBranch) {
            // Apply inverse narrowing for equality conditions (x == nil)
            // When x == nil is false, x is definitely non-nullable
            if (info.hasNarrowing && info.isEquality) {
                ctx.stack.pushNarrowingLevel(true);
                for (const auto& [name, type] : info.narrowings) {
                    ctx.stack.narrowVariable(name, type);
                }
                ConstantValue result = executeStmt(ctx, stmt->elseBranch);
                ctx.stack.popNarrowingLevel();
                return result;
            }
            return executeStmt(ctx, stmt->elseBranch);
        }
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeWhile(SemaContext& ctx, const WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        if (++iterations > MAX_ITERATIONS) {
            ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, stmt,
                                  "while loop exceeded maximum iterations (",
                                  MAX_ITERATIONS, ")");
            return ConstantValue::error();
        }

        ConstantValue cond = evaluate(ctx, stmt->condition);
        if (cond.isError()) return cond;
        if (cond.isUnknown()) return ConstantValue::unknown();

        if (!cond.isBool()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                                  "while condition must be bool");
            return ConstantValue::error();
        }

        if (!cond.asBool()) break;

        ConstantValue result = executeStmt(ctx, stmt->body);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        if (result.isVoid()) continue;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeExprStmt(SemaContext& ctx, const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evaluate(ctx, stmt->expr);
    if (result.isError()) return result;
    if (result.isUnknown()) return ConstantValue::unknown();

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(SemaContext& ctx, const DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    if (stmt->decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const && var->init) {
            // Evaluate and register the const local
            ConstantValue val = evaluate(ctx, var->init);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            
            // Store the const value on the AST
            const_cast<ExprAST*>(var->init)->isConst = true;
            const_cast<ExprAST*>(var->init)->constValue = val;
            ctx.insertValue(var);
            return ConstantValue::voidValue();
        }
        
        // Mutable local variables are not allowed in const functions
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->decl,
                              "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    return ConstantValue::unknown();
}

// ─── Function Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeFunction(SemaContext& ctx, const FuncDeclAST* func,
                                               const std::vector<ConstantValue>& args) {
    if (!func) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                              "null function");
        return ConstantValue::error();
    }

    // ─── 1. Setup function context ──────────────────────────────────────
    ConstFunctionContext context(ctx, func);

    // ─── 2. Bind arguments to parameters ────────────────────────────────
    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                // Store the argument value on the parameter's initializer
                if (argIndex < args.size()) {
                    // Create a synthetic literal expression for the argument
                    // or store it directly on the parameter's type
                    // For now, we'll store it on the param's constValue
                    const_cast<ParamAST*>(param)->type = getConstantType(ctx, args[argIndex]);
                    const_cast<ParamAST*>(param)->isConst = true;
                    argIndex++;
                }
            }
        }
    }

    // ─── 3. Execute the body ─────────────────────────────────────────────
    ConstantValue result = ConstantValue::voidValue();
    if (func->body) {
        result = executeStmt(ctx, func->body);
    } else {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func,
                              "const function has no body");
        return ConstantValue::error();
    }

    // ─── 4. Check return type ────────────────────────────────────────────
    if (func->funcType && func->funcType->returnType) {
        if (result.isVoid()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func->body,
                                  "non-void const function does not return a value");
            return ConstantValue::error();
        }
    } else {
        if (!result.isVoid()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, func->body,
                                  "void const function returns a value");
            return ConstantValue::error();
        }
    }

    return result;
}

// ─── Dependency Analysis ─────────────────────────────────────────────────

void ConstEvaluator::collectDeps(SemaContext& ctx, const ExprAST* expr,
                                  std::vector<const DeclAST*>& deps) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = ctx.lookupValue(id->name);
            if (decl && decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    deps.push_back(var);
                }
            }
            if (decl && decl->isa<FuncDeclAST>()) {
                const FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    deps.push_back(func);
                }
            }
            break;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            collectDeps(ctx, bin->left, deps);
            collectDeps(ctx, bin->right, deps);
            break;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            collectDeps(ctx, unary->operand, deps);
            break;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            collectDeps(ctx, call->callee, deps);
            for (const ExprAST* arg : call->args) {
                collectDeps(ctx, arg, deps);
            }
            break;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            collectDeps(ctx, field->object, deps);
            break;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                collectDeps(ctx, init->value, deps);
            }
            break;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                collectDeps(ctx, elem, deps);
            }
            break;
        }
        default:
            break;
    }
}

void ConstEvaluator::collectDepsFromStmt(SemaContext& ctx, const StmtAST* stmt,
                                          std::vector<const DeclAST*>& deps) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            const BlockStmtAST* block = stmt->as<BlockStmtAST>();
            for (const StmtPtr s : block->stmts) {
                collectDepsFromStmt(ctx, s, deps);
            }
            break;
        }
        case ASTKind::ExprStmt: {
            const ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
            collectDeps(ctx, exprStmt->expr, deps);
            break;
        }
        case ASTKind::ReturnStmt: {
            const ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            if (ret->value) {
                collectDeps(ctx, ret->value, deps);
            }
            break;
        }
        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            collectDeps(ctx, ifStmt->condition, deps);
            collectDepsFromStmt(ctx, ifStmt->thenBranch, deps);
            if (ifStmt->elseBranch) {
                collectDepsFromStmt(ctx, ifStmt->elseBranch, deps);
            }
            break;
        }
        case ASTKind::WhileStmt: {
            const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            collectDeps(ctx, whileStmt->condition, deps);
            collectDepsFromStmt(ctx, whileStmt->body, deps);
            break;
        }
        case ASTKind::DeclStmt: {
            const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            if (declStmt->decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const && var->init) {
                    collectDeps(ctx, var->init, deps);
                }
            }
            break;
        }
        default:
            break;
    }
}

std::vector<const DeclAST*> ConstEvaluator::topologicalSort(SemaContext& ctx) {
    std::vector<const DeclAST*> result;
    std::unordered_map<const DeclAST*, size_t> inDegree;
    std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> graph;

    for (const auto& [decl, deps] : m_deps) {
        inDegree[decl] = 0;
        graph[decl] = {};
    }

    for (const auto& [decl, deps] : m_deps) {
        for (const DeclAST* dep : deps) {
            if (graph.find(dep) == graph.end()) continue;
            graph[decl].push_back(dep);
            inDegree[dep]++;
        }
    }

    std::queue<const DeclAST*> queue;
    for (const auto& [decl, degree] : inDegree) {
        if (degree == 0) {
            queue.push(decl);
        }
    }

    while (!queue.empty()) {
        const DeclAST* decl = queue.front();
        queue.pop();
        result.push_back(decl);

        for (const DeclAST* dep : graph[decl]) {
            inDegree[dep]--;
            if (inDegree[dep] == 0) {
                queue.push(dep);
            }
        }
    }

    if (result.size() != m_deps.size()) {
        std::vector<const DeclAST*> cycle;
        for (const auto& [decl, degree] : inDegree) {
            if (degree > 0) {
                cycle.push_back(decl);
            }
        }
        reportCycle(ctx, cycle);
    }

    return result;
}

} // namespace sema