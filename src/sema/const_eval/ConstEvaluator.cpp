/// @file const_eval/ConstEvaluator.cpp
/// @brief Implementation of ConstEvaluator.

#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"

#include <cmath>
#include <queue>
#include <algorithm>

namespace sema {

// ─── Static Member Initialization ────────────────────────────────────────

std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> ConstEvaluator::m_deps;
std::vector<const DeclAST*> ConstEvaluator::m_constDecls;
std::unordered_set<const ExprAST*> ConstEvaluator::m_evaluatedExprs;
std::unordered_set<const DeclAST*> ConstEvaluator::m_evaluating;
size_t ConstEvaluator::m_recursionDepth = 0;

// ─── Frame Management ────────────────────────────────────────────────────

ConstFrame& ConstEvaluator::currentFrame(std::vector<ConstFrame>& frames) {
    return frames.back();
}

const ConstFrame& ConstEvaluator::currentFrame(const std::vector<ConstFrame>& frames) {
    return frames.back();
}

void ConstEvaluator::pushFrame(std::vector<ConstFrame>& frames) {
    frames.emplace_back();
}

void ConstEvaluator::popFrame(std::vector<ConstFrame>& frames) {
    if (!frames.empty()) {
        frames.pop_back();
    }
}

ConstantValue ConstEvaluator::getLocal(std::vector<ConstFrame>& frames, InternedString name) {
    auto it = currentFrame(frames).locals.find(name);
    if (it != currentFrame(frames).locals.end()) {
        return it->second;
    }
    return ConstantValue::unknown();
}

void ConstEvaluator::setLocal(std::vector<ConstFrame>& frames, InternedString name, const ConstantValue& value) {
    currentFrame(frames).locals[name] = value;
}

bool ConstEvaluator::isLocalVariable(const std::vector<ConstFrame>& frames, InternedString name) {
    return currentFrame(frames).locals.find(name) != currentFrame(frames).locals.end();
}

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

    std::vector<ConstFrame> frames;
    pushFrame(frames);
    ConstantValue result = evaluate(ctx, decl->init);
    popFrame(frames);

    m_recursionDepth--;
    return result;
}

ConstantValue ConstEvaluator::evaluate(SemaContext& ctx, const ExprAST* expr) {
    if (!expr) return ConstantValue::error();

    if (m_evaluatedExprs.find(expr) != m_evaluatedExprs.end()) {
        return expr->constValue;
    }

    ConstantValue result;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = evalLiteral(ctx, expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr: {
            std::vector<ConstFrame> frames;
            pushFrame(frames);
            result = evalIdentifier(ctx, frames, expr->as<IdentifierExprAST>());
            popFrame(frames);
            break;
        }
        case ASTKind::BinaryExpr:
            result = evalBinary(ctx, expr->as<BinaryExprAST>());
            break;
        case ASTKind::UnaryExpr:
            result = evalUnary(ctx, expr->as<UnaryExprAST>());
            break;
        case ASTKind::CallExpr: {
            std::vector<ConstFrame> frames;
            pushFrame(frames);
            result = evalCall(ctx, frames, expr->as<CallExprAST>());
            popFrame(frames);
            break;
        }
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
            // Not const-evaluable - this is normal, no diagnostic
            return ConstantValue::unknown();
    }

    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(ctx, result);
        const_cast<ExprAST*>(expr)->valueState = ValueState::Definite;
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

// ─── Literal Evaluation ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalLiteral(SemaContext& ctx, const LiteralExprAST* expr) {
    switch (expr->kind) {
        case LiteralKind::True:
            return ConstantValue(true);
        case LiteralKind::False:
            return ConstantValue(false);
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                int64_t val = std::stoll(str, nullptr, 0);
                return ConstantValue(val);
            } catch (const std::exception&) {
                ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                      "invalid integer literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::Float: {
            std::string str = ctx.pool.lookup(expr->value);
            try {
                double val = std::stod(str);
                return ConstantValue(val);
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
        case LiteralKind::Nil:
            return ConstantValue::nil();
        case LiteralKind::Err:
            return ConstantValue::err();
        default:
            return ConstantValue::unknown();
    }
}

// ─── Identifier Evaluation ──────────────────────────────────────────────

ConstantValue ConstEvaluator::evalIdentifier(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                              const IdentifierExprAST* expr) {
    const TypeAST* narrowedType = ctx.stack.getNarrowedType(expr->name);
    if (narrowedType) {
        const ValueDeclAST* decl = ctx.lookupValue(expr->name);
        if (!decl) {
            return ConstantValue::error();
        }
        return getLocal(frames, expr->name);
    }

    const ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        return ConstantValue::error();
    }

    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        
        if (isLocalVariable(frames, expr->name)) {
            return getLocal(frames, expr->name);
        }
        
        if (var->keyword != DeclKeyword::Const) {
            return ConstantValue::unknown();
        }

        if (!var->init) {
            return ConstantValue::error();
        }

        if (m_evaluating.find(var) != m_evaluating.end()) {
            ctx.diagnostics.error(DiagCode::Sem_CircularDependency, expr,
                                  "cycle detected in const declaration '",
                                  ctx.pool.lookup(expr->name), "'");
            return ConstantValue::error();
        }

        return evaluate(ctx, var->init);
    }

    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            return ConstantValue::unknown();
        }
        return ConstantValue(func);
    }

    return ConstantValue::unknown();
}

// ─── Binary Expression Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalBinary(SemaContext& ctx, const BinaryExprAST* expr) {
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
        }
    }

    ConstantValue left = evaluate(ctx, expr->left);
    if (left.isError()) return left;
    if (left.isUnknown()) return ConstantValue::unknown();

    ConstantValue right = evaluate(ctx, expr->right);
    if (right.isError()) return right;
    if (right.isUnknown()) return ConstantValue::unknown();

    return evalBinaryOp(ctx, expr->op, left, right, expr);
}

// ─── Binary Operation Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                            const ConstantValue& left,
                                            const ConstantValue& right,
                                            const BaseAST* node) {
    auto bothNumeric = [](const ConstantValue& a, const ConstantValue& b) {
        return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
    };

    auto toDouble = [](const ConstantValue& v) -> double {
        if (v.isInt()) return static_cast<double>(v.asInt());
        if (v.isFloat()) return v.asFloat();
        return 0.0;
    };

    auto emitTypeError = [&](const char* op) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                              "invalid operands for '", op, "'");
        return ConstantValue::error();
    };

    switch (op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
        case BinaryOp::Add:
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
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) + toDouble(right));
            }
            if (left.isString() && right.isString()) {
                std::string result = ctx.pool.lookup(left.asString());
                result += ctx.pool.lookup(right.asString());
                return ConstantValue(ctx.pool.intern(result));
            }
            return emitTypeError("+");

        case BinaryOp::Sub:
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
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) - toDouble(right));
            }
            return emitTypeError("-");

        case BinaryOp::Mul:
            if (left.isInt() && right.isInt()) {
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if (r != 0 && l > INT64_MAX / r) {
                    ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                          "integer overflow in const multiplication");
                    return ConstantValue::error();
                }
                return ConstantValue(l * r);
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) * toDouble(right));
            }
            return emitTypeError("*");

        case BinaryOp::Div:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                                          "division by zero in const expression");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            if (bothNumeric(left, right)) {
                double divisor = toDouble(right);
                if (divisor == 0.0) {
                    ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                                          "division by zero in const expression");
                    return ConstantValue::error();
                }
                return ConstantValue(toDouble(left) / divisor);
            }
            return emitTypeError("/");

        case BinaryOp::Mod:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                                          "modulo by zero in const expression");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() % right.asInt());
            }
            return emitTypeError("%");

        case BinaryOp::Pow:
            if (bothNumeric(left, right)) {
                double result = std::pow(toDouble(left), toDouble(right));
                if (left.isInt() && right.isInt() && result == std::floor(result)) {
                    return ConstantValue(static_cast<int64_t>(result));
                }
                return ConstantValue(result);
            }
            return emitTypeError("**");

        // ─── Comparison ──────────────────────────────────────────────────
        case BinaryOp::Eq:
            return ConstantValue(compareEqual(left, right));
        case BinaryOp::Ne:
            return ConstantValue(!compareEqual(left, right));
        case BinaryOp::Lt:
            return ConstantValue(compareOrder(left, right, ctx) < 0);
        case BinaryOp::Gt:
            return ConstantValue(compareOrder(left, right, ctx) > 0);
        case BinaryOp::Le:
            return ConstantValue(compareOrder(left, right, ctx) <= 0);
        case BinaryOp::Ge:
            return ConstantValue(compareOrder(left, right, ctx) >= 0);

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
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() & right.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                  "bitwise AND requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitOr:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() | right.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                  "bitwise OR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() ^ right.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                  "bitwise XOR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shl:
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
                return ConstantValue(left.asInt() << right.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                  "shift requires integer operands");
            return ConstantValue::error();

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
                return ConstantValue(left.asInt() >> right.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                  "shift requires integer operands");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Unary Expression Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalUnary(SemaContext& ctx, const UnaryExprAST* expr) {
    ConstantValue operand = evaluate(ctx, expr->operand);
    if (operand.isError()) return operand;
    if (operand.isUnknown()) return ConstantValue::unknown();

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand.isInt()) {
                if (operand.asInt() == INT64_MIN) {
                    ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, expr,
                                          "integer overflow in const negation");
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
                                  "'not' requires bool operand");
            return ConstantValue::error();

        case UnaryOp::BitNot:
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "bitwise NOT requires integer operand");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Call Expression Evaluation ─────────────────────────────────────────

ConstantValue ConstEvaluator::evalCall(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                        const CallExprAST* expr) {
    ConstantValue callee = evaluate(ctx, expr->callee);
    if (callee.isError()) return callee;
    if (callee.isUnknown()) return ConstantValue::unknown();

    if (!callee.isFunction()) {
        return ConstantValue::error();
    }

    const FuncDeclAST* func = callee.asFunction();
    if (func->keyword != DeclKeyword::Const) {
        return ConstantValue::unknown();
    }

    for (AttributeAST* attr : func->attributes) {
        if (ctx.pool.lookup(attr->name) == "foreign") {
            ctx.diagnostics.error(DiagCode::Ffi_ConstContext, expr,
                                  "cannot call foreign function '",
                                  ctx.pool.lookup(func->name),
                                  "' in const context");
            return ConstantValue::error();
        }
    }

    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evaluate(ctx, arg);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        args.push_back(val);
    }

    return executeFunction(ctx, frames, func, args);
}

// ─── Struct Literal Evaluation ──────────────────────────────────────────

ConstantValue ConstEvaluator::evalStructLiteral(SemaContext& ctx, const StructLiteralExprAST* expr) {
    const TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        return ConstantValue::error();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "'", ctx.pool.lookup(expr->typeName),
                              "' is not a struct");
        return ConstantValue::error();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    std::unordered_map<InternedString, ConstantValue> fields;

    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->defaultVal) {
            ConstantValue val = evaluate(ctx, field->defaultVal);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            fields[field->name] = val;
        }
    }

    for (const FieldInitAST* init : expr->inits) {
        ConstantValue val = evaluate(ctx, init->value);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        fields[init->name] = val;
    }

    for (const FieldDeclAST* field : structDecl->fields) {
        if (fields.find(field->name) == fields.end() && !field->defaultVal) {
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
            if (elements[i].type != firstType) {
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

ConstantValue ConstEvaluator::executeStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                           const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    if (currentFrame(frames).hasReturned) {
        return currentFrame(frames).returnValue;
    }

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            return executeBlock(ctx, frames, stmt->as<BlockStmtAST>());
        case ASTKind::ReturnStmt:
            return executeReturn(ctx, frames, stmt->as<ReturnStmtAST>());
        case ASTKind::IfStmt:
            return executeIf(ctx, frames, stmt->as<IfStmtAST>());
        case ASTKind::WhileStmt:
            return executeWhile(ctx, frames, stmt->as<WhileStmtAST>());
        case ASTKind::ExprStmt:
            return executeExprStmt(ctx, frames, stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:
            return executeDeclStmt(ctx, frames, stmt->as<DeclStmtAST>());
        default:
            return ConstantValue::unknown();
    }
}

ConstantValue ConstEvaluator::executeBlock(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                            const BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    auto savedLocals = currentFrame(frames).locals;
    ConstantValue result = ConstantValue::voidValue();

    for (const StmtPtr stmt : block->stmts) {
        result = executeStmt(ctx, frames, stmt);
        if (result.isError()) break;
        if (result.isUnknown()) break;
        if (currentFrame(frames).hasReturned) break;
    }

    currentFrame(frames).locals = savedLocals;
    return result;
}

ConstantValue ConstEvaluator::executeReturn(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                              const ReturnStmtAST* stmt) {
    auto& frame = currentFrame(frames);

    if (stmt->value) {
        frame.returnValue = evaluate(ctx, stmt->value);
        if (frame.returnValue.isError()) return frame.returnValue;
        if (frame.returnValue.isUnknown()) return ConstantValue::unknown();
    } else {
        frame.returnValue = ConstantValue::voidValue();
    }

    frame.hasReturned = true;
    return frame.returnValue;
}

ConstantValue ConstEvaluator::executeIf(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                         const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    ConstIfContext ifContext(ctx, stmt->elseBranch != nullptr);

    ConstantValue cond = evaluate(ctx, stmt->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                              "if condition must be bool");
        return ConstantValue::error();
    }

    NarrowingInfo info = ctx.stack.getPendingNarrowing();
    ctx.stack.clearPendingNarrowing();

    if (cond.asBool()) {
        if (stmt->thenBranch) {
            if (info.hasNarrowing && !info.isEquality) {
                for (const auto& [name, type] : info.narrowings) {
                    ConstNarrowing narrow(ctx, name, type, false);
                }
            }
            return executeStmt(ctx, frames, stmt->thenBranch);
        }
    } else {
        if (stmt->elseBranch) {
            if (info.hasNarrowing && info.isEquality) {
                for (const auto& [name, type] : info.narrowings) {
                    ConstNarrowing narrow(ctx, name, type, true);
                }
            }
            return executeStmt(ctx, frames, stmt->elseBranch);
        }
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeWhile(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                             const WhileStmtAST* stmt) {
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

        ConstantValue result = executeStmt(ctx, frames, stmt->body);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        if (currentFrame(frames).hasReturned) break;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeAssign(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                              const AssignExprAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    ConstantValue rhs = evaluate(ctx, stmt->rhs);
    if (rhs.isError()) return rhs;
    if (rhs.isUnknown()) return ConstantValue::unknown();

    if (stmt->lhs->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = stmt->lhs->as<IdentifierExprAST>();
        setLocal(frames, id->name, rhs);
        return rhs;
    }

    ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->lhs,
                          "assignment target not supported in const function");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::executeExprStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                                const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evaluate(ctx, stmt->expr);
    if (result.isError()) return result;
    if (result.isUnknown()) return ConstantValue::unknown();

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                                const DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    if (stmt->decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const) {
            if (var->init) {
                ConstantValue val = evaluate(ctx, var->init);
                if (val.isError()) return val;
                if (val.isUnknown()) return ConstantValue::unknown();
                setLocal(frames, var->name, val);
                return ConstantValue::voidValue();
            }
        }
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->decl,
                              "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    return ConstantValue::unknown();
}

// ─── Function Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeFunction(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                               const FuncDeclAST* func,
                                               const std::vector<ConstantValue>& args) {
    if (!func) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                              "null function");
        return ConstantValue::error();
    }

    size_t paramCount = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        paramCount += group->params.size();
    }

    if (args.size() != paramCount) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, func,
                              "argument count mismatch: expected ",
                              paramCount, ", got ", args.size());
        return ConstantValue::error();
    }

    ConstFunctionContext context(ctx, func);
    pushFrame(frames);

    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                setLocal(frames, param->name, args[argIndex]);
                argIndex++;
            }
        }
    }

    ConstantValue result;
    if (func->body) {
        result = executeStmt(ctx, frames, func->body);
        if (currentFrame(frames).hasReturned) {
            result = currentFrame(frames).returnValue;
        } else if (func->funcType && !func->funcType->returnType) {
            result = ConstantValue::voidValue();
        } else if (!result.isError() && !result.isUnknown()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func->body,
                                  "non-void const function does not return a value");
            result = ConstantValue::error();
        }
    } else {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func,
                              "const function has no body");
        result = ConstantValue::error();
    }

    popFrame(frames);
    return result;
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

TypeAST* ConstEvaluator::getConstantType(SemaContext& ctx, const ConstantValue& val) {
    if (val.type) return val.type;

    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            return ctx.getBoolType();
        case ConstantValue::Kind::Int:
            return ctx.getIntType();
        case ConstantValue::Kind::Float:
            return ctx.getFloatType();
        case ConstantValue::Kind::String:
            return ctx.getStringType();
        case ConstantValue::Kind::Char:
            return ctx.getCharType();
        default:
            return nullptr;
    }
}

bool ConstEvaluator::compareEqual(const ConstantValue& a, const ConstantValue& b) {
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
            return a.asString() == b.asString();
        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err:
            return true;
        default:
            return false;
    }
}

int ConstEvaluator::compareOrder(const ConstantValue& a, const ConstantValue& b, SemaContext& ctx) {
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
            return ctx.pool.lookup(a.asString()).compare(
                   ctx.pool.lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Dependency Analysis ─────────────────────────────────────────────────

void ConstEvaluator::buildDependencyGraph(SemaContext& ctx) {
    m_deps.clear();
    m_constDecls.clear();

    for (ModuleAST* module : ctx.modules) {
        for (const DeclPtr decl : module->decls) {
            if (decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(var);
                }
            }
            if (decl->isa<FuncDeclAST>()) {
                const FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(func);
                }
            }
        }
    }

    for (const DeclAST* decl : m_constDecls) {
        std::vector<const DeclAST*> deps;
        if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->init) {
                collectDeps(ctx, var->init, deps);
            }
        } else if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                collectDepsFromStmt(ctx, func->body, deps);
            }
        }
        m_deps[decl] = deps;
    }
}

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