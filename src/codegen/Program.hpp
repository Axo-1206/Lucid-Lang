/// @file codegen/Program.hpp
/// @brief Per-program state that outlives any single function body.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The state that exists for the whole `generate()` call: the LLVM context,
/// the module, the builder, and the components built on top of them — the
/// type mapper (`Types`), the runtime call surface (`Abi`), the ownership
/// engine (`Ownership`), and the emitter (`Emitter`).
///
/// Constructed once at the top of `generate()`, destroyed when the last
/// reference to the `CodegenResult` goes away. Its LLVM objects are moved
/// into the result before destruction; its caches and components are
/// discarded.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT per-function state. The current function's scope stack, loop
/// stack, and value-binding map live on `FunctionState`, which is
/// RAII-scoped around each function body. `ProgramState` holds a pointer
/// to the currently active `FunctionState` and provides the enter/exit
/// methods that manage its lifetime.
///
/// It is NOT the emitter. It holds the emitter; the emitter reads from it.
/// The division of responsibility: `ProgramState` owns state, the emitter
/// interprets AST nodes and calls into the state's components.
///
/// ─── Why the Scalar Current-Function Fields Are Here ──────────────────────
/// `currentFunction`, `currentDeclaredReturnType`, and `currentEnvPtr`
/// are technically per-function, so a purist design would put them on
/// `FunctionState`. They live here because the emitter and several other
/// codegen files read them without holding a `FunctionState&` (they hold
/// a `ProgramState&` and access the current function through it). Moving
/// them would require every reader to first look up the active
/// `FunctionState` and then read the field, which is one indirection for
/// no gain. They are set by `FunctionState`'s constructor and restored by
/// its destructor; no other code writes them.
///
/// The scope stack, loop stack, and value-binding map are NOT here. Those
/// live entirely on `FunctionState`. The original Task 3 design had them
/// on `ProgramState` and moved them into each `FunctionState` — the
/// review removed that. Keeping them per-`FunctionState` means a closure
/// body's scopes don't leak into its enclosing function, and nested
/// function bodies don't require moving state back and forth.

#pragma once

#include "Abi.hpp"
#include "Types.hpp"
#include "FunctionState.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace codegen {

class Ownership;
class Emitter;
struct Manifest;

class ProgramState {
public:
    /// @brief Construct a `ProgramState` for one program.
    ///
    /// The `StringPool` and `DiagnosticEngine` are external references
    /// that outlive the `ProgramState`. The `LLVMContext` is owned by
    /// `ProgramState`; the module is created from it in the constructor
    /// body.
    ///
    /// Component construction order matters: `Types` is constructed in
    /// the initializer list (needs `llvmCtx` and `module`), `Abi` next
    /// (needs a back-reference to `*this`), `Ownership` and `Emitter`
    /// after that (constructed in the constructor body, both need
    /// `*this`).
    ProgramState(StringPool& pool,
                 DiagnosticEngine& diagnostics,
                 std::unique_ptr<llvm::LLVMContext> llvmCtx,
                 std::string moduleName);

    ProgramState(const ProgramState&) = delete;
    ProgramState& operator=(const ProgramState&) = delete;

    /// Defined in the .cpp, not `= default` here, because the unique_ptr
    /// members (`ownership_`, `emitter_`) hold incomplete types at this
    /// point. The .cpp includes their full definitions before the
    /// destructor is instantiated.
    ~ProgramState();

    // ─── Core LLVM Objects ────────────────────────────────────────────────

    llvm::LLVMContext& llvmContext() { return *llvmCtx_; }
    llvm::Module& module() { return *module_; }
    llvm::IRBuilder<>& builder() { return builder_; }

    /// Move the context and module out. Called once, at the end of
    /// `generate()`, to populate the `CodegenResult`. After this, the
    /// `ProgramState` is in a moved-from state and must not be used.
    std::unique_ptr<llvm::LLVMContext> releaseContext() {
        return std::move(llvmCtx_);
    }
    std::unique_ptr<llvm::Module> releaseModule() {
        return std::move(module_);
    }

    // ─── Components ───────────────────────────────────────────────────────

    Types& types() { return types_; }
    Abi& abi() { return abi_; }
    Ownership& ownership() { return *ownership_; }
    Emitter& emitter();

    // ─── External References ──────────────────────────────────────────────

    StringPool& pool;
    DiagnosticEngine& diagnostics;

    // ─── Runtime-Function Usage Tracking ──────────────────────────────────
    //
    // The set of runtime ABI functions that `Abi` has declared in this
    // program. Populated by `Abi::declareOrGet` on every runtime call.
    //
    // Read by the module pass to populate the manifest's runtime-symbol
    // list. The interpreter and the AOT linker use that list to decide
    // which runtime object files to link.
    //
    // The set is a `std::set<RuntimeFn>`, not a `std::set<std::string>`,
    // because the manifest needs the enumerator's canonical name (from
    // `functions.def`), and mapping from name back to enumerator would
    // be an extra step.

    std::set<RuntimeFn>& usedRuntimeFns() { return usedRuntimeFns_; }
    const std::set<RuntimeFn>& usedRuntimeFns() const {
        return usedRuntimeFns_;
    }

    // ─── Function Table ───────────────────────────────────────────────────
    //
    // Maps a function declaration to its LLVM function in the module. Set
    // by the declare pass, read by the define pass and by every
    // call-lowering site.
    //
    // Keyed on the declaration, not the mangled name, because two
    // declarations in different modules can have the same mangled name
    // in the (unreachable) case of a mangled-name collision — using the
    // pointer sidesteps the problem entirely.

    void storeFunction(FuncDeclAST* decl, llvm::Function* fn);
    llvm::Function* lookupFunction(FuncDeclAST* decl) const;

    // ─── Drop-Glue Cache ──────────────────────────────────────────────────
    //
    // Lazily-generated `__drop_<type>` and `__copy_<type>` functions for
    // aggregate types. Populated by the `Ownership` layer.

    void storeDropGlue(TypeAST* type, llvm::Function* fn);
    llvm::Function* lookupDropGlue(TypeAST* type) const;

    void storeCopyGlue(TypeAST* type, llvm::Function* fn);
    llvm::Function* lookupCopyGlue(TypeAST* type) const;

    // ─── Closure Caches ──────────────────────────────────────────────────
    //
    // LLVM facts for anonymous functions are cached here rather than on the
    // AST, keeping the AST free of codegen-specific state.

    void storeClosureEnvType(AnonFuncExprAST* expr, llvm::StructType* ty);
    llvm::StructType* lookupClosureEnvType(AnonFuncExprAST* expr) const;

    void storeClosureFunction(AnonFuncExprAST* expr, llvm::Function* fn);
    llvm::Function* lookupClosureFunction(AnonFuncExprAST* expr) const;

    // ─── Module Layouts ───────────────────────────────────────────────────
    //
    // Per-module instance struct types and their field order. Populated
    // by `Types::moduleInstanceType` on first lookup, read by the module
    // pass and by every module-level access.

    std::unordered_map<ModuleAST*, ModuleInstanceLayout>& moduleLayouts() {
        return moduleLayouts_;
    }

    // ─── Module State Symbol Naming ──────────────────────────────────────
    //
    // Keep module state, init, free, and size symbol derivation in one place
    // so emitters and consumers cannot disagree about the sanitized path.

    /// @brief The linker-level symbol name of a module's state global.
    ///
    /// Returns an empty string if `module` is null.
    std::string moduleStateSymbol(ModuleAST* module) const;

    /// @brief The linker-level symbol name of a module's initializer.
    ///
    /// Returns an empty string if `module` is null.
    std::string moduleInitSymbol(ModuleAST* module) const;

    /// @brief The linker-level symbol name of a module's finalizer.
    ///
    /// Returns an empty string if `module` is null.
    std::string moduleFreeSymbol(ModuleAST* module) const;

    /// @brief The linker-level symbol name of a module's size helper.
    ///
    /// Returns an empty string if `module` is null.
    std::string moduleSizeSymbol(ModuleAST* module) const;

    // ─── Current Module ───────────────────────────────────────────────────
    //
    // The `ModuleAST` currently being lowered. Set by the pass runner
    // before dispatching each declaration into the emitter. Used by
    // codegen paths that need to know which module a declaration belongs
    // to (cross-module access, module-relative symbol naming).

    ModuleAST* currentModule = nullptr;
    InternedString currentFile;

    // ─── Current Function State (managed via enter/exit) ──────────────────
    //
    // The currently active `FunctionState`, or null when no function body
    // is being lowered. Entered by `setCurrentFunctionState`, exited by
    // `clearCurrentFunctionState`. Constructing a `FunctionState` saves
    // the previous values of the scalar fields below and installs the
    // new ones; destroying it restores them.

    void setCurrentFunctionState(llvm::Function* fn, TypeAST* returnType);
    void clearCurrentFunctionState();

    FunctionState* currentFunctionState = nullptr;

    // ─── Current Function Scalars ─────────────────────────────────────────
    //
    // Set by `FunctionState`'s constructor, restored by its destructor.
    // No other code writes these.

    llvm::Function* currentFunction = nullptr;
    TypeAST* currentDeclaredReturnType = nullptr;
    llvm::Value* currentEnvPtr = nullptr;

private:
    // ─── Owned LLVM Objects ───────────────────────────────────────────────

    std::unique_ptr<llvm::LLVMContext> llvmCtx_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;

    // ─── Components ───────────────────────────────────────────────────────
    //
    // Construction order is declaration order. `Types` first (needs only
    // `llvmCtx` and `module`), then `Abi` (needs `*this` — see the
    // constructor), then `Ownership` and `Emitter` (constructed in the
    // constructor body via `make_unique`).

    Types types_;
    Abi abi_;
    std::unique_ptr<Ownership> ownership_;
    std::unique_ptr<Emitter> emitter_;

    // ─── Caches ───────────────────────────────────────────────────────────

    std::unordered_map<FuncDeclAST*, llvm::Function*> functions_;
    std::unordered_map<TypeAST*, llvm::Function*> dropGlue_;
    std::unordered_map<TypeAST*, llvm::Function*> copyGlue_;
    std::unordered_map<AnonFuncExprAST*, llvm::StructType*> closureEnvTypes_;
    std::unordered_map<AnonFuncExprAST*, llvm::Function*> closureFunctions_;
    std::unordered_map<ModuleAST*, ModuleInstanceLayout> moduleLayouts_;

    std::set<RuntimeFn> usedRuntimeFns_;

    // ─── Current-Function Lifetime ────────────────────────────────────────
    //
    // The `FunctionState` is owned by `ProgramState` via this unique_ptr.
    // `currentFunctionState` (public) points at it. Both are null when
    // no function body is active.

    std::unique_ptr<FunctionState> currentFunctionStateOwned_;
};

} // namespace codegen