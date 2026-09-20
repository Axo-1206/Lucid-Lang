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
#include <utility>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Scope — one lexical scope's live-variable tracker
// ─────────────────────────────────────────────────────────────────────────────
//
// A scope corresponds to one `{ ... }` block, or the parameter list of a
// function body. It tracks:
//
//   - `alive`: the set of declarations whose value is currently owned by
//     this frame and must be released at scope exit.
//
//   - `consumed`: the set of declarations that were moved out of this
//     scope (by a return, a closure capture, an `await`/`join`, or an
//     explicit move) and whose release has been claimed elsewhere.
//
//   - `declarationOrder`: the same bindings as `alive`, in the order they
//     were declared. Cleanup iterates this in reverse.
//
//   - `block`: the `BlockStmtAST*` this scope corresponds to, if any.

struct Scope {
    std::unordered_set<ValueDeclAST*> alive;
    std::unordered_set<ValueDeclAST*> consumed;
    std::vector<ValueDeclAST*> declarationOrder;
    BlockStmtAST* block = nullptr;

    void markAlive(ValueDeclAST* decl) {
        if (!decl) return;
        assert(consumed.find(decl) == consumed.end()
               && "markAlive() called for a binding already consumed in "
                  "this scope");
        if (alive.insert(decl).second) {
            declarationOrder.push_back(decl);
        }
    }

    void markConsumed(ValueDeclAST* decl) {
        if (!decl) return;
        alive.erase(decl);
        consumed.insert(decl);
        // `declarationOrder` is intentionally not touched. Cleanup
        // iterates it and checks `isAlive` to skip consumed entries.
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
    //
    // For a `Future<T>` or `Thread<T>` binding, this maps the declaration
    // to the **value slot** (`alloca T`), not the handle slot. See the
    // `concurrencyHandles` map below.

    void storeValue(ValueDeclAST* decl, llvm::Value* value);
    llvm::Value* lookupValue(ValueDeclAST* decl) const;
    bool hasValue(ValueDeclAST* decl) const;
    void eraseValue(ValueDeclAST* decl);

    // ─── Concurrency Handle Bindings (NEW) ────────────────────────────────
    //
    // A `Future<T>` or `Thread<T>` binding has two pieces of storage:
    //
    //   - The **value slot**: an `alloca T` holding the eventual result.
    //     Registered in the `values` map via `storeValue`, so
    //     `emitIdentifier` loads `T` from it after the binding is
    //     narrowed by `await`/`join`.
    //
    //   - The **handle slot**: an `alloca ptr` holding the runtime handle
    //     (a `FutureHandle*` or `ThreadHandle*`). Written by
    //     `__lucid_async`/`__lucid_spawn`; consumed (set to null) by
    //     `__lucid_await`/`__lucid_join`.
    //
    // The handle slot isn't a "value" in the emitter's sense — no AST
    // expression loads it, and `emitIdentifier` never resolves to it.
    // It's a private runtime channel between the async/spawn emitter and
    // the await/join emitter. It lives in a separate map so the value map
    // stays clean.
    //
    // The methods are keyed on the same `ValueDeclAST*` that the value map
    // uses. `emitAsyncStmt`/`emitSpawnStmt` store the handle slot when
    // they lower the statement; `emitAwaitStmt`/`emitJoinStmt` look it up.

    void storeHandle(ValueDeclAST* decl, llvm::Value* slot);
    llvm::Value* lookupHandle(ValueDeclAST* decl) const;

    // ─── Binding Save/Restore for Nested Function Bodies ──────────────────
    //
    // (Unchanged.)

    void saveBinding(ValueDeclAST* decl);
    void restoreSavedBindings();

    // ─── Alive/Consumed Convenience ───────────────────────────────────────

    void markAlive(ValueDeclAST* decl);
    void markConsumed(ValueDeclAST* decl);
    bool isAlive(ValueDeclAST* decl) const;
    bool isConsumed(ValueDeclAST* decl) const;

    // ─── Full Stack Access ────────────────────────────────────────────────

    std::vector<Scope>& scopeStack() { return scopes; }

private:
    void restore();

    // ─── State ────────────────────────────────────────────────────────────

    ProgramState& program;

    llvm::IRBuilderBase::InsertPointGuard insertGuard;

    FunctionState* enclosing = nullptr;

    llvm::Function* fn = nullptr;
    TypeAST* returnType = nullptr;
    llvm::Value* envPtr = nullptr;

    std::vector<Scope> scopes;
    std::vector<LoopInfo> loops;

    std::unordered_map<ValueDeclAST*, llvm::Value*> values;

    /// Handles for async/spawn bindings. See `storeHandle`/`lookupHandle`.
    ///
    /// Keyed on the same declaration pointer as `values`; the value map's
    /// entry for the same declaration holds the value slot, and this map's
    /// entry holds the handle slot.
    std::unordered_map<ValueDeclAST*, llvm::Value*> concurrencyHandles;

    std::vector<std::pair<ValueDeclAST*, llvm::Value*>> savedBindings;

    struct CapturedScalars {
        llvm::Function* prevFunction = nullptr;
        TypeAST* prevReturnType = nullptr;
        llvm::Value* prevEnvPtr = nullptr;
    };

    CapturedScalars captured;
};

} // namespace codegen