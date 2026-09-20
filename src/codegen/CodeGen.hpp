/// @file codegen/CodeGen.hpp
/// @brief The one public entry point of the codegen subsystem.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The public API. Everything downstream of codegen — the interpreter,
/// the AOT backend, the LSP's IR view — consumes `CodegenResult`, which
/// this file defines and the single function `generate()` produces.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the implementation. `generate()`'s body lives in
/// `CodeGen.cpp`; the passes live in `passes/`.
///
/// It does NOT expose the internals. `ProgramState`, `Types`, `Abi`,
/// `Ownership`, `Emitter` are all implementation details. Callers see
/// only the `CodegenResult`.
///
/// ─── The Single Entry Point ───────────────────────────────────────────────
/// `generate()` takes a list of modules, a string pool, a diagnostic
/// engine, and options, and returns a `CodegenResult`. There is no other
/// public function.
///
/// The old API returned `std::vector<std::unique_ptr<llvm::Module>>`, one
/// module per input. The new API returns a single `llvm::Module` for the
/// whole program. The interpreter and AOT backends will adapt — the
/// interpreter splits it back into per-file modules for hot-reload, the
/// AOT backend passes it to the optimiser as-is.

#pragma once

#include "Manifest.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// CodegenResult — the plain-data output of one codegen run
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Everything `generate()` produces.
///
/// The LLVM context is owned by the result; the module lives inside it.
/// The manifest is the host-readable summary of what was generated.
///
/// The result is move-only (owns a unique_ptr<LLVMContext> and a
/// unique_ptr<Module>). Callers either use it in place or move it into a
/// long-lived structure.
struct CodegenResult {
    std::unique_ptr<llvm::LLVMContext> llvmCtx;
    std::unique_ptr<llvm::Module> module;
    Manifest manifest;

    /// True if codegen succeeded without emitting any error diagnostics.
    /// A false value may still have a valid (partial) module — the caller
    /// decides whether to use it or discard it.
    bool success = false;

    bool hasModule() const { return module != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// generate — the one public entry point
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generate LLVM IR for a whole program.
///
/// The `modules` list must be in dependency order: a module's dependencies
/// appear before it. This is the order that `ModuleResolver` produces.
/// Codegen does not re-derive it.
///
/// On success, the returned `CodegenResult` has `success == true` and a
/// verified `llvm::Module`. On failure, `success == false`, the diagnostic
/// engine has the error, and the module may be null or partial.
///
/// The string pool and diagnostic engine must outlive the call.
CodegenResult generate(const std::vector<ModuleAST*>& modules,
                       StringPool& pool,
                       DiagnosticEngine& diagnostics);

} // namespace codegen