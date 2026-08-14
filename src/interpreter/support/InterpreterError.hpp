/// @file support/InterpreterError.hpp
/// @brief Interpreter-specific error integration with the diagnostic system.

#pragma once

#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace interpreter {

/// @brief Exception thrown when interpreter operations fail.
///
/// This exception integrates with the existing DiagnosticEngine system.
/// Errors are reported via diagnostics and then wrapped in an exception
/// for control flow.
class InterpreterError : public std::runtime_error {
public:
    enum class Kind {
        InitFailed,          // Interpreter initialization failed
        ModuleLoadFailed,    // Failed to load module
        ModuleNotFound,      // Module not found
        EntryPointNotFound,  // Entry point not found
        ExecutionFailed,     // Runtime execution failed
        HotReloadFailed,     // Hot-reload operation failed
        LibraryLoadFailed,   // Foreign library load failed
        SymbolLookupFailed,  // Symbol lookup failed
        InvalidIR,           // Invalid LLVM IR
        JITError,            // JIT compilation error
        EmptyModuleList,     // No modules provided
    };

    InterpreterError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind), m_code(DiagCode(0)) {}

    InterpreterError(Kind kind, DiagCode code, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind), m_code(code) {}

    InterpreterError(Kind kind, DiagCode code, BaseAST* node, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind), m_code(code), m_node(node) {}

    Kind getKind() const { return m_kind; }
    DiagCode getCode() const { return m_code; }
    BaseAST* getNode() const { return m_node; }

    /// @brief Report this error to a diagnostic engine.
    void report(DiagnosticEngine& diagnostics) const {
        if (m_code != DiagCode(0)) {
            diagnostics.error(m_code, m_node, m_code != DiagCode(0) ? what() : "");
        } else {
            diagnostics.errorAt(DiagCode::Backend_CodegenError, 
                                m_node ? m_node->loc : SourceLocation(), 
                                what());
        }
    }

    /// @brief Check if this error has an associated diagnostic code.
    bool hasCode() const { return m_code != DiagCode(0); }

private:
    Kind m_kind;
    DiagCode m_code = DiagCode(0);
    BaseAST* m_node = nullptr;
};

/// @brief Convert error kind to string.
inline std::string_view interpreterErrorKindToString(InterpreterError::Kind kind) {
    using Kind = InterpreterError::Kind;
    switch (kind) {
        case Kind::InitFailed:          return "InitFailed";
        case Kind::ModuleLoadFailed:    return "ModuleLoadFailed";
        case Kind::ModuleNotFound:      return "ModuleNotFound";
        case Kind::EntryPointNotFound:  return "EntryPointNotFound";
        case Kind::ExecutionFailed:     return "ExecutionFailed";
        case Kind::HotReloadFailed:     return "HotReloadFailed";
        case Kind::LibraryLoadFailed:   return "LibraryLoadFailed";
        case Kind::SymbolLookupFailed:  return "SymbolLookupFailed";
        case Kind::InvalidIR:           return "InvalidIR";
        case Kind::JITError:            return "JITError";
        case Kind::EmptyModuleList:     return "EmptyModuleList";
        default:                        return "Unknown";
    }
}

// ─── Interpreter-Specific Diagnostic Helpers ──────────────────────────────

/// @brief Report a module load failure.
inline void reportModuleLoadError(DiagnosticEngine& diagnostics, 
                                  InternedString moduleName,
                                  const std::string& reason) {
    diagnostics.error(DiagCode::Sem_UndefinedModule, nullptr,
                      "failed to load module '", 
                      StringPool::instance().lookup(moduleName),
                      "': ", reason);
}

/// @brief Report an entry point not found.
inline void reportEntryPointNotFound(DiagnosticEngine& diagnostics,
                                     const std::string& entryPoint) {
    diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                      "entry point '", entryPoint, "' not found in any loaded module");
}

/// @brief Report a library load failure.
inline void reportLibraryLoadError(DiagnosticEngine& diagnostics,
                                   const std::string& libraryName,
                                   const std::string& reason) {
    diagnostics.error(DiagCode::Ffi_LibraryNotFound, nullptr,
                      "failed to load library '", libraryName, "': ", reason);
}

/// @brief Report a symbol lookup failure.
inline void reportSymbolLookupError(DiagnosticEngine& diagnostics,
                                    const std::string& symbolName,
                                    const std::string& reason) {
    diagnostics.error(DiagCode::Ffi_UnknownSymbol, nullptr,
                      "symbol '", symbolName, "' not found: ", reason);
}

} // namespace interpreter