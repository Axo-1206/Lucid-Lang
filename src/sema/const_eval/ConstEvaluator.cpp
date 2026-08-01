/// @file const_eval/ConstEvaluator.cpp
/// @brief Implementation of ConstEvaluator.

#include "ConstEvaluator.hpp"

#include <cmath>
#include <string>
#include <queue>
#include <algorithm>
#include <sstream>

namespace sema {

// ─── Constructor ──────────────────────────────────────────────────────────

ConstEvaluator::ConstEvaluator(SemaContext& ctx) : m_ctx(ctx) {
    // Push initial frame for top-level evaluation
    pushFrame();
}

// ─── Main Entry Points ───────────────────────────────────────────────────

ConstantValue ConstEvaluator::evaluateDecl(const VarDeclAST* decl) {
    if (!decl || !decl->init) {
        // Const variable with no initializer - this is an error
        m_ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                "const variable '", m_ctx.pool.lookup(decl->name),
                                "' has no initializer");
        return ConstantValue::error();
    }

    if (m_recursionDepth > MAX_RECURSION) {
        // Internal limit exceeded - this is a safety mechanism
        // We treat it as an error because the program is too complex
        m_ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, decl,
                                "const evaluation recursion limit exceeded (",
                                MAX_RECURSION, ")");
        return ConstantValue::error();
    }

    if (m_evaluating.find(decl) != m_evaluating.end()) {
        // Circular dependency - real error
        m_ctx.diagnostics.error(DiagCode::Sem_CircularDependency, decl,
                                "circular dependency detected in const declaration '",
                                m_ctx.pool.lookup(decl->name), "'");
        return ConstantValue::error();
    }

    EvaluationGuard guard(m_evaluating, decl);
    m_recursionDepth++;

    ConstantValue result = evalExpr(decl->init);
    
    m_recursionDepth--;
    return result;
}

// ─── Expression Evaluation ──────────────────────────────────────────────

ConstantValue ConstEvaluator::evalExpr(const ExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // Check if already evaluated
    if (m_evaluatedExprs.find(expr) != m_evaluatedExprs.end()) {
        return expr->constValue;
    }

    ConstantValue result;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = evalLiteral(expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr:
            result = evalIdentifier(expr->as<IdentifierExprAST>());
            break;
        case ASTKind::BinaryExpr:
            result = evalBinary(expr->as<BinaryExprAST>());
            break;
        case ASTKind::UnaryExpr:
            result = evalUnary(expr->as<UnaryExprAST>());
            break;
        case ASTKind::CallExpr:
            result = evalCall(expr->as<CallExprAST>());
            break;
        case ASTKind::StructLiteralExpr:
            result = evalStructLiteral(expr->as<StructLiteralExprAST>());
            break;
        case ASTKind::ArrayLiteralExpr:
            result = evalArrayLiteral(expr->as<ArrayLiteralExprAST>());
            break;
        case ASTKind::FieldAccessExpr:
            result = evalFieldAccess(expr->as<FieldAccessExprAST>());
            break;
        case ASTKind::NullCoalesceExpr:
            result = evalNullCoalesce(expr->as<NullCoalesceExprAST>());
            break;
        case ASTKind::IfExpr:
            result = evalIfExpr(expr->as<IfExprAST>());
            break;
        default:
            // Not const-evaluable - this is normal, no diagnostic
            // The expression will be evaluated at runtime
            return ConstantValue::unknown();
    }

    // Store result on the expression node (ExprAST is mutable)
    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(result);
        const_cast<ExprAST*>(expr)->valueState = ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

// ─── Literal Evaluation ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalLiteral(const LiteralExprAST* expr) {
    switch (expr->kind) {
        case LiteralKind::True:
            return ConstantValue(true);
        case LiteralKind::False:
            return ConstantValue(false);
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = m_ctx.pool.lookup(expr->value);
            try {
                int64_t val = std::stoll(str, nullptr, 0);
                return ConstantValue(val);
            } catch (const std::exception&) {
                // Invalid literal - this is a lexical error, but we report it
                // as a const eval error since it appears in a const context
                m_ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
                                        "invalid integer literal '", str, "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::Float: {
            std::string str = m_ctx.pool.lookup(expr->value);
            try {
                double val = std::stod(str);
                return ConstantValue(val);
            } catch (const std::exception&) {
                m_ctx.diagnostics.error(DiagCode::Lex_InvalidNumberLiteral, expr,
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
            // Unsupported literal - not a valid const expression
            return ConstantValue::unknown();
    }
}

// ─── Identifier Evaluation ──────────────────────────────────────────────

ConstantValue ConstEvaluator::evalIdentifier(const IdentifierExprAST* expr) {
    // ─── 1. Check for narrowed type (from if conditions) ──────────────────
    const TypeAST* narrowedType = m_ctx.stack.getNarrowedType(expr->name);
    if (narrowedType) {
        // Variable has been narrowed - get its value
        const ValueDeclAST* decl = m_ctx.lookupValue(expr->name);
        if (!decl) {
            // Name resolution should have caught this, but handle gracefully
            return ConstantValue::error();
        }
        return getLocal(expr->name);
    }

    // ─── 2. Look up in symbol table ──────────────────────────────────────
    const ValueDeclAST* decl = m_ctx.lookupValue(expr->name);
    if (!decl) {
        // Name resolution should have already reported this
        // We just return error silently
        return ConstantValue::error();
    }

    // ─── 3. Check if it's a const declaration ────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        
        // Local variable (in current frame)
        if (isLocalVariable(expr->name)) {
            return getLocal(expr->name);
        }
        
        // Global const variable
        if (var->keyword != DeclKeyword::Const) {
            // Not const-evaluable - this is normal
            return ConstantValue::unknown();
        }

        if (!var->init) {
            // Const without initializer - should have been caught earlier
            return ConstantValue::error();
        }

        if (m_evaluating.find(var) != m_evaluating.end()) {
            // Circular dependency - real error
            m_ctx.diagnostics.error(DiagCode::Sem_CircularDependency, expr,
                                    "cycle detected in const declaration '",
                                    m_ctx.pool.lookup(expr->name), "'");
            return ConstantValue::error();
        }

        return evalExpr(var->init);
    }

    // ─── 4. Function reference ────────────────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            // Non-const function - not const-evaluable
            return ConstantValue::unknown();
        }
        return ConstantValue(func);
    }

    // Not a constant value
    return ConstantValue::unknown();
}

// ─── Binary Expression Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalBinary(const BinaryExprAST* expr) {
    // ─── 1. Check for type narrowing pattern ─────────────────────────────
    if (m_ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, m_ctx);
        if (info.hasNarrowing) {
            m_ctx.stack.setPendingNarrowing(info);
        }
    }

    // ─── 2. Evaluate operands ────────────────────────────────────────────
    ConstantValue left = evalExpr(expr->left);
    if (left.isError()) return left;
    if (left.isUnknown()) return ConstantValue::unknown();

    ConstantValue right = evalExpr(expr->right);
    if (right.isError()) return right;
    if (right.isUnknown()) return ConstantValue::unknown();

    // ─── 3. Evaluate operation ──────────────────────────────────────────
    return evalBinaryOp(expr->op, left, right, expr);
}

// ─── Binary Operation Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(BinaryOp op, 
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
        m_ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                "invalid operands for '", op, "'");
        return ConstantValue::error();
    };

    switch (op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
        case BinaryOp::Add:
            if (left.isInt() && right.isInt()) {
                // Check for overflow
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r)) {
                    m_ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                            "integer overflow in const addition");
                    return ConstantValue::error();
                }
                return ConstantValue(l + r);
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) + toDouble(right));
            }
            if (left.isString() && right.isString()) {
                std::string result = m_ctx.pool.lookup(left.asString());
                result += m_ctx.pool.lookup(right.asString());
                return ConstantValue(m_ctx.pool.intern(result));
            }
            return emitTypeError("+");

        case BinaryOp::Sub:
            if (left.isInt() && right.isInt()) {
                int64_t l = left.asInt();
                int64_t r = right.asInt();
                if ((r > 0 && l < INT64_MIN + r) || (r < 0 && l > INT64_MAX + r)) {
                    m_ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
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
                    m_ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
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
                    m_ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                                            "division by zero in const expression");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            if (bothNumeric(left, right)) {
                double divisor = toDouble(right);
                if (divisor == 0.0) {
                    m_ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                                            "division by zero in const expression");
                    return ConstantValue::error();
                }
                return ConstantValue(toDouble(left) / divisor);
            }
            return emitTypeError("/");

        case BinaryOp::Mod:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    m_ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
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
            return ConstantValue(compareOrder(left, right) < 0);
        case BinaryOp::Gt:
            return ConstantValue(compareOrder(left, right) > 0);
        case BinaryOp::Le:
            return ConstantValue(compareOrder(left, right) <= 0);
        case BinaryOp::Ge:
            return ConstantValue(compareOrder(left, right) >= 0);

        // ─── Logical ──────────────────────────────────────────────────────
        case BinaryOp::And:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() && right.asBool());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                    "'and' requires bool operands");
            return ConstantValue::error();

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                    "'or' requires bool operands");
            return ConstantValue::error();

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() & right.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                    "bitwise AND requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitOr:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() | right.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                    "bitwise OR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() ^ right.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                    "bitwise XOR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shl:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    m_ctx.diagnostics.error(DiagCode::Sem_NegativeShift, node,
                                            "negative shift amount in const expression");
                    return ConstantValue::error();
                }
                if (right.asInt() >= 64) {
                    m_ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                            "shift amount exceeds bit width");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() << right.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                    "shift requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    m_ctx.diagnostics.error(DiagCode::Sem_NegativeShift, node,
                                            "negative shift amount in const expression");
                    return ConstantValue::error();
                }
                if (right.asInt() >= 64) {
                    m_ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                            "shift amount exceeds bit width");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() >> right.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                    "shift requires integer operands");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Unary Expression Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalUnary(const UnaryExprAST* expr) {
    ConstantValue operand = evalExpr(expr->operand);
    if (operand.isError()) return operand;
    if (operand.isUnknown()) return ConstantValue::unknown();

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand.isInt()) {
                if (operand.asInt() == INT64_MIN) {
                    m_ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, expr,
                                            "integer overflow in const negation");
                    return ConstantValue::error();
                }
                return ConstantValue(-operand.asInt());
            }
            if (operand.isFloat()) {
                return ConstantValue(-operand.asFloat());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                    "negation requires numeric operand");
            return ConstantValue::error();

        case UnaryOp::Not:
            if (operand.isBool()) {
                return ConstantValue(!operand.asBool());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                    "'not' requires bool operand");
            return ConstantValue::error();

        case UnaryOp::BitNot:
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            m_ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                    "bitwise NOT requires integer operand");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

// ─── Call Expression Evaluation ─────────────────────────────────────────

ConstantValue ConstEvaluator::evalCall(const CallExprAST* expr) {
    // ─── 1. Evaluate callee ──────────────────────────────────────────────
    ConstantValue callee = evalExpr(expr->callee);
    if (callee.isError()) return callee;
    if (callee.isUnknown()) return ConstantValue::unknown();

    if (!callee.isFunction()) {
        // Not a function - this is a type error, already reported
        return ConstantValue::error();
    }

    const FuncDeclAST* func = callee.asFunction();
    if (func->keyword != DeclKeyword::Const) {
        // Non-const function - not const-evaluable
        return ConstantValue::unknown();
    }

    // Check for foreign attribute
    for (AttributeAST* attr : func->attributes) {
        if (m_ctx.pool.lookup(attr->name) == "foreign") {
            // Foreign functions cannot be evaluated at compile time
            // This is a real error - user tried to call foreign in const context
            m_ctx.diagnostics.error(DiagCode::Ffi_ConstContext, expr,
                                    "cannot call foreign function '",
                                    m_ctx.pool.lookup(func->name),
                                    "' in const context");
            return ConstantValue::error();
        }
    }

    // ─── 2. Evaluate arguments ────────────────────────────────────────────
    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evalExpr(arg);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        args.push_back(val);
    }

    // ─── 3. Execute the const function ────────────────────────────────────
    return executeFunction(func, args);
}

// ─── Struct Literal Evaluation ──────────────────────────────────────────

ConstantValue ConstEvaluator::evalStructLiteral(const StructLiteralExprAST* expr) {
    // ─── 1. Look up struct declaration ────────────────────────────────────
    const TypeDeclAST* typeDecl = m_ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        // Type not found - already reported by name resolution
        return ConstantValue::error();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        m_ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                "'", m_ctx.pool.lookup(expr->typeName),
                                "' is not a struct");
        return ConstantValue::error();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── 2. Build struct value ────────────────────────────────────────────
    std::unordered_map<InternedString, ConstantValue> fields;

    // First, collect default values from struct fields
    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->defaultVal) {
            ConstantValue val = evalExpr(field->defaultVal);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            fields[field->name] = val;
        }
    }

    // Override with explicit initializers
    for (const FieldInitAST* init : expr->inits) {
        ConstantValue val = evalExpr(init->value);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        fields[init->name] = val;
    }

    // ─── 3. Verify all required fields are initialized ────────────────────
    for (const FieldDeclAST* field : structDecl->fields) {
        if (fields.find(field->name) == fields.end() && !field->defaultVal) {
            m_ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                    "missing initializer for struct field '",
                                    m_ctx.pool.lookup(field->name), "'");
            return ConstantValue::error();
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Struct;
    result.value = fields;
    result.type = m_ctx.getNamedType(structDecl->name);
    return result;
}

// ─── Array Literal Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalArrayLiteral(const ArrayLiteralExprAST* expr) {
    std::vector<ConstantValue> elements;

    for (const ExprAST* elem : expr->elements) {
        ConstantValue val = evalExpr(elem);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        elements.push_back(val);
    }

    // Verify all elements have the same type
    if (!elements.empty()) {
        const TypeAST* firstType = elements[0].type;
        for (size_t i = 1; i < elements.size(); ++i) {
            if (elements[i].type != firstType) {
                m_ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr,
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

ConstantValue ConstEvaluator::evalFieldAccess(const FieldAccessExprAST* expr) {
    ConstantValue obj = evalExpr(expr->object);
    if (obj.isError()) return obj;
    if (obj.isUnknown()) return ConstantValue::unknown();

    if (!obj.isStruct()) {
        m_ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr->object,
                                "field access on non-struct value");
        return ConstantValue::error();
    }

    const auto& structFields = obj.asStruct();
    auto it = structFields.find(expr->fieldName);
    if (it == structFields.end()) {
        m_ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                                "struct has no field '",
                                m_ctx.pool.lookup(expr->fieldName), "'");
        return ConstantValue::error();
    }

    return it->second;
}

// ─── Null Coalesce Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalNullCoalesce(const NullCoalesceExprAST* expr) {
    ConstantValue val = evalExpr(expr->value);
    if (val.isError()) return val;

    // If value is nil or err, evaluate fallback
    if (val.isNil() || val.isErr()) {
        return evalExpr(expr->fallback);
    }

    if (val.isUnknown()) return ConstantValue::unknown();
    return val;
}

// ─── If Expression Evaluation ───────────────────────────────────────────

ConstantValue ConstEvaluator::evalIfExpr(const IfExprAST* expr) {
    ConstantValue cond = evalExpr(expr->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        m_ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->condition,
                                "if condition must be bool");
        return ConstantValue::error();
    }

    if (cond.asBool()) {
        return evalExpr(expr->thenBranch);
    } else {
        return evalExpr(expr->elseBranch);
    }
}

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    if (currentFrame().hasReturned) {
        return currentFrame().returnValue;
    }

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            return executeBlock(stmt->as<BlockStmtAST>());
        case ASTKind::ReturnStmt:
            return executeReturn(stmt->as<ReturnStmtAST>());
        case ASTKind::IfStmt:
            return executeIf(stmt->as<IfStmtAST>());
        case ASTKind::WhileStmt:
            return executeWhile(stmt->as<WhileStmtAST>());
        case ASTKind::ExprStmt:
            return executeExprStmt(stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:
            return executeDeclStmt(stmt->as<DeclStmtAST>());
        default:
            // Unsupported statement - not const-evaluable
            return ConstantValue::unknown();
    }
}

ConstantValue ConstEvaluator::executeBlock(const BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    // Save current locals (for nested blocks)
    auto savedLocals = currentFrame().locals;

    ConstantValue result = ConstantValue::voidValue();

    for (const StmtPtr stmt : block->stmts) {
        result = executeStmt(stmt);
        if (result.isError()) break;
        if (result.isUnknown()) break;
        if (currentFrame().hasReturned) break;
    }

    // Restore locals (block scope)
    currentFrame().locals = savedLocals;

    return result;
}

ConstantValue ConstEvaluator::executeReturn(const ReturnStmtAST* stmt) {
    auto& frame = currentFrame();

    if (stmt->value) {
        frame.returnValue = evalExpr(stmt->value);
        if (frame.returnValue.isError()) return frame.returnValue;
        if (frame.returnValue.isUnknown()) return ConstantValue::unknown();
    } else {
        frame.returnValue = ConstantValue::voidValue();
    }

    frame.hasReturned = true;
    return frame.returnValue;
}

ConstantValue ConstEvaluator::executeIf(const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    ConstIfContext ifContext(m_ctx, stmt->elseBranch != nullptr);

    ConstantValue cond = evalExpr(stmt->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        m_ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                                "if condition must be bool");
        return ConstantValue::error();
    }

    NarrowingInfo info = m_ctx.stack.getPendingNarrowing();
    m_ctx.stack.clearPendingNarrowing();

    if (cond.asBool()) {
        if (stmt->thenBranch) {
            if (info.hasNarrowing && !info.isEquality) {
                for (const auto& [name, type] : info.narrowings) {
                    ConstNarrowing narrow(m_ctx, name, type, false);
                }
            }
            return executeStmt(stmt->thenBranch);
        }
    } else {
        if (stmt->elseBranch) {
            if (info.hasNarrowing && info.isEquality) {
                for (const auto& [name, type] : info.narrowings) {
                    ConstNarrowing narrow(m_ctx, name, type, true);
                }
            }
            return executeStmt(stmt->elseBranch);
        }
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeWhile(const WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        if (++iterations > MAX_ITERATIONS) {
            m_ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, stmt,
                                    "while loop exceeded maximum iterations (",
                                    MAX_ITERATIONS, ")");
            return ConstantValue::error();
        }

        ConstantValue cond = evalExpr(stmt->condition);
        if (cond.isError()) return cond;
        if (cond.isUnknown()) return ConstantValue::unknown();

        if (!cond.isBool()) {
            m_ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                                    "while condition must be bool");
            return ConstantValue::error();
        }

        if (!cond.asBool()) break;

        ConstantValue result = executeStmt(stmt->body);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        if (currentFrame().hasReturned) break;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeAssign(const AssignExprAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    ConstantValue rhs = evalExpr(stmt->rhs);
    if (rhs.isError()) return rhs;
    if (rhs.isUnknown()) return ConstantValue::unknown();

    if (stmt->lhs->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = stmt->lhs->as<IdentifierExprAST>();
        setLocal(id->name, rhs);
        return rhs;
    }

    m_ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->lhs,
                            "assignment target not supported in const function");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::executeExprStmt(const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evalExpr(stmt->expr);
    if (result.isError()) return result;
    if (result.isUnknown()) return ConstantValue::unknown();

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(const DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    if (stmt->decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const) {
            if (var->init) {
                ConstantValue val = evalExpr(var->init);
                if (val.isError()) return val;
                if (val.isUnknown()) return ConstantValue::unknown();
                setLocal(var->name, val);
                return ConstantValue::voidValue();
            }
        }
        // Mutable local variables not allowed in const functions
        m_ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->decl,
                                "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    // Declaration not supported
    return ConstantValue::unknown();
}

// ─── Function Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeFunction(
    const FuncDeclAST* func,
    const std::vector<ConstantValue>& args) {

    if (!func) {
        m_ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                                "null function");
        return ConstantValue::error();
    }

    // Check parameter count
    size_t paramCount = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        paramCount += group->params.size();
    }

    if (args.size() != paramCount) {
        m_ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, func,
                                "argument count mismatch: expected ",
                                paramCount, ", got ", args.size());
        return ConstantValue::error();
    }

    // ─── Use existing context system ──────────────────────────────────────
    ConstFunctionContext context(m_ctx, func);
    pushFrame();

    // Bind parameters
    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                setLocal(param->name, args[argIndex]);
                argIndex++;
            }
        }
    }

    // Execute body
    ConstantValue result;
    if (func->body) {
        result = executeStmt(func->body);
        if (currentFrame().hasReturned) {
            result = currentFrame().returnValue;
        } else if (func->funcType && !func->funcType->returnType) {
            result = ConstantValue::voidValue();
        } else if (!result.isError() && !result.isUnknown()) {
            m_ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func->body,
                                    "non-void const function does not return a value");
            result = ConstantValue::error();
        }
    } else {
        // Function has no body - this is an error
        m_ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func,
                                "const function has no body");
        result = ConstantValue::error();
    }

    popFrame();
    return result;
}

// ─── Frame Management ────────────────────────────────────────────────────

ConstantValue ConstEvaluator::getLocal(InternedString name) const {
    auto it = currentFrame().locals.find(name);
    if (it != currentFrame().locals.end()) {
        return it->second;
    }
    return ConstantValue::unknown();
}

void ConstEvaluator::setLocal(InternedString name, const ConstantValue& value) {
    currentFrame().locals[name] = value;
}

bool ConstEvaluator::isLocalVariable(InternedString name) const {
    return currentFrame().locals.find(name) != currentFrame().locals.end();
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

TypeAST* ConstEvaluator::getConstantType(const ConstantValue& val) {
    if (val.type) return val.type;

    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            return m_ctx.getBoolType();
        case ConstantValue::Kind::Int:
            return m_ctx.getIntType();
        case ConstantValue::Kind::Float:
            return m_ctx.getFloatType();
        case ConstantValue::Kind::String:
            return m_ctx.getStringType();
        case ConstantValue::Kind::Char:
            return m_ctx.getCharType();
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

int ConstEvaluator::compareOrder(const ConstantValue& a, const ConstantValue& b) {
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
            return m_ctx.pool.lookup(a.asString()).compare(
                   m_ctx.pool.lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Error Reporting ─────────────────────────────────────────────────────

void ConstEvaluator::reportCycle(const std::vector<const DeclAST*>& cycle) {
    if (cycle.empty()) return;
    
    std::string msg = "circular dependency in const declarations: ";
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) msg += " → ";
        msg += m_ctx.pool.lookup(cycle[i]->name);
    }
    m_ctx.diagnostics.error(DiagCode::Sem_CircularDependency, cycle[0], msg);
}

// ─── Dependency Analysis ─────────────────────────────────────────────────

void ConstEvaluator::buildDependencyGraph() {
    m_deps.clear();
    m_constDecls.clear();

    for (ModuleAST* module : m_ctx.modules) {
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
                collectDeps(var->init, deps);
            }
        } else if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                collectDepsFromStmt(func->body, deps);
            }
        }
        m_deps[decl] = deps;
    }
}

void ConstEvaluator::collectDeps(const ExprAST* expr,
                                  std::vector<const DeclAST*>& deps) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = m_ctx.lookupValue(id->name);
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
            collectDeps(bin->left, deps);
            collectDeps(bin->right, deps);
            break;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            collectDeps(unary->operand, deps);
            break;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            collectDeps(call->callee, deps);
            for (const ExprAST* arg : call->args) {
                collectDeps(arg, deps);
            }
            break;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            collectDeps(field->object, deps);
            break;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                collectDeps(init->value, deps);
            }
            break;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                collectDeps(elem, deps);
            }
            break;
        }
        default:
            break;
    }
}

void ConstEvaluator::collectDepsFromStmt(const StmtAST* stmt,
                                          std::vector<const DeclAST*>& deps) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            const BlockStmtAST* block = stmt->as<BlockStmtAST>();
            for (const StmtPtr s : block->stmts) {
                collectDepsFromStmt(s, deps);
            }
            break;
        }
        case ASTKind::ExprStmt: {
            const ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
            collectDeps(exprStmt->expr, deps);
            break;
        }
        case ASTKind::ReturnStmt: {
            const ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            if (ret->value) {
                collectDeps(ret->value, deps);
            }
            break;
        }
        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            collectDeps(ifStmt->condition, deps);
            collectDepsFromStmt(ifStmt->thenBranch, deps);
            if (ifStmt->elseBranch) {
                collectDepsFromStmt(ifStmt->elseBranch, deps);
            }
            break;
        }
        case ASTKind::WhileStmt: {
            const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            collectDeps(whileStmt->condition, deps);
            collectDepsFromStmt(whileStmt->body, deps);
            break;
        }
        case ASTKind::DeclStmt: {
            const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            if (declStmt->decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const && var->init) {
                    collectDeps(var->init, deps);
                }
            }
            break;
        }
        default:
            break;
    }
}

std::vector<const DeclAST*> ConstEvaluator::topologicalSort() {
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
        reportCycle(cycle);
    }

    return result;
}

} // namespace sema