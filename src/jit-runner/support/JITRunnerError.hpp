/// @file jit-runner/support/JITRunnerError.hpp
/// @brief Exception type for JIT runner failures.
///
/// These are compiler-side errors (JIT initialization failed, module
/// verification failed, entry symbol not found) — not runtime panics.
/// The runner throws these and the CLI catches them, converts them to
/// diagnostics, and exits.
///
/// A panic (from `__lucid_panic` inside running JIT code) does not
/// become one of these. Panics terminate the process; they are not
/// exceptions.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace jit_runner {

enum class JITRunnerErrorKind {
    /// The JIT session could not be initialized.
    /// Raised by `initialize()` when LLVM target initialization or
    /// LLJIT creation fails.
    InitFailed,

    /// The CodegenResult is malformed — for example, `success == false`,
    /// `module == nullptr`, or the manifest's program-init symbol is
    /// missing.
    InvalidCodegenResult,

    /// Adding the module to the JIT failed. This can happen if LLVM's
    /// verifier rejects the module, or if the JITDylib rejects it
    /// (duplicate symbol, unsupported construct).
    ModuleAddFailed,

    /// Loading a foreign library named in the manifest failed.
    /// The library was not found, or `dlopen` / `LoadLibrary` rejected it.
    LibraryLoadFailed,

    /// The entry symbol from the manifest could not be resolved in the
    /// JIT. This is a compiler bug — the manifest should never name a
    /// symbol that codegen did not emit.
    EntrySymbolNotFound,

    /// The program initializer or finalizer from the manifest could not
    /// be resolved in the JIT. Same category as EntrySymbolNotFound.
    ProgramSymbolNotFound,

    /// A symbol lookup against the JIT failed for a reason other than
    /// "symbol not found" — for example, LLVM surfaced an error from the
    /// resolver.
    SymbolLookupFailed,
};

inline std::string_view jitRunnerErrorKindToString(JITRunnerErrorKind kind) {
    switch (kind) {
        case JITRunnerErrorKind::InitFailed:            return "InitFailed";
        case JITRunnerErrorKind::InvalidCodegenResult:  return "InvalidCodegenResult";
        case JITRunnerErrorKind::ModuleAddFailed:       return "ModuleAddFailed";
        case JITRunnerErrorKind::LibraryLoadFailed:     return "LibraryLoadFailed";
        case JITRunnerErrorKind::EntrySymbolNotFound:   return "EntrySymbolNotFound";
        case JITRunnerErrorKind::ProgramSymbolNotFound: return "ProgramSymbolNotFound";
        case JITRunnerErrorKind::SymbolLookupFailed:    return "SymbolLookupFailed";
    }
    return "Unknown";
}

class JITRunnerError : public std::runtime_error {
public:
    JITRunnerError(JITRunnerErrorKind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    JITRunnerErrorKind getKind() const { return m_kind; }
    std::string_view kindToString() const {
        return jitRunnerErrorKindToString(m_kind);
    }

private:
    JITRunnerErrorKind m_kind;
};

} // namespace jit_runner