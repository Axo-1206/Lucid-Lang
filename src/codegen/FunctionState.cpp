/// @file codegen/FunctionState.cpp
/// @brief Implementation of per-function lowering state.

#include "FunctionState.hpp"
#include "Program.hpp"

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────
//
// The initializer list order is the member declaration order in the class:
// `program`, then `insertGuard`, then the rest. `insertGuard` is initialized
// in the list, not in the constructor body, because it has no default
// constructor — it needs `program.builder()` and it must be constructed
// before anything runs that could touch the builder.

FunctionState::FunctionState(ProgramState& program_,
                             llvm::Function* fn_,
                             TypeAST* declaredReturnType)
    : program(program_)
    , insertGuard(program_.builder())
    , fn(fn_)
    , returnType(declaredReturnType)
{
    assert(fn && "FunctionState requires a non-null llvm::Function");

    // ─── Capture the enclosing function's scalar state ────────────────────
    // Read `program`'s current values before overwriting them. If this is
    // a nested function body (a closure inside a function), these hold
    // the enclosing function's values; if it's a top-level function, they
    // hold the initial null values that `ProgramState` was constructed
    // with.
    captured.prevFunction = program.currentFunction;
    captured.prevReturnType = program.currentDeclaredReturnType;
    captured.prevEnvPtr = program.currentEnvPtr;

    // ─── Record the enclosing `FunctionState` ─────────────────────────────
    // Also captured before overwriting. A nested function body sees its
    // outer function's `FunctionState` here; a top-level one sees null.
    enclosing = program.currentFunctionState;

    // ─── Install the new state ────────────────────────────────────────────
    // `program` now points at this function for the duration of the body.
    // The scopes, loops, and value map start empty — they're members of
    // this `FunctionState`, not `program`'s, so there's nothing to reset.
    program.currentFunctionState = this;
    program.currentFunction = fn;
    program.currentDeclaredReturnType = declaredReturnType;
    program.currentEnvPtr = nullptr;

    // The builder's insertion point is captured by `insertGuard` in the
    // initializer list. The caller is expected to set up the entry block
    // and set the insertion point to it after this constructor returns.
    // `FunctionState` only guarantees that on destruction, the builder
    // is back where it was when the constructor ran.
}

FunctionState::~FunctionState() {
    restore();
    // Members are destroyed in reverse declaration order. `insertGuard`
    // is declared second, after `program`, so it's destroyed near the
    // end (after `savedBindings`, `values`, `loops`, `scopes`, `envPtr`,
    // `returnType`, `fn`, `enclosing`, and `captured` are gone, and
    // after `program` — a reference, so no-op). Its destructor restores
    // the builder's insertion point as the last thing that happens.
}

// ─────────────────────────────────────────────────────────────────────────────
// Restoration
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::restore() {
    // ─── Restore saved bindings ───────────────────────────────────────────
    // Bindings clobbered by this function's setup (e.g. captured
    // declarations rebound to env-loaded values in a closure body) are
    // restored here. Reverse order so a decl saved twice gets the
    // outermost value last.
    //
    // A top-level function has no `enclosing`, so there are no saved
    // bindings — `saveBinding` returns early when `enclosing` is null.
    // The loop below is a no-op in that case.
    if (enclosing) {
        for (auto it = savedBindings.rbegin(); it != savedBindings.rend(); ++it) {
            if (it->second) {
                enclosing->values[it->first] = it->second;
            } else {
                enclosing->values.erase(it->first);
            }
        }
    }
    savedBindings.clear();

    // ─── Restore the scalar state on `ProgramState` ───────────────────────
    // These were captured in the constructor before being overwritten.
    program.currentFunction = captured.prevFunction;
    program.currentDeclaredReturnType = captured.prevReturnType;
    program.currentEnvPtr = captured.prevEnvPtr;

    // ─── Restore the `currentFunctionState` pointer ───────────────────────
    // Puts the enclosing `FunctionState` back as the active one, or null
    // if there was no enclosing function.
    program.currentFunctionState = enclosing;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scope Stack
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::pushScope(BlockStmtAST* block) {
    Scope s;
    s.block = block;
    scopes.push_back(std::move(s));
}

void FunctionState::popScope() {
    assert(!scopes.empty() && "popScope() on empty scope stack");
    scopes.pop_back();
}

Scope& FunctionState::currentScope() {
    assert(!scopes.empty() && "currentScope() on empty scope stack");
    return scopes.back();
}

const Scope& FunctionState::currentScope() const {
    assert(!scopes.empty() && "currentScope() on empty scope stack");
    return scopes.back();
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop Stack
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::pushLoop(LoopInfo info) {
    loops.push_back(info);
}

void FunctionState::popLoop() {
    assert(!loops.empty() && "popLoop() on empty loop stack");
    loops.pop_back();
}

LoopInfo* FunctionState::currentLoop() {
    return loops.empty() ? nullptr : &loops.back();
}

const LoopInfo* FunctionState::currentLoop() const {
    return loops.empty() ? nullptr : &loops.back();
}

// ─────────────────────────────────────────────────────────────────────────────
// Value Bindings
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::storeValue(ValueDeclAST* decl, llvm::Value* value) {
    assert(decl && "storeValue() with null decl");
    assert(value && "storeValue() with null value");
    values[decl] = value;
}

llvm::Value* FunctionState::lookupValue(ValueDeclAST* decl) const {
    if (!decl) return nullptr;
    auto it = values.find(decl);
    return it != values.end() ? it->second : nullptr;
}

bool FunctionState::hasValue(ValueDeclAST* decl) const {
    if (!decl) return false;
    return values.find(decl) != values.end();
}

void FunctionState::eraseValue(ValueDeclAST* decl) {
    if (!decl) return;
    values.erase(decl);
}

// ─────────────────────────────────────────────────────────────────────────────
// Binding Save/Restore
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::saveBinding(ValueDeclAST* decl) {
    if (!decl) return;

    // No enclosing function: nothing to save. A top-level function body
    // never clobbers an enclosing binding, because there is no enclosing
    // binding to clobber.
    if (!enclosing) return;

    // Save the enclosing function's current binding for this declaration.
    // The nested function body (a closure) is about to overwrite the
    // declaration's binding in its own `values` map with an env-loaded
    // value; the enclosing function's binding must be restored when the
    // nested body finishes.
    auto it = enclosing->values.find(decl);
    llvm::Value* prev = (it != enclosing->values.end()) ? it->second : nullptr;
    savedBindings.emplace_back(decl, prev);
}

void FunctionState::restoreSavedBindings() {
    if (!enclosing) return;

    // Restore each saved binding to the enclosing function's value map,
    // in reverse order so a declaration saved twice gets its outermost
    // value.
    for (auto it = savedBindings.rbegin(); it != savedBindings.rend(); ++it) {
        if (it->second) {
            enclosing->values[it->first] = it->second;
        } else {
            enclosing->values.erase(it->first);
        }
    }
    savedBindings.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Alive/Consumed Convenience
// ─────────────────────────────────────────────────────────────────────────────

void FunctionState::markAlive(ValueDeclAST* decl) {
    currentScope().markAlive(decl);
}

void FunctionState::markConsumed(ValueDeclAST* decl) {
    currentScope().markConsumed(decl);
}

bool FunctionState::isAlive(ValueDeclAST* decl) const {
    // Walk all scopes. A binding is alive if it's in any scope's `alive`
    // set and not in any scope's `consumed` set. The first scope that
    // mentions the declaration decides the answer.
    for (const Scope& s : scopes) {
        if (s.isAlive(decl)) return true;
        if (s.isConsumed(decl)) return false;
    }
    return false;
}

bool FunctionState::isConsumed(ValueDeclAST* decl) const {
    for (const Scope& s : scopes) {
        if (s.isConsumed(decl)) return true;
        if (s.isAlive(decl)) return false;
    }
    return false;
}

} // namespace codegen