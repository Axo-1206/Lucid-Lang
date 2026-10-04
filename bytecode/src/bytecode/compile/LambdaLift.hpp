/**
 * @file compile/LambdaLift.hpp
 *
 * @responsibility Synthesize a top-level FnDeclAST for every lambda
 *                 Sema collected. Register each with the compiler's
 *                 function index. Record the lambda → function map
 *                 and the function → body-expression map.
 *
 * ─── Why this is small ────────────────────────────────────────────────────
 * Sema walks every expression, statement, and declaration already, and
 * it collects every LambdaExprAST into the enclosing ModuleAST's
 * `lambdas` span during that walk. The lift therefore does not re-walk
 * the AST: it iterates each module's span and synthesizes one function
 * per lambda.
 *
 * ─── Nested lambdas ───────────────────────────────────────────────────────
 * Sema collects nested lambdas too — walking into a lambda's body
 * appends any inner lambda to the same span. So the lift sees a flat
 * list of every lambda in the module, including ones nested inside
 * other lambdas' bodies. No recursive lifting is needed here.
 *
 * ─── AST node ownership ───────────────────────────────────────────────────
 * The synthesized FnDeclAST nodes live in the lift's own deque, not in
 * Sema's AST arena. Deque (not vector) so the FnDeclAST* keys in the
 * maps stay stable across insertions. The lifetime is the Compiler's:
 * when the Compiler is destroyed, the deque is destroyed, and the
 * synthesized functions go with it.
 *
 * ─── No body block ────────────────────────────────────────────────────────
 * A synthesized function's `body` is null. Its body is the lambda's
 * body expression, held in a side map (bodyOf). Pass B special-cases
 * a synthesized function: instead of emitting a block, it emits the
 * expression as the return value.
 */

#pragma once

#include "bytecode/compile/Compiler.hpp"

#include "core/ast/BaseAST.hpp"     // for ModuleAST
#include "core/ast/ExprAST.hpp"     // for LambdaExprAST
#include "core/ast/DeclAST.hpp"     // for FnDeclAST
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode::compile {

class LambdaLift {
public:
    LambdaLift(Compiler& compiler, StringPool& pool);

    /// Synthesize a top-level function for every lambda in every
    /// module's `lambdas` span, and register each with the compiler.
    ///
    /// Called once, between pass A and pass B, after all module-level
    /// FnDeclASTs are registered.
    void lift(const std::vector<ModuleAST*>& modules);

    /// The synthesized FnDeclAST for a lambda, or nullptr if the
    /// lambda was not lifted.
    const FnDeclAST* functionFor(const LambdaExprAST* lambda) const;

    /// The lambda body expression for a synthesized function, or
    /// nullptr if the FnDeclAST is not a synthesized lambda.
    ExprAST* bodyOf(const FnDeclAST* fn) const;

    /// The ModuleAST a synthesized function belongs to. Used by pass B
    /// to build the per-function CompilerContext (it needs a module
    /// for line-table file paths).
    ModuleAST* moduleOf(const FnDeclAST* fn) const;

    /// True if the FnDeclAST is a synthesized lambda function.
    bool isSynthesized(const FnDeclAST* fn) const;

    /// The list of synthesized functions, in creation order. Pass B
    /// iterates this list after the module walk.
    const std::vector<FnDeclAST*>& synthesizedFunctions() const noexcept {
        return m_synthesized;
    }

private:
    Compiler&   m_compiler;
    StringPool& m_pool;

    /// Stable storage for the synthesized FnDeclAST nodes.
    std::deque<FnDeclAST> m_functions;

    /// LambdaExprAST* → synthesized FnDeclAST*.
    std::unordered_map<const LambdaExprAST*, FnDeclAST*> m_byLambda;

    /// FnDeclAST* (synthesized) → body expression.
    std::unordered_map<const FnDeclAST*, ExprAST*> m_bodies;

    /// FnDeclAST* (synthesized) → owning ModuleAST*.
    std::unordered_map<const FnDeclAST*, ModuleAST*> m_moduleOf;

    /// The synthesized functions, in creation order.
    std::vector<FnDeclAST*> m_synthesized;

    /// Per-compilation counter for synthesized function names.
    uint32_t m_nextIndex = 0;
};

} // namespace lucid::bytecode::compile