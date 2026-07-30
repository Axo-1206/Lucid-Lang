/// @file const_eval/ConstEvaluator.cpp
/// @brief Implementation of ConstEvaluator.

#include "ConstEvaluator.hpp"
#include "ConstInterpreter.hpp"
#include "../types/SemaType.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace sema {

// ─── Constructor ──────────────────────────────────────────────────────────

ConstEvaluator::ConstEvaluator(SemaContext& ctx) : m_ctx(ctx) {}

// ─── Main Entry Points ───────────────────────────────────────────────────

void ConstEvaluator::evaluateAll() {
    // 1. Build dependency graph
    buildDependencyGraph();

    // 2. Topological sort
    auto order = topologicalSort();

    // 3. Evaluate in order
    for (const DeclAST* decl : order) {
        if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->keyword == DeclKeyword::Const) {
                evaluateDecl(var);
            }
        }
        if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->keyword == DeclKeyword::Const) {
                evaluateDecl(func);
            }
        }
    }
}

ConstantValue ConstEvaluator::evaluateDecl(const DeclAST* decl) {
    // Check recursion limit
    if (m_recursionDepth > MAX_RECURSION) {
        reportError(decl, "const evaluation recursion limit exceeded");
        return ConstantValue::error();
    }

    // Check for cycles (detected during topological sort)
    if (m_evaluating.find(decl) != m_evaluating.end()) {
        return ConstantValue::error();
    }

    m_evaluating.insert(decl);
    m_recursionDepth++;

    ConstantValue result;

    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->init) {
            result = evalExpr(var->init);
            if (result.isError()) {
                reportError(var->init, "failed to evaluate const variable '"
                           + m_ctx.pool().lookup(var->name) + "'");
            }
            result.type = var->type;
        } else {
            reportError(decl, "const variable '" + m_ctx.pool().lookup(var->name)
                       + "' has no initializer");
            result = ConstantValue::error();
        }
    } else if (decl->isa<FuncDeclAST>()) {
        // Const functions are evaluated when called
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        result = ConstantValue(func);
        result.type = const_cast<FuncDeclAST*>(func)->funcType;
    }

    m_recursionDepth--;
    m_evaluating.erase(decl);

    return result;
}

bool ConstEvaluator::isEvaluated(const ExprAST* expr) const {
    return m_evaluatedExprs.find(expr) != m_evaluatedExprs.end();
}

ConstantValue ConstEvaluator::getValue(const ExprAST* expr) const {
    if (isEvaluated(expr)) {
        // The expression should have its constValue set
        return expr->constValue;
    }
    return ConstantValue::unknown();
}

// ─── Expression Evaluation ──────────────────────────────────────────────

ConstantValue ConstEvaluator::evalExpr(const ExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // Check if already evaluated
    if (isEvaluated(expr)) {
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
            reportError(expr, "not a constant expression");
            result = ConstantValue::error();
            break;
    }

    // Store result on the expression if successful
    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = result.type;
        const_cast<ExprAST*>(expr)->valueState = ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

ConstantValue ConstEvaluator::evalLiteral(const LiteralExprAST* expr) {
    switch (expr->kind) {
        case LiteralKind::True:
            return ConstantValue(true);
        case LiteralKind::False:
            return ConstantValue(false);
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string str = m_ctx.pool().lookup(expr->value);
            try {
                int64_t val = std::stoll(str, nullptr, 0);
                return ConstantValue(val);
            } catch (const std::exception&) {
                reportError(expr, "invalid integer literal '" + str + "'");
                return ConstantValue::error();
            }
        }
        case LiteralKind::Float: {
            std::string str = m_ctx.pool().lookup(expr->value);
            try {
                double val = std::stod(str);
                return ConstantValue(val);
            } catch (const std::exception&) {
                reportError(expr, "invalid float literal '" + str + "'");
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
            reportError(expr, "unsupported literal in const expression");
            return ConstantValue::error();
    }
}

ConstantValue ConstEvaluator::evalIdentifier(const IdentifierExprAST* expr) {
    // Look up the declaration
    const ValueDeclAST* decl = lookupValue(expr->name, m_ctx);
    if (!decl) {
        reportError(expr, "undefined identifier '" + m_ctx.pool().lookup(expr->name) + "'");
        return ConstantValue::error();
    }

    // Check if it's a const declaration
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->keyword != DeclKeyword::Const) {
            reportError(expr, "'" + m_ctx.pool().lookup(expr->name)
                       + "' is not const (declared as 'let')");
            return ConstantValue::error();
        }

        // Evaluate the const variable
        if (!var->init) {
            reportError(expr, "const variable '" + m_ctx.pool().lookup(expr->name)
                       + "' has no initializer");
            return ConstantValue::error();
        }

        // Check if we're currently evaluating this variable (cycle)
        if (m_evaluating.find(var) != m_evaluating.end()) {
            reportError(expr, "cycle detected: '" + m_ctx.pool().lookup(expr->name) + "'");
            return ConstantValue::error();
        }

        return evalExpr(var->init);
    }

    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            reportError(expr, "'" + m_ctx.pool().lookup(expr->name)
                       + "' is not const (declared as 'let')");
            return ConstantValue::error();
        }

        // Return function pointer for later call
        return ConstantValue(func);
    }

    reportError(expr, "'" + m_ctx.pool().lookup(expr->name)
               + "' is not a constant value");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalBinary(const BinaryExprAST* expr) {
    ConstantValue left = evalExpr(expr->left);
    if (left.isError()) return ConstantValue::error();

    ConstantValue right = evalExpr(expr->right);
    if (right.isError()) return ConstantValue::error();

    // Helper: check both are numeric
    auto bothNumeric = [](const ConstantValue& a, const ConstantValue& b) {
        return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
    };

    // Helper: get numeric values as double
    auto toDouble = [](const ConstantValue& v) -> double {
        if (v.isInt()) return static_cast<double>(v.asInt());
        if (v.isFloat()) return v.asFloat();
        return 0.0;
    };

    switch (expr->op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
        case BinaryOp::Add:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() + right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) + toDouble(right));
            }
            if (left.isString() && right.isString()) {
                // String concatenation at compile-time
                std::string result = m_ctx.pool().lookup(left.asString());
                result += m_ctx.pool().lookup(right.asString());
                return ConstantValue(m_ctx.pool().intern(result));
            }
            reportError(expr, "invalid operands for '+'");
            return ConstantValue::error();

        case BinaryOp::Sub:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() - right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) - toDouble(right));
            }
            reportError(expr, "invalid operands for '-'");
            return ConstantValue::error();

        case BinaryOp::Mul:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() * right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) * toDouble(right));
            }
            reportError(expr, "invalid operands for '*'");
            return ConstantValue::error();

        case BinaryOp::Div:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    reportError(expr->right, "division by zero");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            if (bothNumeric(left, right)) {
                double divisor = toDouble(right);
                if (divisor == 0.0) {
                    reportError(expr->right, "division by zero");
                    return ConstantValue::error();
                }
                return ConstantValue(toDouble(left) / divisor);
            }
            reportError(expr, "invalid operands for '/'");
            return ConstantValue::error();

        case BinaryOp::Mod:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    reportError(expr->right, "modulo by zero");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() % right.asInt());
            }
            reportError(expr, "modulo requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Pow:
            if (bothNumeric(left, right)) {
                double result = std::pow(toDouble(left), toDouble(right));
                if (left.isInt() && right.isInt() && result == std::floor(result)) {
                    return ConstantValue(static_cast<int64_t>(result));
                }
                return ConstantValue(result);
            }
            reportError(expr, "invalid operands for '**'");
            return ConstantValue::error();

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
            reportError(expr, "'and' requires bool operands");
            return ConstantValue::error();

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            reportError(expr, "'or' requires bool operands");
            return ConstantValue::error();

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() & right.asInt());
            }
            reportError(expr, "bitwise AND requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitOr:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() | right.asInt());
            }
            reportError(expr, "bitwise OR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() ^ right.asInt());
            }
            reportError(expr, "bitwise XOR requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shl:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    reportError(expr->right, "negative shift amount");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() << right.asInt());
            }
            reportError(expr, "shift requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    reportError(expr->right, "negative shift amount");
                    return ConstantValue::error();
                }
                return ConstantValue(left.asInt() >> right.asInt());
            }
            reportError(expr, "shift requires integer operands");
            return ConstantValue::error();

        default:
            reportError(expr, "unsupported binary operator in const expression");
            return ConstantValue::error();
    }
}

ConstantValue ConstEvaluator::evalUnary(const UnaryExprAST* expr) {
    ConstantValue operand = evalExpr(expr->operand);
    if (operand.isError()) return ConstantValue::error();

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand.isInt()) {
                return ConstantValue(-operand.asInt());
            }
            if (operand.isFloat()) {
                return ConstantValue(-operand.asFloat());
            }
            reportError(expr, "negation requires numeric operand");
            return ConstantValue::error();

        case UnaryOp::Not:
            if (operand.isBool()) {
                return ConstantValue(!operand.asBool());
            }
            reportError(expr, "'not' requires bool operand");
            return ConstantValue::error();

        case UnaryOp::BitNot:
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            reportError(expr, "bitwise NOT requires integer operand");
            return ConstantValue::error();

        default:
            reportError(expr, "unsupported unary operator in const expression");
            return ConstantValue::error();
    }
}

ConstantValue ConstEvaluator::evalCall(const CallExprAST* expr) {
    // Evaluate the callee
    ConstantValue callee = evalExpr(expr->callee);
    if (callee.isError()) return ConstantValue::error();

    // Must be a const function
    if (!callee.isFunction()) {
        reportError(expr->callee, "not a const function");
        return ConstantValue::error();
    }

    const FuncDeclAST* func = callee.asFunction();
    if (func->keyword != DeclKeyword::Const) {
        reportError(expr->callee, "function is not const (declared as 'let')");
        return ConstantValue::error();
    }

    // Evaluate arguments
    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evalExpr(arg);
        if (val.isError()) return ConstantValue::error();
        args.push_back(val);
    }

    // Execute the const function
    ConstInterpreter interpreter(*this, m_ctx);
    return interpreter.executeFunction(func, args);
}

ConstantValue ConstEvaluator::evalStructLiteral(const StructLiteralExprAST* expr) {
    // TODO: Implement struct literal evaluation
    // This requires resolving the struct type and evaluating each field

    reportError(expr, "struct literal evaluation not yet implemented");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalArrayLiteral(const ArrayLiteralExprAST* expr) {
    // TODO: Implement array literal evaluation

    reportError(expr, "array literal evaluation not yet implemented");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalFieldAccess(const FieldAccessExprAST* expr) {
    // TODO: Implement field access evaluation
    // This requires evaluating the object and extracting the field

    reportError(expr, "field access evaluation not yet implemented");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalNullCoalesce(const NullCoalesceExprAST* expr) {
    // TODO: Implement null coalesce evaluation

    reportError(expr, "null coalesce evaluation not yet implemented");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalIfExpr(const IfExprAST* expr) {
    // Evaluate condition
    ConstantValue cond = evalExpr(expr->condition);
    if (cond.isError()) return ConstantValue::error();

    if (!cond.isBool()) {
        reportError(expr->condition, "if condition must be bool");
        return ConstantValue::error();
    }

    // Evaluate the appropriate branch
    if (cond.asBool()) {
        return evalExpr(expr->thenBranch);
    } else {
        return evalExpr(expr->elseBranch);
    }
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

bool ConstEvaluator::isEvaluableType(const TypeAST* type) {
    if (!type) return false;

    // Primitive types are evaluable
    if (type->isa<PrimitiveTypeAST>()) return true;

    // Named types can be evaluable if they resolve to structs or enums
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        const TypeDeclAST* decl = lookupType(named->name, m_ctx);
        if (!decl) return false;

        // Structs and enums can be evaluated
        if (decl->isa<StructDeclAST>() || decl->isa<EnumDeclAST>()) {
            return true;
        }

        // Traits cannot be evaluated (no concrete values)
        if (decl->isa<TraitDeclAST>()) {
            return false;
        }

        return false;
    }

    // Array types are evaluable if element type is evaluable
    if (type->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* array = type->as<ArrayTypeAST>();
        return isEvaluableType(array->element);
    }

    // Function types are evaluable (function pointers)
    if (type->isa<FuncTypeAST>()) {
        return true;
    }

    // Nullable/fallible types are evaluable if inner type is evaluable
    if (type->isa<NullableTypeAST>()) {
        return isEvaluableType(type->as<NullableTypeAST>()->inner);
    }
    if (type->isa<FallibleTypeAST>()) {
        return isEvaluableType(type->as<FallibleTypeAST>()->inner);
    }
    if (type->isa<CombinedTypeAST>()) {
        return isEvaluableType(type->as<CombinedTypeAST>()->inner);
    }

    // Reference and pointer types are not evaluable
    if (type->isa<RefTypeAST>() || type->isa<PtrTypeAST>()) {
        return false;
    }

    return false;
}

TypeAST* ConstEvaluator::getConstantType(const ConstantValue& val) {
    // If we already have a type, use it
    if (val.type) return val.type;

    // Otherwise, infer from kind
    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            // TODO: Return bool type
            return nullptr;
        case ConstantValue::Kind::Int:
            // TODO: Return int type
            return nullptr;
        case ConstantValue::Kind::Float:
            // TODO: Return float type
            return nullptr;
        case ConstantValue::Kind::String:
            // TODO: Return string type
            return nullptr;
        default:
            return nullptr;
    }
}

bool ConstEvaluator::compareEqual(const ConstantValue& a, const ConstantValue& b) {
    // Different kinds are never equal
    if (a.kind != b.kind) return false;

    switch (a.kind) {
        case ConstantValue::Kind::Bool:
            return a.asBool() == b.asBool();
        case ConstantValue::Kind::Int:
            return a.asInt() == b.asInt();
        case ConstantValue::Kind::Float:
            return a.asFloat() == b.asFloat();
        case ConstantValue::Kind::String:
            return a.asString() == b.asString();
        case ConstantValue::Kind::Char:
            return a.asString() == b.asString();
        case ConstantValue::Kind::Enum:
            return a.asString() == b.asString();
        case ConstantValue::Kind::Nil:
            return true; // All nil are equal
        case ConstantValue::Kind::Err:
            return true; // All err are equal
        default:
            return false;
    }
}

int ConstEvaluator::compareOrder(const ConstantValue& a, const ConstantValue& b) {
    // Can only compare same kinds
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
            return m_ctx.pool().lookup(a.asString()).compare(
                   m_ctx.pool().lookup(b.asString()));
        case ConstantValue::Kind::Char:
            return m_ctx.pool().lookup(a.asString()).compare(
                   m_ctx.pool().lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Dependency Analysis ─────────────────────────────────────────────────

void ConstEvaluator::buildDependencyGraph() {
    // Walk all modules and collect const declarations
    std::vector<const DeclAST*> constDecls;

    for (ModuleAST* module : m_ctx.modules) {
        for (const DeclPtr decl : module->decls) {
            if (decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    constDecls.push_back(var);
                }
            }
            if (decl->isa<FuncDeclAST>()) {
                const FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    constDecls.push_back(func);
                }
            }
        }
    }

    // Build dependencies
    for (const DeclAST* decl : constDecls) {
        std::vector<const DeclAST*> deps;

        if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->init) {
                collectDeps(var->init, deps);
            }
        } else if (decl->isa<FuncDeclAST>()) {
            // Const functions depend on other const functions/variables they reference
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                // TODO: Walk the body to collect dependencies
                // For now, we'll handle this during evaluation
            }
        }

        m_deps[decl] = deps;
    }
}

void ConstEvaluator::collectDeps(const ExprAST* expr,
                                  std::vector<const DeclAST*>& deps) {
    if (!expr) return;

    // Walk the AST and collect identifiers that refer to const declarations
    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = lookupValue(id->name, m_ctx);
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

std::vector<const DeclAST*> ConstEvaluator::topologicalSort() {
    std::vector<const DeclAST*> result;
    std::unordered_set<const DeclAST*> visited;
    std::unordered_set<const DeclAST*> inStack;

    std::function<void(const DeclAST*)> dfs = [&](const DeclAST* node) {
        if (visited.find(node) != visited.end()) return;
        if (inStack.find(node) != inStack.end()) {
            // Cycle detected
            return;
        }

        inStack.insert(node);

        auto it = m_deps.find(node);
        if (it != m_deps.end()) {
            for (const DeclAST* dep : it->second) {
                dfs(dep);
            }
        }

        inStack.erase(node);
        visited.insert(node);
        result.push_back(node);
    };

    for (const auto& pair : m_deps) {
        if (visited.find(pair.first) == visited.end()) {
            dfs(pair.first);
        }
    }

    return result;
}

bool ConstEvaluator::detectCycle(std::vector<const DeclAST*>& cycle) {
    // TODO: Implement cycle detection
    // This is handled by the DFS in topologicalSort
    return false;
}

// ─── Error Reporting ─────────────────────────────────────────────────────

void ConstEvaluator::reportError(const BaseAST* node, const std::string& msg) {
    m_ctx.error(node, DiagCode::E3003, "const evaluation failed: ", msg);
}

void ConstEvaluator::reportCycle(const std::vector<const DeclAST*>& cycle) {
    std::string msg = "circular dependency in const declarations: ";
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) msg += " → ";
        msg += m_ctx.pool().lookup(cycle[i]->name);
    }
    if (!cycle.empty()) {
        m_ctx.error(cycle[0], DiagCode::E3003, msg);
    }
}

std::string ConstEvaluator::valueToString(const ConstantValue& val) const {
    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            return val.asBool() ? "true" : "false";
        case ConstantValue::Kind::Int:
            return std::to_string(val.asInt());
        case ConstantValue::Kind::Float:
            return std::to_string(val.asFloat());
        case ConstantValue::Kind::String:
            return m_ctx.pool().lookup(val.asString());
        case ConstantValue::Kind::Nil:
            return "nil";
        case ConstantValue::Kind::Err:
            return "err";
        default:
            return "<unknown>";
    }
}

} // namespace sema