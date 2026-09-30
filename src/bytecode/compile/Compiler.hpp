/**
 * @file compile/Compiler.hpp
 *
 * @responsibility The driver: takes a resolved module set and produces
 *                 a Bytecode. The one public entry point of the code
 *                 generator.
 *
 * ─── Design: the compiler trusts Sema ─────────────────────────────────────
 * Every name is resolved, every type is resolved, every constant is
 * folded, every resource kind is classified. The compiler reads those
 * fields and asserts them as it goes. An assertion failure means Sema
 * broke its contract; it is a compiler bug, not a user error.
 */

#pragma once

#include "../Bytecode.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <vector>

namespace lucid::bytecode::compile {

/// @brief The driver.
class Compiler {
public:
    /// @param diagnostics  The engine to report compiler-bug diagnostics
    ///                     through. User-facing errors should already
    ///                     have been reported by Sema; anything the
    ///                     compiler reports here is a bug indicator.
    explicit Compiler(lucid::diag::DiagnosticEngine& diagnostics);

    /// @brief Compile a set of resolved modules into a Bytecode.
    ///
    /// Preconditions (asserted):
    ///   - every module is non-null and !hasErrors
    ///   - every top-level declaration has its Sema-resolved fields
    ///     populated (types resolved, constants folded, resource kinds
    ///     classified)
    ///
    /// On success, returns the Bytecode. On a compiler bug, the
    /// assertion fires before this function returns.
    Bytecode compile(const std::vector<ModuleAST*>& modules);

private:
    lucid::diag::DiagnosticEngine& m_diag;
};

} // namespace lucid::bytecode::compile