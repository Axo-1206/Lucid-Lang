/// @file codegen/Program.hpp
/// @brief Per-program state that outlives any single function body.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The state that exists for the whole `generate()` call: the LLVM context,
/// the module, the builder, the type mapper, the ABI surface, the function
/// table, the module layouts, the drop-glue cache. Constructed once at the
/// top of `generate()`, destroyed when `CodegenResult` is destroyed.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT per-function state. That lives on `FunctionState`, which is
/// RAII-scoped around each function body and saves/restores the relevant
/// slices of `ProgramState` while it's alive.
///
/// It is NOT the emitter. It holds state; the emitter is a separate object
/// (created in Task 5) that reads from `ProgramState` and `FunctionState`.
///
/// ─── Why the Enclosing-State Fields Exist ─────────────────────────────────
/// `FunctionState` saves and restores the scope stack, loop stack, and
/// value-binding map from `ProgramState`. Those three pieces of state are
/// logically per-function, but they're stored on `ProgramState` so the
/// currently-active function's state is always reachable from any code
/// that has a `ProgramState&`. `FunctionState`'s constructor moves them
/// into itself (via `captured.prevScopes` etc.) and installs an empty set
/// for the new function; its destructor moves them back.
///
/// The fields are named `enclosing*` (not `current*`) to signal that they
/// hold the *enclosing* function's state whenever a nested `FunctionState`
/// is active. At the top level, "enclosing" and "current" coincide.

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
#include <string>
#include <unordered_map>
#include <vector>

namespace codegen {

class ProgramState {
public:
    /// @brief Construct a `ProgramState` for one program.
    ///
    /// The `StringPool` and `DiagnosticEngine` are external references that
    /// outlive the `ProgramState`. The `llvm::Module` is owned by
    /// `ProgramState` — it's constructed here from the supplied name and
    /// context, and moved into the `CodegenResult` at the end of
    /// `generate()`.
    ProgramState(StringPool& pool,
                 DiagnosticEngine& diagnostics,
                 std::unique_ptr<llvm::LLVMContext> llvmCtx,
                 std::string moduleName);

    ProgramState(const ProgramState&) = delete;
    ProgramState& operator=(const ProgramState&) = delete;

    // ─── Core LLVM Objects ────────────────────────────────────────────────

    llvm::LLVMContext& llvmContext() { return *llvmCtx_; }
    llvm::Module& module() { return *module_; }
    llvm::IRBuilder<>& builder() { return builder_; }

    /// Move the context and module out. Called once, at the end of
    /// `generate()`, to populate the `CodegenResult`. After this, the
    /// `ProgramState` is in a moved-from state and must not be used.
    std::unique_ptr<llvm::LLVMContext> releaseContext() { return std::move(llvmCtx_); }
    std::unique_ptr<llvm::Module> releaseModule() { return std::move(module_); }

    // ─── Components ───────────────────────────────────────────────────────

    Types& types() { return types_; }
    Abi& abi() { return abi_; }

    // ─── External References ──────────────────────────────────────────────

    StringPool& pool;
    DiagnosticEngine& diagnostics;

    // ─── Function Table ───────────────────────────────────────────────────
    //
    // Maps a function declaration to its LLVM function in the module. Set
    // by the declare pass (Task 7). Read by the define pass and by every
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
    // aggregate types. Populated by Task 4's `Ownership` layer. Keyed on
    // the type pointer, one entry per aggregate type that needs glue.
    //
    // The exact key is a `TypeAST*` and the value is an `llvm::Function*`.
    // The cache is not consulted by anyone yet — Task 4 introduces the
    // consumers.

    void storeDropGlue(TypeAST* type, llvm::Function* fn);
    llvm::Function* lookupDropGlue(TypeAST* type) const;

    void storeCopyGlue(TypeAST* type, llvm::Function* fn);
    llvm::Function* lookupCopyGlue(TypeAST* type) const;

    // ─── Module Layouts ───────────────────────────────────────────────────
    //
    // Per-module instance struct types and their field order. Populated by
    // `Types::moduleInstanceType` on first lookup, read by the module pass
    // (Task 7) and by every module-level access.

    std::unordered_map<ModuleAST*, ModuleInstanceLayout>& moduleLayouts() {
        return moduleLayouts_;
    }

    // ─── Current Module ───────────────────────────────────────────────────
    //
    // The `ModuleAST` currently being lowered. Set by the pass runner
    // before dispatching each declaration into the emitter. Used by
    // codegen paths that need to know which module a declaration belongs
    // to (cross-module access, module-relative symbol naming).

    ModuleAST* currentModule = nullptr;
    InternedString currentFile;

    // Points to the currently active function body, if any.
    FunctionState* currentFunctionState = nullptr;

    llvm::Function* currentFunction = nullptr;
    TypeAST* currentDeclaredReturnType = nullptr;
    llvm::Value* currentEnvPtr = nullptr;

private:
    // ─── Owned LLVM Objects ───────────────────────────────────────────────

    std::unique_ptr<llvm::LLVMContext> llvmCtx_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;

    // ─── Components ───────────────────────────────────────────────────────

    Types types_;
    Abi abi_;

    // ─── Caches ───────────────────────────────────────────────────────────

    std::unordered_map<FuncDeclAST*, llvm::Function*> functions_;

    std::unordered_map<TypeAST*, llvm::Function*> dropGlue_;
    std::unordered_map<TypeAST*, llvm::Function*> copyGlue_;

    std::unordered_map<ModuleAST*, ModuleInstanceLayout> moduleLayouts_;
};

} // namespace codegen