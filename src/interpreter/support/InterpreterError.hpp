/// @file support/InterpreterError.hpp
/// @brief Interpreter-specific exceptions and error types.
///
/// This file defines errors that can occur during interpreter operations
/// (JIT compilation, module loading, hot-reload, symbol lookup, etc.).
/// These are compiler-time errors, not runtime panics (see RuntimeError.hpp
/// in codegen for those).

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace interpreter {

/// @brief Enumeration of all interpreter error kinds.
///
/// These are errors that can occur during interpreter operations.
/// Each error has a unique name and is used to categorize exceptions.
enum class InterpreterErrorKind {
    InitFailed,          ///< Interpreter initialization failed
    ModuleLoadFailed,    ///< Failed to load module
    ModuleNotFound,      ///< Module not found
    EntryPointNotFound,  ///< Entry point not found
    ExecutionFailed,     ///< Runtime execution failed
    HotReloadFailed,     ///< Hot-reload operation failed
    LibraryLoadFailed,   ///< Foreign library load failed
    SymbolLookupFailed,  ///< Symbol lookup failed
    InvalidIR,           ///< Invalid LLVM IR
    JITError,            ///< JIT compilation error
    EmptyModuleList,     ///< No modules provided
};

/// @brief Convert an interpreter error kind to a string.
inline std::string_view interpreterErrorKindToString(InterpreterErrorKind kind) {
    switch (kind) {
        case InterpreterErrorKind::InitFailed:          return "InitFailed";
        case InterpreterErrorKind::ModuleLoadFailed:    return "ModuleLoadFailed";
        case InterpreterErrorKind::ModuleNotFound:      return "ModuleNotFound";
        case InterpreterErrorKind::EntryPointNotFound:  return "EntryPointNotFound";
        case InterpreterErrorKind::ExecutionFailed:     return "ExecutionFailed";
        case InterpreterErrorKind::HotReloadFailed:     return "HotReloadFailed";
        case InterpreterErrorKind::LibraryLoadFailed:   return "LibraryLoadFailed";
        case InterpreterErrorKind::SymbolLookupFailed:  return "SymbolLookupFailed";
        case InterpreterErrorKind::InvalidIR:           return "InvalidIR";
        case InterpreterErrorKind::JITError:            return "JITError";
        case InterpreterErrorKind::EmptyModuleList:     return "EmptyModuleList";
        default:                                        return "Unknown";
    }
}

/// @brief Exception thrown when interpreter operations fail.
///
/// This is a lightweight exception type for control flow in the interpreter.
/// Error messages are already reported via DiagnosticEngine before throwing.
/// The exception exists only to unwind the stack to a catch point.
class InterpreterError : public std::runtime_error {
public:
    InterpreterError(InterpreterErrorKind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    InterpreterErrorKind getKind() const { return m_kind; }

    /// @brief Get the error kind as a string (for debugging).
    std::string_view kindToString() const {
        return interpreterErrorKindToString(m_kind);
    }

private:
    InterpreterErrorKind m_kind;
};

} // namespace interpreter