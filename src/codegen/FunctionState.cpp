/// @file codegen/FunctionState.cpp
/// @brief Implementation of per-function lowering state.

#include "FunctionState.hpp"
#include "Program.hpp"

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

FunctionState::FunctionState(ProgramState& program_,
                             llvm::Function* fn_,
                             TypeAST* declaredReturnType)
    : program(program_)
    , fn(fn_)
    , returnType(declaredReturnType)
{
    assert(fn && "FunctionState requires a non-null llvm::Function");

    // Capture the enclosing function state before installing the new one.
    enclosing = program.currentFunctionState;
    program.currentFunctionState = this;

    captured.prevFunction = program.currentFunction;
    captured.prevReturnType = program.currentDeclaredReturnType;
    captured.prevEnvPtr = program.currentEnvPtr;

    // ─── Install the new state ────────────────────────────────────────────
    program.currentFunction = fn;
    program.currentDeclaredReturnType = declaredReturnType;
    program.currentEnvPtr = nullptr;

    // ─── Save the builder's insertion point ───────────────────────────────
    // This is the last thing, because installing the new state doesn't
    // touch the builder. The `InsertPointGuard` lives in `captured`, and
    // its destructor restores the insertion point when this
    // `FunctionState` is destroyed.
    //
    // NOTE: we don't set the insertion point here — the caller does,
    // after creating the entry block. `FunctionState` only guarantees
    // that on destruction, the builder is back where it was.

    captured.insertGuard.emplace(program.builder);
}

FunctionState::~FunctionState() {
    restore();
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

    // ─── Restore captured state ───────────────────────────────────────────
    program.currentFunction = captured.prevFunction;
    program.currentDeclaredReturnType = captured.prevReturnType;
    program.currentEnvPtr = captured.prevEnvPtr;
    program.currentFunctionState = enclosing;

    // `captured.insertGuard`'s destructor runs after this body, restoring
    // the builder's insertion point. Member destruction order is reverse
    // of declaration order, and `captured` is the last member, so this
    // body runs before `captured`'s members destruct.
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
    if (!decl || !enclosing) return;
    // Save the current binding from the enclosing function's value map,
    // not from this function's local table. If the declaration has no
    // binding in the outer frame, save `nullptr` so the restore path
    // erases it.
    auto it = enclosing->values.find(decl);
    llvm::Value* prev = (it != enclosing->values.end()) ? it->second : nullptr;
    savedBindings.emplace_back(decl, prev);
}

void FunctionState::restoreSavedBindings() {
    if (!enclosing) return;
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
    // set and not in any scope's `consumed` set.
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