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
        return error(decl, "const evaluation recursion limit exceeded");
    }

    // Check for cycles (detected during topological sort)
    if (m_evaluating.find(decl) != m_evaluating.end()) {
        return error(decl, "circular dependency detected in const evaluation");
    }

    // RAII guard for recursion tracking
    EvaluationGuard guard(m_evaluating, decl);
    m_recursionDepth++;

    ConstantValue result;

    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->init) {
            result = evalExpr(var->init);
            if (result.isError()) {
                return error(var->init, "failed to evaluate const variable '" 
                           + m_ctx.pool().lookup(var->name) + "'");
            }
            result.type = var->type;
        } else {
            return error(decl, "const variable '" + m_ctx.pool().lookup(var->name)
                       + "' has no initializer");
        }
    } else if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* func = decl->as<FuncDeclAST>();
        result = ConstantValue(func);
        result.type = const_cast<FuncDeclAST*>(func)->funcType;
    }

    m_recursionDepth--;
    return result;
}

bool ConstEvaluator::isEvaluated(const ExprAST* expr) const {
    return m_evaluatedExprs.find(expr) != m_evaluatedExprs.end();
}

ConstantValue ConstEvaluator::getValue(const ExprAST* expr) const {
    if (isEvaluated(expr)) {
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
            return error(expr, "not a constant expression");
    }

    // Store result on the expression if successful
    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(result);
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
                return error(expr, "invalid integer literal '" + str + "'");
            }
        }
        case LiteralKind::Float: {
            std::string str = m_ctx.pool().lookup(expr->value);
            try {
                double val = std::stod(str);
                return ConstantValue(val);
            } catch (const std::exception&) {
                return error(expr, "invalid float literal '" + str + "'");
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
            return error(expr, "unsupported literal in const expression");
    }
}

ConstantValue ConstEvaluator::evalIdentifier(const IdentifierExprAST* expr) {
    // Look up the declaration
    const ValueDeclAST* decl = lookupValue(expr->name, m_ctx);
    if (!decl) {
        return error(expr, "undefined identifier '" + m_ctx.pool().lookup(expr->name) + "'");
    }

    // Check if it's a const declaration
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->keyword != DeclKeyword::Const) {
            return error(expr, "'" + m_ctx.pool().lookup(expr->name)
                       + "' is not const (declared as 'let')");
        }

        if (!var->init) {
            return error(expr, "const variable '" + m_ctx.pool().lookup(expr->name)
                       + "' has no initializer");
        }

        // Check for cycle
        if (m_evaluating.find(var) != m_evaluating.end()) {
            return error(expr, "cycle detected: '" + m_ctx.pool().lookup(expr->name) + "'");
        }

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

// ─── Binary Expression Evaluation ──────────────────────────────────────

// Helper: check if two values are numeric
static bool bothNumeric(const ConstantValue& a, const ConstantValue& b) {
    return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
}

// Helper: get numeric value as double
static double toDouble(const ConstantValue& v) {
    if (v.isInt()) return static_cast<double>(v.asInt());
    if (v.isFloat()) return v.asFloat();
    return 0.0;
}

ConstantValue ConstEvaluator::evalBinary(const BinaryExprAST* expr) {
    ConstantValue left = evalExpr(expr->left);
    if (left.isError()) return left;

    ConstantValue right = evalExpr(expr->right);
    if (right.isError()) return right;

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
                std::string result = m_ctx.pool().lookup(left.asString());
                result += m_ctx.pool().lookup(right.asString());
                return ConstantValue(m_ctx.pool().intern(result));
            }
            return error(expr, "invalid operands for '+'");

        case BinaryOp::Sub:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() - right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) - toDouble(right));
            }
            return error(expr, "invalid operands for '-'");

        case BinaryOp::Mul:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() * right.asInt());
            }
            if (bothNumeric(left, right)) {
                return ConstantValue(toDouble(left) * toDouble(right));
            }
            return error(expr, "invalid operands for '*'");

        case BinaryOp::Div:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return error(expr->right, "division by zero");
                }
                return ConstantValue(left.asInt() / right.asInt());
            }
            if (bothNumeric(left, right)) {
                double divisor = toDouble(right);
                if (divisor == 0.0) {
                    return error(expr->right, "division by zero");
                }
                return ConstantValue(toDouble(left) / divisor);
            }
            return error(expr, "invalid operands for '/'");

        case BinaryOp::Mod:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() == 0) {
                    return error(expr->right, "modulo by zero");
                }
                return ConstantValue(left.asInt() % right.asInt());
            }
            return error(expr, "modulo requires integer operands");

        case BinaryOp::Pow:
            if (bothNumeric(left, right)) {
                double result = std::pow(toDouble(left), toDouble(right));
                if (left.isInt() && right.isInt() && result == std::floor(result)) {
                    return ConstantValue(static_cast<int64_t>(result));
                }
                return ConstantValue(result);
            }
            return error(expr, "invalid operands for '**'");

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
            return error(expr, "'and' requires bool operands");

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            return error(expr, "'or' requires bool operands");

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() & right.asInt());
            }
            return error(expr, "bitwise AND requires integer operands");

        case BinaryOp::BitOr:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() | right.asInt());
            }
            return error(expr, "bitwise OR requires integer operands");

        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(left.asInt() ^ right.asInt());
            }
            return error(expr, "bitwise XOR requires integer operands");

        case BinaryOp::Shl:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    return error(expr->right, "negative shift amount");
                }
                return ConstantValue(left.asInt() << right.asInt());
            }
            return error(expr, "shift requires integer operands");

        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    return error(expr->right, "negative shift amount");
                }
                return ConstantValue(left.asInt() >> right.asInt());
            }
            return error(expr, "shift requires integer operands");

        default:
            return error(expr, "unsupported binary operator in const expression");
    }
}

ConstantValue ConstEvaluator::evalUnary(const UnaryExprAST* expr) {
    ConstantValue operand = evalExpr(expr->operand);
    if (operand.isError()) return operand;

    switch (expr->op) {
        case UnaryOp::Neg:
            if (operand.isInt()) {
                return ConstantValue(-operand.asInt());
            }
            if (operand.isFloat()) {
                return ConstantValue(-operand.asFloat());
            }
            return error(expr, "negation requires numeric operand");

        case UnaryOp::Not:
            if (operand.isBool()) {
                return ConstantValue(!operand.asBool());
            }
            return error(expr, "'not' requires bool operand");

        case UnaryOp::BitNot:
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            return error(expr, "bitwise NOT requires integer operand");

        default:
            return error(expr, "unsupported unary operator in const expression");
    }
}

ConstantValue ConstEvaluator::evalCall(const CallExprAST* expr) {
    // Evaluate the callee
    ConstantValue callee = evalExpr(expr->callee);
    if (callee.isError()) return callee;

    // Must be a const function
    if (!callee.isFunction()) {
        return error(expr->callee, "not a const function");
    }

    const FuncDeclAST* func = callee.asFunction();
    if (func->keyword != DeclKeyword::Const) {
        return error(expr->callee, "function is not const (declared as 'let')");
    }

    // Evaluate arguments
    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evalExpr(arg);
        if (val.isError()) return val;
        args.push_back(val);
    }

    // Execute the const function (now inlined)
    return executeFunction(func, args);
}

ConstantValue ConstEvaluator::evalStructLiteral(const StructLiteralExprAST* expr) {
    // Get the struct declaration
    const TypeDeclAST* typeDecl = lookupType(expr->typeName, m_ctx);
    if (!typeDecl) {
        return error(expr, "undefined struct type '" + m_ctx.pool().lookup(expr->typeName) + "'");
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        return error(expr, "'" + m_ctx.pool().lookup(expr->typeName) + "' is not a struct");
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Build struct value
    std::unordered_map<InternedString, ConstantValue> fields;

    // First, collect default values from struct fields
    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->defaultVal) {
            ConstantValue val = evalExpr(field->defaultVal);
            if (val.isError()) return val;
            fields[field->name] = val;
        }
    }

    // Then override with explicit initializers
    for (const FieldInitAST* init : expr->inits) {
        ConstantValue val = evalExpr(init->value);
        if (val.isError()) return val;
        fields[init->name] = val;
    }

    // Verify all required fields are initialized
    for (const FieldDeclAST* field : structDecl->fields) {
        if (fields.find(field->name) == fields.end() && !field->defaultVal) {
            return error(expr, "missing initializer for struct field '" 
                       + m_ctx.pool().lookup(field->name) + "'");
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Struct;
    result.value = fields;
    
    // ─── FIX: Create a NamedTypeAST for the struct type ──────────────────
    // The type should be a NamedTypeAST that refers to the struct,
    // not the StructDeclAST itself. This matches how types are represented
    // elsewhere in the AST.
    NamedTypeAST* structType = m_ctx.arena().make<NamedTypeAST>(expr->typeName);
    
    // Copy generic arguments if present
    if (!expr->genericArgs.empty()) {
        // We need to copy the generic args to the new NamedTypeAST
        // This requires creating a new span for the generic arguments
        auto builder = m_ctx.arena().makeBuilder<TypePtr>();
        for (const TypePtr arg : expr->genericArgs) {
            builder.push_back(arg); // The args are already resolved
        }
        structType->genericArgs = builder.build();
    }
    
    result.type = structType;
    return result;
}

ConstantValue ConstEvaluator::evalArrayLiteral(const ArrayLiteralExprAST* expr) {
    std::vector<ConstantValue> elements;

    for (const ExprAST* elem : expr->elements) {
        ConstantValue val = evalExpr(elem);
        if (val.isError()) return val;
        elements.push_back(val);
    }

    // Verify all elements have the same type
    if (!elements.empty()) {
        const TypeAST* firstType = elements[0].type;
        for (size_t i = 1; i < elements.size(); ++i) {
            if (elements[i].type != firstType) {
                return error(expr, "array elements must have the same type");
            }
        }
    }

    ConstantValue result;
    result.kind = ConstantValue::Kind::Array;
    result.value = elements;
    return result;
}

ConstantValue ConstEvaluator::evalFieldAccess(const FieldAccessExprAST* expr) {
    // Evaluate the object
    ConstantValue obj = evalExpr(expr->object);
    if (obj.isError()) return obj;

    if (!obj.isStruct()) {
        return error(expr->object, "field access on non-struct value");
    }

    const auto& structFields = obj.asStruct();
    auto it = structFields.find(expr->fieldName);
    if (it == structFields.end()) {
        return error(expr, "struct has no field '" + m_ctx.pool().lookup(expr->fieldName) + "'");
    }

    return it->second;
}

ConstantValue ConstEvaluator::evalNullCoalesce(const NullCoalesceExprAST* expr) {
    // Evaluate the value
    ConstantValue val = evalExpr(expr->value);
    if (val.isError()) return val;

    // If value is nil or err, evaluate fallback
    if (val.isNil() || val.isErr()) {
        return evalExpr(expr->fallback);
    }

    // Otherwise return the value
    return val;
}

ConstantValue ConstEvaluator::evalIfExpr(const IfExprAST* expr) {
    // Evaluate condition
    ConstantValue cond = evalExpr(expr->condition);
    if (cond.isError()) return cond;

    if (!cond.isBool()) {
        return error(expr->condition, "if condition must be bool");
    }

    // Evaluate the appropriate branch
    if (cond.asBool()) {
        return evalExpr(expr->thenBranch);
    } else {
        return evalExpr(expr->elseBranch);
    }
}

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Check if we've already returned
    if (currentFrame().hasReturned) return currentFrame().returnValue;

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
            return error(stmt, "unsupported statement in const function");
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
    } else {
        frame.returnValue = ConstantValue::voidValue();
    }

    frame.hasReturned = true;
    return frame.returnValue;
}

ConstantValue ConstEvaluator::executeIf(const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Evaluate condition
    ConstantValue cond = evalExpr(stmt->condition);
    if (cond.isError()) return cond;

    if (!cond.isBool()) {
        return error(stmt->condition, "if condition must be bool");
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

ConstantValue ConstEvaluator::executeWhile(const WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        if (++iterations > MAX_ITERATIONS) {
            return error(stmt, "while loop exceeded maximum iterations (" 
                       + std::to_string(MAX_ITERATIONS) + ")");
        }

        ConstantValue cond = evalExpr(stmt->condition);
        if (cond.isError()) return cond;

        if (!cond.isBool()) {
            return error(stmt->condition, "while condition must be bool");
        }

        if (!cond.asBool()) break;

        ConstantValue result = executeStmt(stmt->body);
        if (result.isError()) return result;
        if (currentFrame().hasReturned) return currentFrame().returnValue;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeAssign(const AssignExprAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // Evaluate RHS
    ConstantValue rhs = evalExpr(stmt->rhs);
    if (rhs.isError()) return rhs;

    // Handle assignment to local variable
    if (stmt->lhs->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = stmt->lhs->as<IdentifierExprAST>();
        setLocal(id->name, rhs);
        return rhs;
    }

    return error(stmt->lhs, "assignment target not supported in const function");
}

ConstantValue ConstEvaluator::executeExprStmt(const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evalExpr(stmt->expr);
    if (result.isError()) return result;

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
                setLocal(var->name, val);
                return ConstantValue::voidValue();
            }
        }
        return error(stmt->decl, "mutable local variables not allowed in const functions");
    }

    return error(stmt->decl, "declaration not supported in const function");
}

ConstantValue ConstEvaluator::executeBreak() {
    return error(nullptr, "'break' not supported in const functions");
}

ConstantValue ConstEvaluator::executeContinue() {
    return error(nullptr, "'continue' not supported in const functions");
}

// ─── Function Execution ──────────────────────────────────────────────────

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

    // Push a new frame
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

    ConstantValue result;

    // Execute the body
    if (func->body) {
        result = executeStmt(func->body);

        // If the body returned a value, use it
        if (currentFrame().hasReturned) {
            result = currentFrame().returnValue;
        } else if (func->funcType && !func->funcType->returnType) {
            result = ConstantValue::voidValue();
        } else {
            result = error(func->body, "non-void const function does not return a value");
        }
    } else {
        result = error(func, "const function has no body");
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

        if (decl->isa<StructDeclAST>() || decl->isa<EnumDeclAST>()) {
            return true;
        }
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

    // Function types are evaluable
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

    // Otherwise, infer from kind using your type system
    switch (val.kind) {
        case ConstantValue::Kind::Bool:
            // Use your existing type system to get bool type
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Bool);
        case ConstantValue::Kind::Int:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Int);
        case ConstantValue::Kind::Float:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Float);
        case ConstantValue::Kind::String:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::String);
        case ConstantValue::Kind::Char:
            return m_ctx.arena().make<PrimitiveTypeAST>(PrimitiveKind::Char);
        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err:
            // Sentinels have no type
            return nullptr;
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
        case ConstantValue::Kind::Enum:
            return a.asString() == b.asString();
        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err:
            return true; // All sentinels are equal
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

// ─── Dependency Analysis ─────────────────────────────────────────────────

void ConstEvaluator::buildDependencyGraph() {
    m_deps.clear();
    m_constDecls.clear();

    // Collect all const declarations
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

    // Build dependencies
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
                // Walk the body to collect dependencies
                // For simplicity, we collect from the body recursively
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

    // Build graph and compute in-degrees
    for (const auto& [decl, deps] : m_deps) {
        inDegree[decl] = 0;
        graph[decl] = {};
    }

    for (const auto& [decl, deps] : m_deps) {
        for (const DeclAST* dep : deps) {
            // Skip dependencies not in the graph (non-const declarations)
            if (graph.find(dep) == graph.end()) continue;
            graph[decl].push_back(dep);
            inDegree[dep]++;
        }
    }

    // Kahn's algorithm
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

    // Check for cycles
    if (result.size() != m_deps.size()) {
        // Find cycle
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