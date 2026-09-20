/// @file codegen/context/CodeGenContext.hpp
/// @brief Transitional aggregator over ProgramState and FunctionState.
///
/// ─── What This File Is NOW ────────────────────────────────────────────────
/// Phase 3 is rewriting the codegen subsystem from the bottom up. Task 3
/// split the old god-object `CodeGenContext` into two new objects:
///
///   - `ProgramState`   — per-program state (module, caches, function table)
///   - `FunctionState`  — per-function state (current function, scopes, loops)
///
/// Each of those lives in its own file with its own responsibility.
///
/// `CodeGenContext` — this file — is a transitional shim that exposes the
/// old flat API surface as forwarders to the two new objects. Its purpose
/// is to keep every existing call site compiling while Task 4–10 migrate
/// them to the new APIs one file at a time.
///
/// ─── What This File Will Be ──────────────────────────────────────────────
/// At the end of Phase 3, this file is deleted. Every call site will use
/// `ProgramState` and `FunctionState` directly, or — more likely — the
/// `Emitter` object (Task 5), which composes the two.
///
/// ─── What This File Is NOT ANYMORE ───────────────────────────────────────
/// It no longer owns:
///   - the LLVM module (moved to ProgramState)
///   - the LLVM context (moved to ProgramState)
///   - the IRBuilder (moved to ProgramState)
///   - the type cache (moved to Types, owned by ProgramState)
///   - the struct cache (moved to Types)
///   - the function table (moved to ProgramState)
///   - the value-binding map (moved to FunctionState)
///   - the scope stack (moved to FunctionState)
///   - the loop stack (moved to FunctionState)
///   - the runtime function cache (moved to Abi, owned by ProgramState)
///   - the module-instance table and layouts (going away entirely in
///     Task 7; dropped from the shim now)
///   - the module ID assignment (going away entirely in Task 7)
///
/// Anything it appears to have, it forwards. If a field or method is not
/// in the new design, it is not here — old call sites that use it fail to
/// compile, which is the signal for their owning task to rewrite them.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Types.hpp"
#include "codegen/LLVMTypeHelpers.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Instructions.h>

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// CodeGenOptions — vestigial, emptied in Task 3
// ─────────────────────────────────────────────────────────────────────────────
//
// The old options struct carried `moduleCapacity` (the size of the
// `@__lucid_module_instances` table) and `moduleIds` (the pre-assigned ID
// map). Both are artifacts of the module-instance table design, which is
// going away in Task 7: the table is replaced by per-module state globals,
// and module identity is the `ModuleAST*` pointer, not an integer.
//
// The struct is kept empty for now so `generate()`'s signature still
// compiles. Task 8 deletes it, along with the `options` parameter on
// `generate()`.

struct CodeGenOptions {
};

/// @brief Transitional aggregator over `ProgramState` and `FunctionState`.
///
/// See the file header for the design. Every field and method either
/// forwards to one of the two new objects or is a helper that belongs to
/// the shim during the migration and moves elsewhere in a later task.
struct CodeGenContext {
    // ─── Ownership ────────────────────────────────────────────────────────

    /// The per-program state. Constructed by the caller and passed in by
    /// reference; `CodeGenContext` does not own it.
    ProgramState& prog;

    /// The per-function state. Owned by the shim, created by
    /// `setCurrentFunction` and destroyed by `clearCurrentFunction`.
    /// Null when no function body is active.
    std::unique_ptr<FunctionState> func;

    // ─── Vestigial Options ────────────────────────────────────────────────
    //
    // Kept so `generate()`'s signature compiles. Never read.
    CodeGenOptions options;

    // ─── Constructor ──────────────────────────────────────────────────────

    CodeGenContext(ProgramState& program)
        : prog(program) {}

    CodeGenContext(const CodeGenContext&) = delete;
    CodeGenContext& operator=(const CodeGenContext&) = delete;

    // ─── Forwarders: External References ──────────────────────────────────

    StringPool& pool() { return prog.pool; }
    DiagnosticEngine& diagnostics() { return prog.diagnostics; }

    // The shim historically exposed these as fields, not methods. To keep
    // every existing call site compiling without editing them, provide
    // field-style access via reference members that alias the underlying
    // ProgramState fields. This is legal because ProgramState outlives
    // CodeGenContext (the caller guarantees the lifetime).
    //
    // NOTE: reference members disable the implicit assignment operators,
    // but the copy/move operators are already deleted above, so this is
    // fine.
    StringPool& pool_ref = prog.pool;
    DiagnosticEngine& diagnostics_ref = prog.diagnostics;

    // ─── Forwarders: Core LLVM Objects ────────────────────────────────────

    llvm::LLVMContext& llvmCtx() { return prog.llvmContext(); }
    llvm::Module& module() { return prog.module(); }
    llvm::IRBuilder<>& builder() { return prog.builder(); }

    // ─── Forwarders: Components ───────────────────────────────────────────

    Types& types() { return prog.types(); }
    Abi& abi() { return prog.abi(); }

    // ─── Forwarders: Function Table ───────────────────────────────────────

    void storeFunction(FuncDeclAST* decl, llvm::Function* fn) {
        prog.storeFunction(decl, fn);
    }

    llvm::Function* lookupFunction(FuncDeclAST* decl) const {
        return prog.lookupFunction(decl);
    }

    // ─── Forwarders: Current Module ───────────────────────────────────────

    ModuleAST* currentModule() const { return prog.currentModule; }
    void setCurrentModule(ModuleAST* m) { prog.currentModule = m; }

    InternedString currentFile() const { return prog.currentFile; }
    void setCurrentFile(InternedString f) { prog.currentFile = f; }

    // ─── Forwarders: Current Function ─────────────────────────────────────

    /// @brief Enter a function body.
    ///
    /// Constructs a `FunctionState`, which:
    ///   - saves the enclosing function state (if any)
    ///   - installs the new function as `ProgramState::currentFunction`
    ///   - clears the scope and loop stacks for the new function
    ///   - saves the builder's insertion point for restoration on exit
    ///
    /// The caller is expected to subsequently create the entry block and
    /// set the builder's insertion point to it.
    void setCurrentFunction(llvm::Function* fn,
                            TypeAST* declaredReturnType = nullptr);

    /// @brief Leave the current function body.
    ///
    /// Destroys the `FunctionState`, which restores the enclosing function
    /// state and the builder's insertion point.
    void clearCurrentFunction();

    llvm::Function* getCurrentFunction() const {
        return func ? func->function() : nullptr;
    }

    TypeAST* currentDeclaredReturnType() const {
        return func ? func->declaredReturnType() : nullptr;
    }

    llvm::Value* currentEnvPtr() const {
        return func ? func->environmentPtr() : nullptr;
    }

    void setCurrentEnvPtr(llvm::Value* p) {
        if (func) func->setEnvironmentPtr(p);
    }

    // ─── Forwarders: Value Bindings ───────────────────────────────────────
    //
    // The value-binding map lives on FunctionState. When no function is
    // active (e.g. during the declare pass, which doesn't have bodies),
    // these are no-ops or return null. Callers that rely on the map being
    // present without a function are bugs in the new design, and the
    // null-return behavior surfaces them.

    void storeValue(ValueDeclAST* decl, llvm::Value* value) {
        if (func) func->storeValue(decl, value);
    }

    llvm::Value* lookupValue(ValueDeclAST* decl) const {
        return func ? func->lookupValue(decl) : nullptr;
    }

    bool hasValue(ValueDeclAST* decl) const {
        return func && func->hasValue(decl);
    }

    void eraseValue(ValueDeclAST* decl) {
        if (func) func->eraseValue(decl);
    }

    // ─── Forwarders: Scope Stack ──────────────────────────────────────────

    void pushLiveScope(BlockStmtAST* block = nullptr) {
        if (func) func->pushScope(block);
    }

    void popLiveScope() {
        if (func) func->popScope();
    }

    void markAlive(ValueDeclAST* decl) {
        if (func) func->markAlive(decl);
    }

    void markConsumed(ValueDeclAST* decl) {
        if (func) func->markConsumed(decl);
    }

    bool isAlive(ValueDeclAST* decl) const {
        return func && func->isAlive(decl);
    }

    bool isConsumed(ValueDeclAST* decl) const {
        return func && func->isConsumed(decl);
    }

    // ─── Forwarders: Loop Stack ───────────────────────────────────────────

    using LoopInfo = codegen::LoopInfo;

    void pushLoop(llvm::BasicBlock* header, llvm::BasicBlock* exit,
                  llvm::BasicBlock* continueTarget = nullptr) {
        if (!func) return;
        LoopInfo info;
        info.continueTarget = continueTarget ? continueTarget : header;
        info.exit = exit;
        info.scopeDepth = func->scopeDepth();
        func->pushLoop(info);
    }

    void popLoop() {
        if (func) func->popLoop();
    }

    LoopInfo* currentLoop() {
        return func ? func->currentLoop() : nullptr;
    }

    bool insideLoop() const {
        return func && func->currentLoop() != nullptr;
    }

    // ─── Null Coalesce Stack ──────────────────────────────────────────────
    //
    // This stack was on the old context but has no equivalent in the new
    // design. It stays on the shim until the emitter is rewritten in
    // Task 5, at which point the null-coalesce lowering is restructured
    // to not need a stack (the null-coalesce expression is lowered within
    // a single basic block structure, and the "current ?? context" can be
    // a field on the emitter, not a stack).

    struct NullCoalesceContext {
        llvm::BasicBlock* fallbackBlock = nullptr;
        bool isActive = false;
    };
    std::vector<NullCoalesceContext> nullCoalesceStack;

    void pushNullCoalesce(llvm::BasicBlock* fallbackBlock) {
        nullCoalesceStack.push_back({fallbackBlock, true});
    }

    void popNullCoalesce() {
        if (!nullCoalesceStack.empty()) {
            nullCoalesceStack.pop_back();
        }
    }

    NullCoalesceContext* currentNullCoalesce() {
        return nullCoalesceStack.empty() ? nullptr : &nullCoalesceStack.back();
    }

    bool isInsideNullCoalesce() const {
        return !nullCoalesceStack.empty() && nullCoalesceStack.back().isActive;
    }

    llvm::BasicBlock* getNullCoalesceFallbackBlock() const {
        if (nullCoalesceStack.empty()) return nullptr;
        return nullCoalesceStack.back().fallbackBlock;
    }

    // ─── Cleanup and Unwind ───────────────────────────────────────────────
    //
    // These have real logic and stay on the shim until Task 4's `Ownership`
    // rewrite moves them there. They read from `FunctionState::scopeStack()`
    // instead of the old `liveTrackers` vector.

    /// Emit cleanup for every scope from the current depth down to (but not
    /// including) `targetDepth`. Non-destructive — does not pop scopes or
    /// mutate their trackers.
    void emitUnwindTo(size_t targetDepth);

    // ─── Type Helpers ─────────────────────────────────────────────────────
    //
    // All forward to `Types`. The old field-style accessors
    // (`getStringType()` etc.) become methods on the shim that call the
    // same-named methods on `Types`.

    llvm::StructType* getStringType()  { return prog.types().stringType(); }
    llvm::StructType* getSliceType()   { return prog.types().sliceType(); }
    llvm::StructType* getClosureType() { return prog.types().closureType(); }
    llvm::StructType* getArenaType()   { return prog.types().arenaType(); }
    llvm::StructType* getArenaDescriptorType() {
        return prog.types().arenaDescriptorType();
    }

    llvm::Value* createStringLiteral(const std::string& str);

    // ─── Intrinsic Helpers ────────────────────────────────────────────────

    llvm::Function* getLLVMIntrinsicDecl(
        llvm::Intrinsic::ID id,
        llvm::ArrayRef<llvm::Type*> argTypes);

    // ─── Scope Unwind Helper ──────────────────────────────────────────────
    //
    // Deprecated. Call sites in Task 5+ will use the Emitter's exit path
    // instead. Kept for the migration.

    // (emitUnwindTo is declared above.)

    // ─── Resource Reassignment Helper ─────────────────────────────────────
    //
    // Old signature kept for compatibility. Its logic moves to
    // `Ownership::intoOwned` + `Ownership::drop` in Task 4, and call sites
    // in `lowerAssignExpr` (Task 5) will be rewritten to use the new API.

    void reassign(ValueDeclAST* decl, llvm::Value* oldValue,
                  llvm::Value* newValue);

    // ─── Type Cache Helpers ───────────────────────────────────────────────
    //
    // The caches live on `Types` now. These forwarders keep old call sites
    // compiling; Task 5+ migrate them to `ctx.types().get(...)` directly.
    //
    // NOTE: `Types` caches are keyed on the AST type pointer and populated
    // by `Types::get`. There is no way to inject a value into the cache
    // from outside — if a caller needs to add an entry, it should call
    // `Types::get` and let the caching happen naturally. So `cacheType`
    // is a no-op that returns the argument, and `lookupType` forwards to
    // `Types::get`. This is a semantic change from the old behavior: old
    // code that pre-populated the cache is now doing redundant work that
    // `Types` would have done on the first `get` anyway.

    void cacheType(TypeAST* lucidType, llvm::Type* /*llvmType*/) {
        (void)lucidType;
        // No-op: `Types::get` is the only populator.
    }

    llvm::Type* lookupType(TypeAST* lucidType) {
        return prog.types().get(lucidType);
    }

    void cacheStruct(StructDeclAST* decl, llvm::StructType* /*structType*/) {
        (void)decl;
    }

    llvm::StructType* lookupStruct(StructDeclAST* decl) {
        return prog.types().structType(decl);
    }
};

} // namespace codegen