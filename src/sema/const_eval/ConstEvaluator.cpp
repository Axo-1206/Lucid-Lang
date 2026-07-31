/// @file const_eval/ConstEvaluator.cpp
/// @brief Implementation of ConstEvaluator.

#include "ConstEvaluator.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <cmath>
#include <string>
#include <queue>
#include <algorithm>
#include <sstream>

namespace sema {

// ─── Constructor ──────────────────────────────────────────────────────────

ConstEvaluator::ConstEvaluator(SemaContext& ctx) : m_ctx(ctx) {}

// ─── Main Entry Points ───────────────────────────────────────────────────

void ConstEvaluator::evaluateAll() {
    buildDependencyGraph();
    auto order = topologicalSort();

    for (const DeclAST* decl : order) {
        if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->keyword == DeclKeyword::Const) {
                evaluateDecl(var);
            }
        }
    }
}

ConstantValue ConstEvaluator::evaluateDecl(const VarDeclAST* decl) {
    if (m_recursionDepth > MAX_RECURSION) {
        return error(decl, "const evaluation recursion limit exceeded");
    }

    if (m_evaluating.find(decl) != m_evaluating.end()) {
        return error(decl, "circular dependency detected");
    }

    EvaluationGuard guard(m_evaluating, decl);
    m_recursionDepth++;

    ConstantValue result;
    if (decl->init) {
        result = evalExpr(decl->init);
        if (result.isError()) {
            return error(decl->init, "failed to evaluate const variable '" 
                       + m_ctx.pool().lookup(decl->name) + "'");
        }
        // Store the result on the INITIALIZER expression (ExprAST), not the declaration
        // The declaration itself doesn't store the value - the expression does
        result.type = decl->type;
    } else {
        return error(decl, "const variable '" + m_ctx.pool().lookup(decl->name)
                   + "' has no initializer");
    }

    m_recursionDepth--;
    return result;
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
            return error(expr, "not a constant expression");
    }

    // ─── Store result on the EXPRESSION node (ExprAST is mutable) ──────
    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(result);
        const_cast<ExprAST*>(expr)->valueState = ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

ConstantValue ConstEvaluator::evalIdentifier(const IdentifierExprAST* expr) {
    // ─── 1. Check for narrowed type ──────────────────────────────────────
    const TypeAST* narrowedType = m_ctx.contexts.getNarrowedType(expr->name);
    if (narrowedType) {
        const ValueDeclAST* decl = m_ctx.symbols.lookupValue(expr->name);
        if (!decl) {
            return error(expr, "undefined identifier '" + m_ctx.pool().lookup(expr->name) + "'");
        }
        return getLocalValue(expr->name);
    }

    // ─── 2. Look up in SymbolStorage ────────────────────────────────────
    const ValueDeclAST* decl = m_ctx.symbols.lookupValue(expr->name);
    if (!decl) {
        return error(expr, "undefined identifier '" + m_ctx.pool().lookup(expr->name) + "'");
    }

    // ─── 3. Check if it's a const declaration ────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        
        // Check if it's a local variable (in SymbolStorage scopes)
        if (isLocalVariable(expr->name)) {
            return getLocalValue(expr->name);
        }
        
        // Global const variable - evaluate its initializer
        if (var->keyword != DeclKeyword::Const) {
            return error(expr, "'" + m_ctx.pool().lookup(expr->name)
                       + "' is not const (declared as 'let')");
        }

        if (!var->init) {
            return error(expr, "const variable '" + m_ctx.pool().lookup(expr->name)
                       + "' has no initializer");
        }

        if (m_evaluating.find(var) != m_evaluating.end()) {
            return error(expr, "cycle detected: '" + m_ctx.pool().lookup(expr->name) + "'");
        }

        // Evaluate the initializer expression
        return evalExpr(var->init);
    }

    if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        if (func->keyword != DeclKeyword::Const) {
            return error(expr, "'" + m_ctx.pool().lookup(expr->name)
                       + "' is not const (declared as 'let')");
        }
        return ConstantValue(func);
    }

    return error(expr, "'" + m_ctx.pool().lookup(expr->name)
               + "' is not a constant value");
}

ConstantValue ConstEvaluator::evalBinary(const BinaryExprAST* expr) {
    // ─── 1. Check for type narrowing pattern ─────────────────────────────
    // Use the existing detectNarrowingPattern from TypeNarrowHelpers
    if (m_ctx.contexts.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, m_ctx);
        if (info.hasNarrowing) {
            m_ctx.contexts.setPendingNarrowing(info);
        }
    }

    // ─── 2. Evaluate operands ────────────────────────────────────────────
    ConstantValue left = evalExpr(expr->left);
    if (left.isError()) return left;

    ConstantValue right = evalExpr(expr->right);
    if (right.isError()) return right;

    // ─── 3. Evaluate operation ──────────────────────────────────────────
    return evalBinaryOp(expr->op, left, right, expr);
}

// ─── Binary Operation Evaluation ────────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(BinaryOp op, 
                                            const ConstantValue& left,
                                            const ConstantValue& right,
                                            const BaseAST* node) {
    // Helper: check if both are numeric
    auto bothNumeric = [](const ConstantValue& a, const ConstantValue& b) {
        return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
    };

    // Helper: get numeric value as double
    auto toDouble = [](const ConstantValue& v) -> double {
        if (v.isInt()) return static_cast<double>(v.asInt());
        if (v.isFloat()) return v.asFloat();
        return 0.0;
    };

    switch (op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
        case BinaryOp::Add:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() + right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) + toDouble(right));
            }
            if (left.isString() && right.isString()) {
                std::string result = m_ctx.pool().lookup(left.asString());
                result += m_ctx.pool().lookup(right.asString());
                return ConstantValue(m_ctx.pool().intern(result));
            }
            return error(node, "invalid operands for '+'");

        case BinaryOp::Sub:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() - right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) - toDouble(right));
            }
            return error(node, "invalid operands for '-'");

        case BinaryOp::Mul:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() * right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) * toDouble(right));
            }
            return error(node, "invalid operands for '*'");

        case BinaryOp::Div:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return error(node, "division by zero");
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            if (bothNumeric(left, right)) {
                double divisor = toDouble(right);
                if (divisor == 0.0) {
                    return error(node, "division by zero");
                }
                return ConstantValue(toDouble(left) / divisor);
            }
            return error(node, "invalid operands for '/'");

        case BinaryOp::Mod:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return error(node, "modulo by zero");
                }
                return ConstantValue(left.asInt() % right.asInt());
            }
            return error(node, "modulo requires integer operands");

        case BinaryOp::Pow:
            if (bothNumeric(left, right)) {
                double result = std::pow(toDouble(left), toDouble(right));
                if (left.isInt() && right.isInt() && result == std::floor(result)) {
                    return ConstantValue(static_cast<int64_t>(result));
                }
                return ConstantValue(result);
            }
            return error(node, "invalid operands for '**'");

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
            return error(node, "'and' requires bool operands");

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            return error(node, "'or' requires bool operands");

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() & right.asInt());
            }
            return error(node, "bitwise AND requires integer operands");

        case BinaryOp::BitOr:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() | right.asInt());
            }
            return error(node, "bitwise OR requires integer operands");

        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() ^ right.asInt());
            }
            return error(node, "bitwise XOR requires integer operands");

        case BinaryOp::Shl:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    return error(node, "negative shift amount");
                }
                return ConstantValue(left.asInt() << right.asInt());
            }
            return error(node, "shift requires integer operands");

        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    return error(node, "negative shift amount");
                }
                return ConstantValue(left.asInt() >> right.asInt());
            }
            return error(node, "shift requires integer operands");

        default:
            return error(node, "unsupported binary operator in const expression");
    }
}

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeFunction(
    const FuncDeclAST* func,
    const std::vector<ConstantValue>& args) {

    if (!func) {
        return error(nullptr, "null function");
    }

    // Check parameter count
    size_t paramCount = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        paramCount += group->params.size();
    }

    if (args.size() != paramCount) {
        return error(func, "argument count mismatch: expected "
                   + std::to_string(paramCount) + ", got "
                   + std::to_string(args.size()));
    }

    // ─── Use existing context system ──────────────────────────────────────
    ConstFunctionContext context(m_ctx, func);

    // Bind parameters to SymbolStorage
    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                setLocalValue(param->name, args[argIndex]);
                argIndex++;
            }
        }
    }

    // Execute the body
    if (func->body) {
        ConstantValue result = executeStmt(func->body);
        
        // Check for return using existing ReturnRequirements
        if (m_ctx.contexts.currentReturnReqs()) {
            if (m_ctx.contexts.returnRequirementsSatisfied()) {
                // Return value is tracked via ReturnRequirements
            }
        }
        
        return result;
    }

    return error(func, "const function has no body");
}

ConstantValue ConstEvaluator::executeIf(const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // ─── Use existing ScopedIfCondition for narrowing ────────────────────
    ConstIfContext ifContext(m_ctx, stmt->elseBranch != nullptr);

    // Evaluate condition
    ConstantValue cond = evalExpr(stmt->condition);
    if (cond.isError()) return cond;

    if (!cond.isBool()) {
        return error(stmt->condition, "if condition must be bool");
    }

    // ─── Get narrowing info from condition ──────────────────────────────
    NarrowingInfo info = m_ctx.contexts.getPendingNarrowing();
    m_ctx.contexts.clearPendingNarrowing();

    // Execute the appropriate branch with narrowing
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

// ─── Local Variable Helpers ─────────────────────────────────────────────

ConstantValue ConstEvaluator::getLocalValue(InternedString name) const {
    // Walk scopes from innermost to outermost
    for (auto it = m_ctx.symbols.scopes().rbegin(); 
         it != m_ctx.symbols.scopes().rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            // Look for the value on the VarDeclAST's initializer expression
            const VarDeclAST* var = found->second->as<VarDeclAST>();
            if (var && var->init) {
                // The value was stored on the initializer expression during evaluation
                // Find the matching identifier in the initializer's evaluated expressions
                // For now, check if the initializer expression has been evaluated
                // This is a simplification - we need a better way to track values
                return ConstantValue::unknown();
            }
            return ConstantValue::error();
        }
    }
    return ConstantValue::unknown();
}

void ConstEvaluator::setLocalValue(InternedString name, const ConstantValue& value) {
    // Find the variable in the current scope and store the value on its initializer
    for (auto it = m_ctx.symbols.scopes().rbegin(); 
         it != m_ctx.symbols.scopes().rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            VarDeclAST* var = const_cast<VarDeclAST*>(
                found->second->as<VarDeclAST>()
            );
            if (var && var->init) {
                // Store the value on the initializer expression
                const_cast<ExprAST*>(var->init)->isConst = true;
                const_cast<ExprAST*>(var->init)->constValue = value;
            }
            return;
        }
    }
}

bool ConstEvaluator::isLocalVariable(InternedString name) const {
    for (auto it = m_ctx.symbols.scopes().rbegin(); 
         it != m_ctx.symbols.scopes().rend(); ++it) {
        if (it->values.find(name) != it->values.end()) {
            return true;
        }
    }
    return false;
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

TypeAST* ConstEvaluator::getConstantType(const ConstantValue& val) {
    if (val.type) return val.type;

    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Bool);
        case ConstantValue::Kind::Int:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Int);
        case ConstantValue::Kind::Float:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Float);
        case ConstantValue::Kind::String:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::String);
        case ConstantValue::Kind::Char:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Char);
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
            return m_ctx.pool().lookup(a.asString()).compare(
                   m_ctx.pool().lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Error Reporting ─────────────────────────────────────────────────────

ConstantValue ConstEvaluator::error(const BaseAST* node, const std::string& msg) {
    m_ctx.error(node, DiagCode::E3003, "const evaluation failed: ", msg);
    return ConstantValue::error();
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
            const ValueDeclAST* decl = m_ctx.symbols.lookupValue(id->name);
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

// ─── Stub implementations for remaining methods ─────────────────────────

ConstantValue ConstEvaluator::evalLiteral(const LiteralExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalUnary(const UnaryExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalCall(const CallExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalStructLiteral(const StructLiteralExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalArrayLiteral(const ArrayLiteralExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalFieldAccess(const FieldAccessExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalNullCoalesce(const NullCoalesceExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalIfExpr(const IfExprAST* expr) {
    // ... existing implementation ...
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::executeStmt(const StmtAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeBlock(const BlockStmtAST* block) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeReturn(const ReturnStmtAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeWhile(const WhileStmtAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeAssign(const AssignExprAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeExprStmt(const ExprStmtAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(const DeclStmtAST* stmt) {
    // ... existing implementation ...
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeBreak() {
    return error(nullptr, "'break' not supported in const functions");
}

ConstantValue ConstEvaluator::executeContinue() {
    return error(nullptr, "'continue' not supported in const functions");
}

} // namespace sema