/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Integrated with SemaContext
///   Uses ContextStack for function/if/loop contexts, integrated symbol storage
///   for local variables, and NarrowingStack for type narrowing. No custom
///   frame system - leverages the existing semantic analysis infrastructure.
///
/// @design_decision Called during type resolution (Phase 2)
///   Const evaluation happens when resolving declarations, not as a
///   separate phase. This allows the evaluator to use all the context
///   information already available.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../support/TypeNarrowHelpers.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaCompare.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <stack>

namespace sema {

/// @brief RAII guard for const evaluation recursion tracking.
class EvaluationGuard {
public:
    EvaluationGuard(std::unordered_set<const DeclAST*>& evaluating,
                    const DeclAST* decl)
        : m_evaluating(evaluating), m_decl(decl) {
        m_evaluating.insert(decl);
    }
    
    ~EvaluationGuard() {
        m_evaluating.erase(m_decl);
    }
    
    EvaluationGuard(const EvaluationGuard&) = delete;
    EvaluationGuard& operator=(const EvaluationGuard&) = delete;

private:
    std::unordered_set<const DeclAST*>& m_evaluating;
    const DeclAST* m_decl;
};

/// @brief RAII guard for const evaluation function context.
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, const FuncDeclAST* func)
        : m_ctx(ctx) {
        m_ctx.contexts.pushFunction(
            const_cast<FuncDeclAST*>(func),
            const_cast<FuncTypeAST*>(func->funcType),
            func->loc
        );
        m_ctx.pushScope();
    }
    
    ~ConstFunctionContext() {
        m_ctx.popScope();
        m_ctx.contexts.pop();
    }

private:
    SemaContext& m_ctx;
};

/// @brief RAII guard for const evaluation if context.
class ConstIfContext {
public:
    ConstIfContext(SemaContext& ctx, bool hasElse)
        : m_guard(ctx, hasElse) {}
    
private:
    ScopedIfCondition m_guard;
};

/// @brief RAII guard for const evaluation narrowing.
class ConstNarrowing {
public:
    ConstNarrowing(SemaContext& ctx, InternedString name, 
                   const TypeAST* narrowedType, bool isInverse = false)
        : m_guard(ctx, name, narrowedType, isInverse) {}
    
private:
    ScopedNarrowing m_guard;
};

/// @brief Frame for local variable values during const evaluation.
struct ConstFrame {
    std::unordered_map<InternedString, ConstantValue> locals;
    bool hasReturned = false;
    ConstantValue returnValue;
};

/// @brief Evaluates const declarations at compile-time.
class ConstEvaluator {
public:
    explicit ConstEvaluator(SemaContext& ctx);

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    /// Called from resolveVarDecl during type resolution.
    ConstantValue evaluateDecl(const VarDeclAST* decl);

    /// @brief Evaluate an expression in the current context.
    ConstantValue evalExpr(const ExprAST* expr);

    // ─── Expression Evaluators ──────────────────────────────────────────

    ConstantValue evalLiteral(const LiteralExprAST* expr);
    ConstantValue evalIdentifier(const IdentifierExprAST* expr);
    ConstantValue evalBinary(const BinaryExprAST* expr);
    ConstantValue evalUnary(const UnaryExprAST* expr);
    ConstantValue evalCall(const CallExprAST* expr);
    ConstantValue evalStructLiteral(const StructLiteralExprAST* expr);
    ConstantValue evalArrayLiteral(const ArrayLiteralExprAST* expr);
    ConstantValue evalFieldAccess(const FieldAccessExprAST* expr);
    ConstantValue evalNullCoalesce(const NullCoalesceExprAST* expr);
    ConstantValue evalIfExpr(const IfExprAST* expr);

    // ─── Statement Execution ─────────────────────────────────────────────

    ConstantValue executeStmt(const StmtAST* stmt);
    ConstantValue executeBlock(const BlockStmtAST* block);
    ConstantValue executeReturn(const ReturnStmtAST* stmt);
    ConstantValue executeIf(const IfStmtAST* stmt);
    ConstantValue executeWhile(const WhileStmtAST* stmt);
    ConstantValue executeAssign(const AssignExprAST* stmt);
    ConstantValue executeExprStmt(const ExprStmtAST* stmt);
    ConstantValue executeDeclStmt(const DeclStmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    ConstantValue executeFunction(const FuncDeclAST* func,
                                   const std::vector<ConstantValue>& args);

private:
    // ─── Members ──────────────────────────────────────────────────────────

    SemaContext& m_ctx;
    
    /// Stack of frames for function execution
    std::vector<ConstFrame> m_frames;
    
    /// Track which expressions have been evaluated
    std::unordered_set<const ExprAST*> m_evaluatedExprs;
    
    /// Dependency graph for const declarations
    std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;
    
    /// Currently evaluating (for cycle detection)
    std::unordered_set<const DeclAST*> m_evaluating;
    
    /// All const declarations in order
    std::vector<const DeclAST*> m_constDecls;
    
    static constexpr size_t MAX_RECURSION = 1000;
    size_t m_recursionDepth = 0;

    // ─── Frame Management ────────────────────────────────────────────────

    ConstFrame& currentFrame() { return m_frames.back(); }
    const ConstFrame& currentFrame() const { return m_frames.back(); }
    
    void pushFrame() { m_frames.emplace_back(); }
    void popFrame() { m_frames.pop_back(); }
    
    ConstantValue getLocal(InternedString name) const;
    void setLocal(InternedString name, const ConstantValue& value);

    // ─── Binary Operation Helpers ───────────────────────────────────────

    ConstantValue evalBinaryOp(BinaryOp op,
                                const ConstantValue& left,
                                const ConstantValue& right,
                                const BaseAST* node);

    // ─── Type Helpers ────────────────────────────────────────────────────

    TypeAST* getConstantType(const ConstantValue& val);
    bool compareEqual(const ConstantValue& a, const ConstantValue& b);
    int compareOrder(const ConstantValue& a, const ConstantValue& b);

    // ─── Dependency Analysis ─────────────────────────────────────────────

    void buildDependencyGraph();
    void collectDeps(const ExprAST* expr, std::vector<const DeclAST*>& deps);
    void collectDepsFromStmt(const StmtAST* stmt, std::vector<const DeclAST*>& deps);
    std::vector<const DeclAST*> topologicalSort();

    // ─── Error Reporting ─────────────────────────────────────────────────

    ConstantValue error(const BaseAST* node, const std::string& msg);
    void reportCycle(const std::vector<const DeclAST*>& cycle);
};

} // namespace sema