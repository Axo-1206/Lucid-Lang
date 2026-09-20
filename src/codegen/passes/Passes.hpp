/// @file codegen/passes/Passes.hpp
/// @brief The three codegen passes: declare, define, module.
///
/// ─── What a Pass Is ───────────────────────────────────────────────────────
/// A pass is a function that walks a set of modules and emits part of
/// their IR. The three passes run in a fixed order:
///
///   1. DeclarePass  — types and prototypes.
///   2. DefinePass   — function bodies.
///   3. ModulePass   — module state globals and program-level init/free.
///
/// Each pass iterates over the *entire program* before the next pass
/// starts. That's what makes forward references work: by the time
/// DefinePass runs on module A, every function in every module has a
/// prototype.
///
/// ─── What a Pass Is NOT ───────────────────────────────────────────────────
/// It is NOT a class. Passes have no state of their own; they read from
/// `ProgramState` and the module list, and emit IR via the emitter or
/// directly.
///
/// It is NOT the entry point. `generate()` orchestrates the three passes.
/// Each pass is called once.
///
/// ─── Error Handling ───────────────────────────────────────────────────────
/// Passes may emit diagnostics. They do not throw. If a pass encounters
/// an unrecoverable error (e.g. a manifest inconsistency), it reports
/// and continues; the caller checks the diagnostic engine's state after
/// the pass completes and decides whether to continue.

#pragma once

#include "codegen/Program.hpp"
#include "codegen/Manifest.hpp"

#include "core/ast/BaseAST.hpp"

#include <vector>

namespace codegen {

struct ModuleAST;

/// @brief Emit LLVM types and function prototypes for every module.
///
/// For each module in the list, in order:
///   - Emit every struct's LLVM type (via `Types::structType`).
///   - Emit every enum's LLVM type (via `Types::enumType`).
///   - Emit every function prototype (via `Emitter::emit` on the
///     function's declaration in declare mode).
///   - Emit the module's instance struct type (via
///     `Types::moduleInstanceType`), but not the global yet.
///   - Record each function's mangled name in the manifest.
///
/// No bodies are emitted. After this pass, every function that any
/// subsequent pass references has an `llvm::Function*` in the module.
void runDeclarePass(const std::vector<ModuleAST*>& modules,
                    ProgramState& program,
                    Manifest& manifest);

/// @brief Emit function bodies for every module.
///
/// For each module in the list, in order:
///   - Emit every non-generic function's body.
///   - Emit every specialization's body.
///   - Emit any remaining declaration that produces IR (module-level
///     variable initializers, etc.).
///
/// Emits the entry symbol name into the manifest if an `@[export]`
/// function named `main` is found.
void runDefinePass(const std::vector<ModuleAST*>& modules,
                   ProgramState& program,
                   Manifest& manifest);

/// @brief Emit module state globals and program-level initialization.
///
/// For each module, in order:
///   - Emit `@__module_state_<id>` as a zero-initialized global of the
///     module's instance struct type.
///   - Emit `__module_size_<name>() -> i64`.
///   - Emit `__init_module_<name>(ptr) -> void` — initializes each field
///     in declaration order.
///   - Emit `__free_module_<name>(ptr) -> void` — releases each field
///     in reverse declaration order.
///
/// Then emit:
///   - `__lucid_program_init() -> void` — calls each module's `__init`.
///   - `__lucid_program_free() -> void` — calls each module's `__free`.
///
/// Populates the manifest's module list and program init/free symbols.
///
/// Checks that each module's dependencies (from `resolvedImports`) appear
/// earlier in the list. A violation is an internal-error diagnostic
/// (the module list is supposed to be in dependency order).
void runModulePass(const std::vector<ModuleAST*>& modules,
                   ProgramState& program,
                   Manifest& manifest);

} // namespace codegen