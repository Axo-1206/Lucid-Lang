/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Static methods only - no instance state needed.
///   Each const declaration is evaluated independently. The evaluator
///   doesn't need to persist state between calls.
///
/// @design_decision Silent on "not const-evaluable"
///   If an expression can't be evaluated at compile time, we simply return
///   ConstantValue::unknown() without emitting a diagnostic. This is normal
///   and expected - the compiler continues with regular type checking.
///   Only actual errors (division by zero, circular dependencies, etc.)
///   trigger diagnostics.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../support/TypeNarrowHelpers.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        m_ctx.stack.pushFunction(
            const_cast<FuncDeclAST*>(func),
            const_cast<FuncTypeAST*>(func->funcType),
            func->loc
        );
        m_ctx.pushScope();
    }
    
    ~ConstFunctionContext() {
        m_ctx.popScope();
        m_ctx.stack.pop();
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
/// All methods are static - no instance state needed.
class ConstEvaluator {
public:
    static constexpr size_t MAX_RECURSION = 1000;

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    static ConstantValue evaluateDecl(SemaContext& ctx, const VarDeclAST* decl);

    /// @brief Evaluate an expression in the current context.
    static ConstantValue evaluate(SemaContext& ctx, const ExprAST* expr);

    /// @brief Report a circular dependency in const declarations.
    static void reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle);

    /// @brief Build the dependency graph for const declarations.
    static void buildDependencyGraph(SemaContext& ctx);

private:
    // ─── Frame Management ────────────────────────────────────────────────

    static ConstFrame& currentFrame(std::vector<ConstFrame>& frames);
    static const ConstFrame& currentFrame(const std::vector<ConstFrame>& frames);
    static void pushFrame(std::vector<ConstFrame>& frames);
    static void popFrame(std::vector<ConstFrame>& frames);
    
    static ConstantValue getLocal(std::vector<ConstFrame>& frames, InternedString name);
    static void setLocal(std::vector<ConstFrame>& frames, InternedString name, const ConstantValue& value);
    static bool isLocalVariable(const std::vector<ConstFrame>& frames, InternedString name);

    // ─── Expression Evaluators ──────────────────────────────────────────

    static ConstantValue evalLiteral(SemaContext& ctx, const LiteralExprAST* expr);
    static ConstantValue evalIdentifier(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                         const IdentifierExprAST* expr);
    static ConstantValue evalBinary(SemaContext& ctx, const BinaryExprAST* expr);
    static ConstantValue evalUnary(SemaContext& ctx, const UnaryExprAST* expr);
    static ConstantValue evalCall(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                   const CallExprAST* expr);
    static ConstantValue evalStructLiteral(SemaContext& ctx, const StructLiteralExprAST* expr);
    static ConstantValue evalArrayLiteral(SemaContext& ctx, const ArrayLiteralExprAST* expr);
    static ConstantValue evalFieldAccess(SemaContext& ctx, const FieldAccessExprAST* expr);
    static ConstantValue evalNullCoalesce(SemaContext& ctx, const NullCoalesceExprAST* expr);
    static ConstantValue evalIfExpr(SemaContext& ctx, const IfExprAST* expr);

    // ─── Statement Execution ─────────────────────────────────────────────

    static ConstantValue executeStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                      const StmtAST* stmt);
    static ConstantValue executeBlock(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                       const BlockStmtAST* block);
    static ConstantValue executeReturn(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                        const ReturnStmtAST* stmt);
    static ConstantValue executeIf(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                    const IfStmtAST* stmt);
    static ConstantValue executeWhile(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                       const WhileStmtAST* stmt);
    static ConstantValue executeAssign(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                        const AssignExprAST* stmt);
    static ConstantValue executeExprStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const ExprStmtAST* stmt);
    static ConstantValue executeDeclStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const DeclStmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    static ConstantValue executeFunction(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const FuncDeclAST* func,
                                          const std::vector<ConstantValue>& args);

    // ─── Binary Operation Helpers ───────────────────────────────────────

    static ConstantValue evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                       const ConstantValue& left,
                                       const ConstantValue& right,
                                       const BaseAST* node);

    // ─── Type Helpers ────────────────────────────────────────────────────

    static TypeAST* getConstantType(SemaContext& ctx, const ConstantValue& val);
    static bool compareEqual(const ConstantValue& a, const ConstantValue& b);
    static int compareOrder(const ConstantValue& a, const ConstantValue& b, SemaContext& ctx);

    // ─── Dependency Analysis ─────────────────────────────────────────────

    static void collectDeps(SemaContext& ctx, const ExprAST* expr, 
                            std::vector<const DeclAST*>& deps);
    static void collectDepsFromStmt(SemaContext& ctx, const StmtAST* stmt,
                                     std::vector<const DeclAST*>& deps);
    static std::vector<const DeclAST*> topologicalSort(SemaContext& ctx);

    // ─── Internal State ──────────────────────────────────────────────────

    // These are static because they track global const state across evaluations
    static std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;
    static std::vector<const DeclAST*> m_constDecls;
    static std::unordered_set<const ExprAST*> m_evaluatedExprs;
    static std::unordered_set<const DeclAST*> m_evaluating;
    static size_t m_recursionDepth;
};

} // namespace sema