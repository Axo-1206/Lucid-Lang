/// @file const_eval/ConstInterpreter.cpp
/// @brief Implementation of ConstInterpreter.

#include "ConstInterpreter.hpp"
#include "ConstEvaluator.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace sema {

// ─── Constructor ──────────────────────────────────────────────────────────

ConstInterpreter::ConstInterpreter(ConstEvaluator& evaluator, SemaContext& ctx)
    : m_evaluator(evaluator), m_ctx(ctx) {}

// ─── Function Execution ──────────────────────────────────────────────────

ConstantValue ConstInterpreter::executeFunction(
    const FuncDeclAST* func,
    const std::vector<ConstantValue>& args) {

    if (!func) {
        reportError(nullptr, "null function");
        return ConstantValue::error();
    }

    // Check parameter count
    size_t paramCount = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        paramCount += group->params.size();
    }

    if (args.size() != paramCount) {
        reportError(func, "argument count mismatch: expected "
                   + std::to_string(paramCount) + ", got "
                   + std::to_string(args.size()));
        return ConstantValue::error();
    }

    // Push a new frame (clear locals)
    m_locals.clear();
    m_hasReturned = false;
    m_returnValue = ConstantValue::unknown();

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

    // Execute the body
    if (func->body) {
        ConstantValue result = executeStmt(func->body);
        if (result.isError()) return result;

        // If the body returned a value, use it
        if (m_hasReturned) {
            return m_returnValue;
        }

        // Void function
        if (func->funcType && !func->funcType->returnType) {
            return ConstantValue::voidValue();
        }

        // Non-void function without return
        reportError(func->body, "non-void const function does not return a value");
        return ConstantValue::error();
    }

    reportError(func, "const function has no body");
    return ConstantValue::error();
}

ConstantValue ConstInterpreter::executeBody(
    const StmtAST* body,
    const std::vector<ParamAST*>& params,
    const std::vector<ConstantValue>& args) {

    // Clear locals
    m_locals.clear();
    m_hasReturned = false;
    m_returnValue = ConstantValue::unknown();

    // Bind parameters
    for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
        setLocal(params[i]->name, args[i]);
    }

    // Execute body
    if (body) {
        return executeStmt(body);
    }

    return ConstantValue::voidValue();
}

// ─── Statement Execution ─────────────────────────────────────────────────

ConstantValue ConstInterpreter::executeStmt(const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Check if we've already returned
    if (m_hasReturned) return m_returnValue;

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

        case ASTKind::BreakStmt:
            return executeBreak();

        case ASTKind::ContinueStmt:
            return executeContinue();

        default:
            // Other statement types are not supported in const functions
            reportError(stmt, "unsupported statement in const function");
            return ConstantValue::error();
    }
}

ConstantValue ConstInterpreter::executeBlock(const BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    // Save current locals (for nested blocks)
    auto savedLocals = m_locals;

    ConstantValue result = ConstantValue::voidValue();

    for (const StmtPtr stmt : block->stmts) {
        result = executeStmt(stmt);
        if (result.isError()) break;
        if (m_hasReturned) break;
    }

    // Restore locals (block scope)
    m_locals = savedLocals;

    return result;
}

ConstantValue ConstInterpreter::executeReturn(const ReturnStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    if (stmt->value) {
        m_returnValue = evalExpr(stmt->value);
        if (m_returnValue.isError()) return m_returnValue;
    } else {
        m_returnValue = ConstantValue::voidValue();
    }

    m_hasReturned = true;
    return m_returnValue;
}

ConstantValue ConstInterpreter::executeIf(const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Evaluate condition
    ConstantValue cond = evalExpr(stmt->condition);
    if (cond.isError()) return ConstantValue::error();

    if (!cond.isBool()) {
        reportError(stmt->condition, "if condition must be bool");
        return ConstantValue::error();
    }

    // Execute the appropriate branch
    if (cond.asBool()) {
        if (stmt->thenBranch) {
            return executeStmt(stmt->thenBranch);
        }
    } else {
        if (stmt->elseBranch) {
            return executeStmt(stmt->elseBranch);
        }
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstInterpreter::executeWhile(const WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // For const functions, while loops are allowed but must be bounded
    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        // Check iteration limit
        if (++iterations > MAX_ITERATIONS) {
            reportError(stmt, "while loop exceeded maximum iterations ("
                       + std::to_string(MAX_ITERATIONS) + ")");
            return ConstantValue::error();
        }

        // Evaluate condition
        ConstantValue cond = evalExpr(stmt->condition);
        if (cond.isError()) return ConstantValue::error();

        if (!cond.isBool()) {
            reportError(stmt->condition, "while condition must be bool");
            return ConstantValue::error();
        }

        if (!cond.asBool()) break;

        // Execute body
        ConstantValue result = executeStmt(stmt->body);
        if (result.isError()) return result;
        if (m_hasReturned) return m_returnValue;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstInterpreter::executeAssign(const AssignExprAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Evaluate RHS
    ConstantValue rhs = evalExpr(stmt->rhs);
    if (rhs.isError()) return ConstantValue::error();

    // Handle assignment to local variable
    if (stmt->lhs->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = stmt->lhs->as<IdentifierExprAST>();
        setLocal(id->name, rhs);
        return rhs;
    }

    // TODO: Handle field assignment
    // const structs are immutable, so field assignment should be rejected

    reportError(stmt->lhs, "assignment target not supported in const function");
    return ConstantValue::error();
}

ConstantValue ConstInterpreter::executeExprStmt(const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    // Evaluate the expression for side effects
    ConstantValue result = evalExpr(stmt->expr);
    if (result.isError()) return ConstantValue::error();

    // Expression statements discard the result
    return ConstantValue::voidValue();
}

ConstantValue ConstInterpreter::executeBreak() {
    // Break is not allowed in const functions (no loops that need breaking)
    // Actually, we could support it but it's complex. For now, reject.
    reportError(nullptr, "'break' not supported in const functions");
    return ConstantValue::error();
}

ConstantValue ConstInterpreter::executeContinue() {
    reportError(nullptr, "'continue' not supported in const functions");
    return ConstantValue::error();
}

ConstantValue ConstInterpreter::executeDeclStmt(const DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    // Handle local const declarations
    if (stmt->decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const) {
            if (var->init) {
                ConstantValue val = evalExpr(var->init);
                if (val.isError()) return ConstantValue::error();
                setLocal(var->name, val);
                return ConstantValue::voidValue();
            }
        }
        // Local let declarations are not allowed in const functions
        reportError(stmt->decl, "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    // Other declarations (structs, enums, traits) are not allowed in const functions
    reportError(stmt->decl, "declaration not supported in const function");
    return ConstantValue::error();
}

// ─── Expression Evaluation ───────────────────────────────────────────────

ConstantValue ConstInterpreter::evalExpr(const ExprAST* expr) {
    if (!expr) return ConstantValue::error();

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return m_evaluator.evalLiteral(expr->as<LiteralExprAST>());

        case ASTKind::IdentifierExpr:
            return evalIdentifier(expr->as<IdentifierExprAST>());

        case ASTKind::BinaryExpr:
            return evalBinary(expr->as<BinaryExprAST>());

        case ASTKind::UnaryExpr:
            return evalUnary(expr->as<UnaryExprAST>());

        case ASTKind::CallExpr:
            return evalCall(expr->as<CallExprAST>());

        default:
            // Use the evaluator for other expression types
            return m_evaluator.evalExpr(expr);
    }
}

ConstantValue ConstInterpreter::evalIdentifier(const IdentifierExprAST* expr) {
    // First, check local variables
    auto it = m_locals.find(expr->name);
    if (it != m_locals.end()) {
        return it->second;
    }

    // Not a local - use the evaluator (will check const declarations)
    return m_evaluator.evalIdentifier(expr);
}

ConstantValue ConstInterpreter::evalBinary(const BinaryExprAST* expr) {
    return m_evaluator.evalBinary(expr);
}

ConstantValue ConstInterpreter::evalUnary(const UnaryExprAST* expr) {
    return m_evaluator.evalUnary(expr);
}

ConstantValue ConstInterpreter::evalCall(const CallExprAST* expr) {
    // Evaluate callee
    ConstantValue callee = evalExpr(expr->callee);
    if (callee.isError()) return ConstantValue::error();

    if (!callee.isFunction()) {
        reportError(expr->callee, "not a function");
        return ConstantValue::error();
    }

    const FuncDeclAST* func = callee.asFunction();

    // Evaluate arguments
    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evalExpr(arg);
        if (val.isError()) return ConstantValue::error();
        args.push_back(val);
    }

    // Recursively execute the called function
    return executeFunction(func, args);
}

// ─── Helpers ─────────────────────────────────────────────────────────────

ConstantValue ConstInterpreter::getLocal(InternedString name) const {
    auto it = m_locals.find(name);
    if (it != m_locals.end()) {
        return it->second;
    }
    return ConstantValue::unknown();
}

void ConstInterpreter::setLocal(InternedString name, const ConstantValue& value) {
    m_locals[name] = value;
}

bool ConstInterpreter::blockAlwaysReturns(const BlockStmtAST* block) const {
    // TODO: Implement
    return false;
}

bool ConstInterpreter::isPureStmt(const StmtAST* stmt) const {
    // A statement is pure if it has no side effects
    // I/O, assignments, etc. are impure
    // For now, we assume all statements are pure except assignment and I/O
    switch (stmt->kind) {
        case ASTKind::AssignExpr:
            return false;
        default:
            return true;
    }
}

void ConstInterpreter::reportError(const BaseAST* node, const std::string& msg) {
    m_ctx.error(node, DiagCode::E3003, "const function evaluation failed: ", msg);
}

} // namespace sema