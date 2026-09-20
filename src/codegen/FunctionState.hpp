/// @file codegen/FunctionState.hpp
/// @brief Per-function state, RAII-scoped around each function body.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The state that exists while lowering one function body and is restored
/// when the body finishes. Three groups of things live here:
///
///   1. The current `llvm::Function*` being lowered, its declared return
///      type, and its environment pointer. These are the values that
///      `return` lowering, parameter registration, and closure-body setup
///      read.
///
///   2. The scope stack. Each `{ ... }` block pushes a `Scope`; the block's
///      exit pops it. The scope holds which bindings are alive and which
///      have been consumed, so cleanup can be emitted at scope exit.
///
///   3. The loop stack. Each `for`/`while`/`do-while` pushes a `LoopInfo`
///      with the continue and exit blocks; `break`/`continue` read the
///      back.
///
/// Plus the `ValueDeclAST* → llvm::Value*` binding map that the emitter
/// uses to resolve identifiers to their LLVM storage.
///
/// ─── RAII Semantics ───────────────────────────────────────────────────────
/// Constructing a `FunctionState` captures the enclosing function's scalar
/// state from `ProgramState`, saves the builder's insertion point, and
/// installs the new function. Destroying it restores the saved scalars and
/// insertion point.
///
/// A nested closure body constructs a nested `FunctionState`. The
/// enclosing function's scalars and insertion point are restored when the
/// nested state is destroyed. This replaces the `ClosureBodyScope` RAII
/// class from the pre-redesign codebase: nested function bodies are not a
/// special case, they're just RAII.
///
/// ─── What's Captured and Restored ─────────────────────────────────────────
/// Captured from `ProgramState` on construction, restored on destruction:
///
///   - `currentFunction` — the `llvm::Function*` being lowered
///   - `currentDeclaredReturnType` — the AST return type, used by `return`
///   - `currentEnvPtr` — the environment pointer for a closure body
///   - `currentFunctionState` — the pointer back to the enclosing
///     `FunctionState` (or null for a top-level function)
///
/// Captured from the builder on construction, restored on destruction:
///
///   - the builder's insertion point, held by the `insertGuard` member
///
/// NOT captured, because they live entirely on `FunctionState` and are
/// destroyed with it:
///
///   - `scopes`, `loops`, `values` — these start empty for each function
///     body. A nested closure body has its own empty stacks and its own
///     empty value map; the enclosing function's state is untouched.
///     The `savedBindings` mechanism (see below) handles the specific
///     case where the closure body rebinds a declaration that the
///     enclosing function also has a binding for.
///
/// ─── Why Not on ProgramState? ─────────────────────────────────────────────
/// Function state is per-function, and a program has many functions.
/// Putting the scope stack, loop stack, and value map on `ProgramState`
/// would mean saving and restoring them around every function body. RAII
/// on `FunctionState` makes the discipline automatic and impossible to
/// forget, and it means a nested function body's state can't leak into its
/// enclosing function's state.
///
/// The scalar fields (`currentFunction` and friends) *are* on
/// `ProgramState`, because the emitter and several other codegen files
/// read them without holding a `FunctionState&`. `FunctionState` is what
/// keeps them in sync with the active body.

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
    /// current per-function scalar state from `program`, installs `fn` as
    /// the current function, and saves the builder's insertion point. The
    /// scope stack, loop stack, and value map start empty for the new body.
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

    /// @brief The enclosing `FunctionState`, or null if this is a
    ///        top-level function body.
    ///
    /// Read by `saveBinding` to reach the enclosing function's value map,
    /// and by `restore` to reinstall the previous `currentFunctionState`
    /// pointer on `ProgramState`.
    FunctionState* enclosingState() const { return enclosing; }

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
    //
    // A `saveBinding` call for a declaration in a top-level function
    // (no enclosing `FunctionState`) is a no-op — there's nothing to
    // save from.

    void saveBinding(ValueDeclAST* decl);
    void restoreSavedBindings();

    // ─── Alive/Consumed Convenience ───────────────────────────────────────
    //
    // These forward to the current scope. Callers that need to look at all
    // scopes (like `emitUnwindTo`) walk `scopeStack()` directly.

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
    ///
    /// Restores: the three scalar fields on `ProgramState`, the
    /// `currentFunctionState` pointer on `ProgramState`, and any saved
    /// bindings the closure-body setup clobbered. Does not touch the
    /// builder's insertion point — that's the `insertGuard` destructor's
    /// job, and it runs after this function returns.
    void restore();

    // ─── State ────────────────────────────────────────────────────────────

    /// The owning program state. Held by reference; outlives every
    /// `FunctionState`.
    ProgramState& program;

    /// RAII guard for the builder's insertion point.
    ///
    /// Declared **after** `program` and **before** every other member, so
    /// it's constructed after `program` (which it needs for
    /// `program.builder()`) and destroyed last (member destruction is
    /// reverse of declaration order). Its destructor restores the exact
    /// insertion point that was current at construction time.
    ///
    /// The `InsertPointGuard` has no default constructor. It must be
    /// initialized in the constructor's initializer list, and the list
    /// must run after `program` is initialized.
    llvm::IRBuilderBase::InsertPointGuard insertGuard;

    /// The enclosing function's `FunctionState`, or null for a top-level
    /// function body. Set by the constructor from
    /// `program.currentFunctionState` (which the constructor then
    /// overwrites with `this`). Restored by `restore()`.
    FunctionState* enclosing = nullptr;

    /// The function being lowered. Captured from the constructor's `fn`
    /// parameter; the constructor also stores it on `program` as
    /// `program.currentFunction`.
    llvm::Function* fn = nullptr;

    /// The declared AST return type, used by `return` lowering. Captured
    /// from the constructor's `declaredReturnType` parameter; the
    /// constructor also stores it on `program` as
    /// `program.currentDeclaredReturnType`.
    TypeAST* returnType = nullptr;

    /// The environment pointer for a closure body, or null. Set explicitly
    /// by closure lowering; also mirrored on `program.currentEnvPtr` while
    /// this state is active.
    llvm::Value* envPtr = nullptr;

    /// The function's scope stack. Starts empty; the caller pushes the
    /// function-level scope and each nested block's scope. Destroyed with
    /// the `FunctionState`.
    std::vector<Scope> scopes;

    /// The function's loop stack. Starts empty; each loop pushes and pops.
    std::vector<LoopInfo> loops;

    /// The function's `ValueDeclAST* → llvm::Value*` map. Starts empty;
    /// parameter registration and local declaration push entries. Destroyed
    /// with the `FunctionState`.
    std::unordered_map<ValueDeclAST*, llvm::Value*> values;

    /// Bindings that were clobbered by an inner function body and must be
    /// restored on exit. Each pair is `(decl, previousValue)`. Restored in
    /// reverse order, so a decl saved twice ends up with its outermost
    /// value.
    std::vector<std::pair<ValueDeclAST*, llvm::Value*>> savedBindings;

    /// Captured scalar state of the enclosing function. These are the
    /// fields the constructor reads from `program` and the destructor
    /// writes back to `program`.
    struct CapturedScalars {
        llvm::Function* prevFunction = nullptr;
        TypeAST* prevReturnType = nullptr;
        llvm::Value* prevEnvPtr = nullptr;
    };

    CapturedScalars captured;
};

} // namespace codegen