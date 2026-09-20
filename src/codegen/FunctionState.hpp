/// @file codegen/FunctionState.hpp
/// @brief Per-function state, RAII-scoped around each function body.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The state that exists while lowering one function body and is restored
/// when the body finishes. Three things live here:
///
///   1. The current `llvm::Function*` being lowered. Lookups of parameters
///      and returns go through it.
///
///   2. The scope stack. Each `{ ... }` block pushes a `Scope`; the block's
///      exit pops it. The scope holds which bindings are alive and which
///      have been consumed, so cleanup can be emitted at scope exit.
///
///   3. The loop stack. Each `for`/`while`/`do-while` pushes a `LoopInfo`
///      with the continue and exit blocks; `break`/`continue` read the
///      back.
///
/// Plus a few scalar fields (`currentDeclaredReturnType`, `currentEnvPtr`)
/// that describe what the current function is doing.
///
/// ─── RAII Semantics ───────────────────────────────────────────────────────
/// Constructing a `FunctionState` captures the previous state and installs
/// the new one. Destroying it restores the previous state. A nested closure
/// body constructs a nested `FunctionState`, and the enclosing function's
/// state is restored when the closure body finishes. This replaces the
/// `ClosureBodyScope` hack in the current codebase: nested function bodies
/// are not a special case, they're just RAII.
///
/// What's captured and restored:
///   - the current `llvm::Function*`
///   - the builder's insertion point (via `InsertPointGuard`)
///   - the declared return type
///   - the environment pointer
///   - the scope stack (moved into the new state, restored on destruction)
///   - the loop stack (ditto)
///
/// What's NOT captured:
///   - the value lookup map (`ValueDeclAST* → llvm::Value*`). This is per-
///     function but starts empty for each function. A nested closure body
///     rebinds the captured declarations to its own env-loaded values as
///     part of its own setup; the rebinding is captured and restored by
///     an explicit save/restore (see `savedBindings` below), not by the
///     scope-stack move.
///
/// ─── Why Not on ProgramState? ─────────────────────────────────────────────
/// Function state is per-function, and a program has many functions. Putting
/// it on `ProgramState` would mean manually saving and restoring it around
/// every function body. RAII makes the discipline automatic and impossible
/// to forget.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Scope — one lexical scope's live-variable tracker
// ─────────────────────────────────────────────────────────────────────────────
//
// A scope corresponds to one `{ ... }` block, or the parameter list of a
// function body. It tracks:
//
//   - `alive`: every `ValueDeclAST*` declared in this scope whose value is
//     currently owned by this frame and must be released at scope exit.
//
//   - `consumed`: every `ValueDeclAST*` that was moved out of this scope
//     (by a return, a closure capture, or an explicit move) and whose
//     release has been claimed elsewhere.
//
//   - `block`: the `BlockStmtAST*` this scope corresponds to, if any. Null
//     for synthetic scopes (the function-parameters scope pushed before the
//     body's block). Read at cleanup time to find `#scope_exit`
//     registrations, which Sema attached to the block.
//
// ─── Why `consumed` Is Separate From `alive` ──────────────────────────────
// A binding can be moved out of a scope on one control-flow path and still
// be live on another. `alive` is the source of truth for "does cleanup
// release this at scope exit"; `consumed` records that some path took
// ownership. Cleanup iterates `alive` and skips anything in `consumed`,
// which is why the two sets are disjoint by construction: every
// `markConsumed` removes from `alive` and adds to `consumed`.

struct Scope {
    std::unordered_set<ValueDeclAST*> alive;
    std::unordered_set<ValueDeclAST*> consumed;
    BlockStmtAST* block = nullptr;

    void markAlive(ValueDeclAST* decl) {
        if (!decl) return;
        // Defensive: if a binding was already consumed, adding it to `alive`
        // would produce a scope that releases it *and* records it as moved.
        // That's an emitter bug, and this assertion catches it.
        assert(consumed.find(decl) == consumed.end()
               && "markAlive() called for a binding already consumed in "
                  "this scope");
        alive.insert(decl);
    }

    void markConsumed(ValueDeclAST* decl) {
        if (!decl) return;
        alive.erase(decl);
        consumed.insert(decl);
    }

    bool isAlive(ValueDeclAST* decl) const {
        return alive.find(decl) != alive.end();
    }

    bool isConsumed(ValueDeclAST* decl) const {
        return consumed.find(decl) != consumed.end();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LoopInfo — one loop's break/continue targets
// ─────────────────────────────────────────────────────────────────────────────
//
// Pushed when lowering `for`/`while`/`do-while`. The `continueTarget` is the
// block that `continue` jumps to; the `exit` block is where `break` jumps
// to. `scopeDepth` is the scope-stack depth at the loop's entry, which is
// what `break`/`continue` unwind to before branching out.

struct LoopInfo {
    llvm::BasicBlock* continueTarget = nullptr;
    llvm::BasicBlock* exit = nullptr;
    size_t scopeDepth = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// FunctionState — per-function lowering state
// ─────────────────────────────────────────────────────────────────────────────

class FunctionState {
public:
    /// @brief Construct a `FunctionState` and install it.
    ///
    /// `program` is the owning `ProgramState`. The constructor captures the
    /// current per-function state from `program`, installs `fn` as the
    /// current function, saves the builder's insertion point, and clears
    /// the scope/loop stacks so the new function starts fresh.
    ///
    /// `declaredReturnType` is the function's AST return type, used by
    /// `return` lowering to coerce the value. May be null for void.
    FunctionState(class ProgramState& program,
                  llvm::Function* fn,
                  TypeAST* declaredReturnType);

    ~FunctionState();

    FunctionState(const FunctionState&) = delete;
    FunctionState& operator=(const FunctionState&) = delete;

    // ─── Accessors ────────────────────────────────────────────────────────

    llvm::Function* function() const { return fn; }
    TypeAST* declaredReturnType() const { return returnType; }

    llvm::Value* environmentPtr() const { return envPtr; }
    void setEnvironmentPtr(llvm::Value* p) { envPtr = p; }

    // ─── Scope Stack ──────────────────────────────────────────────────────

    void pushScope(BlockStmtAST* block = nullptr);
    void popScope();
    Scope& currentScope();
    const Scope& currentScope() const;
    size_t scopeDepth() const { return scopes.size(); }

    // ─── Loop Stack ───────────────────────────────────────────────────────

    void pushLoop(LoopInfo info);
    void popLoop();
    LoopInfo* currentLoop();
    const LoopInfo* currentLoop() const;

    // ─── Value Bindings ───────────────────────────────────────────────────
    //
    // The `ValueDeclAST* → llvm::Value*` map that the emitter reads when
    // resolving an identifier to its binding. For locals, this maps the
    // declaration to its alloca; for parameters, to the argument alloca;
    // for captured variables inside a closure body, to the env-loaded
    // value or spill slot. For a `cls`-shaped FuncDeclAST binding, to the
    // closure fat pointer value (not an alloca).

    void storeValue(ValueDeclAST* decl, llvm::Value* value);
    llvm::Value* lookupValue(ValueDeclAST* decl) const;
    bool hasValue(ValueDeclAST* decl) const;
    void eraseValue(ValueDeclAST* decl);

    // ─── Binding Save/Restore for Nested Function Bodies ──────────────────
    //
    // When a closure body rebinds a captured declaration to its own
    // env-loaded value, the enclosing function's binding for the same
    // declaration is clobbered. The closure body saves the previous
    // binding before clobbering and restores it on exit. This API
    // exposes that save/restore pair.

    void saveBinding(ValueDeclAST* decl);
    void restoreSavedBindings();

    // ─── Alive/Consumed Convenience ───────────────────────────────────────
    //
    // These forward to the current scope. Callers that need to look at all
    // scopes (like `emitUnwindTo`) walk `scopes` directly.

    void markAlive(ValueDeclAST* decl);
    void markConsumed(ValueDeclAST* decl);
    bool isAlive(ValueDeclAST* decl) const;
    bool isConsumed(ValueDeclAST* decl) const;

    // ─── Full Stack Access ────────────────────────────────────────────────
    //
    // Used by `emitUnwindTo` and by closure capture setup. Exposed as a
    // range-for-able vector. Not `const` because callers mutate through
    // it (e.g. marking bindings consumed during unwind).

    std::vector<Scope>& scopeStack() { return scopes; }

private:
    /// @brief Restore the captured state on destruction.
    ///
    /// Runs on every exit path (normal, early return, exception, ...),
    /// because C++ destructors run for RAII objects on all of them.
    void restore();

    // ─── State ────────────────────────────────────────────────────────────

    ProgramState& program;

    llvm::Function* fn = nullptr;
    TypeAST* returnType = nullptr;
    llvm::Value* envPtr = nullptr;

    std::vector<Scope> scopes;
    std::vector<LoopInfo> loops;

    /// The `ValueDeclAST* → llvm::Value*` map for this function. Moved out
    /// of `program` on construction (the enclosing function's map, if any)
    /// and moved back on destruction.
    std::unordered_map<ValueDeclAST*, llvm::Value*> values;

    /// Bindings that were clobbered by an inner function body and must be
    /// restored on exit. Each pair is `(decl, previousValue)`. Restored in
    /// reverse order, so a decl saved twice ends up with its outermost
    /// value.
    std::vector<std::pair<ValueDeclAST*, llvm::Value*>> savedBindings;

    // ─── Captured Enclosing State ─────────────────────────────────────────
    //
    // Everything below is captured on construction and restored on
    // destruction. `prev*` names mean "the enclosing function state's
    // value at the time this `FunctionState` was constructed."
    //
    // The builder insertion point is captured as an `InsertPointGuard`.
    // Its destructor (called after `restore()`, because members destruct
    // in reverse declaration order and it's declared first among the
    // guards) restores the builder's insertion point.
    //
    // IMPORTANT: `InsertPointGuard` must be constructed before any code
    // runs that could modify the builder, and its destructor restores the
    // exact insertion point that was current at construction time. So it's
    // the very first member (declaration order in the class), and its
    // constructor runs before any of the other fields are set.

    struct CapturedState {
        llvm::Function* prevFunction = nullptr;
        TypeAST* prevReturnType = nullptr;
        llvm::Value* prevEnvPtr = nullptr;
        std::vector<Scope> prevScopes;
        std::vector<LoopInfo> prevLoops;
        std::unordered_map<ValueDeclAST*, llvm::Value*> prevValues;
    };

    CapturedState captured;
};

} // namespace codegen