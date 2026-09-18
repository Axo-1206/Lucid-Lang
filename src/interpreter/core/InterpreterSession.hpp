/// @file core/InterpreterSession.hpp
/// @brief Session-level state: JIT, DynamicLinker, instance table buffer.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "codegen/CodeGenDefaults.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../jit/JITSession.hpp"
#include "../dynlink/DynamicLinker.hpp"

#include <vector>
#include <memory>

namespace interpreter {

/// @brief Long-lived interpreter session.
///
/// Owns the JIT, DynamicLinker, and the backing storage for
/// @__lucid_module_instances registered as an absolute symbol.
class InterpreterSession {
public:
    static constexpr uint32_t kDefaultModuleCapacity = codegen::defaults::kModuleCapacity;

    InterpreterSession(StringPool& pool,
                       DiagnosticEngine& diag,
                       const InterpreterOptions& options = InterpreterOptions{});
    ~InterpreterSession();

    // Non-copyable
    InterpreterSession(const InterpreterSession&) = delete;
    InterpreterSession& operator=(const InterpreterSession&) = delete;

    /// @brief Initialize the JIT and register absolute symbols.
    void initialize();

    /// @brief Shut down the session.
    void shutdown();

    /// @brief Check if the session is initialized.
    bool isInitialized() const { return m_initialized; }

    /// @brief Get the JIT session.
    JITSession& jit() { return m_jit; }

    /// @brief Get the dynamic linker.
    DynamicLinker& linker() { return m_linker; }

    /// @brief Get the string pool.
    StringPool& pool() { return m_pool; }

    /// @brief Get the diagnostic engine.
    DiagnosticEngine& diagnostics() { return m_diag; }

    /// @brief Get the options.
    const InterpreterOptions& options() const { return m_options; }
    InterpreterOptions& options() { return m_options; }

    /// @brief Get the module instance table buffer.
    std::vector<void*>& instanceTable() { return m_instanceTable; }
    const std::vector<void*>& instanceTable() const { return m_instanceTable; }

    /// @brief Address of the instance table buffer.
    void* instanceTableAddress() { return m_instanceTable.data(); }

    /// @brief Register dynamic library symbols.
    void registerLibrarySymbols(const std::string& path, const std::string& name);

private:
    StringPool& m_pool;
    DiagnosticEngine& m_diag;
    InterpreterOptions m_options;
    DynamicLinker m_linker;
    JITSession m_jit;

    /// Backing storage for the `__lucid_module_instances` absolute symbol.
    ///
    /// ─── Address Stability ──────────────────────────────────────────────
    /// This vector's data() address is registered with the JIT as an
    /// absolute symbol during initialize(). The address MUST NOT change
    /// while the session is initialized: reallocating the underlying
    /// buffer would leave the JIT's registered pointer dangling.
    ///
    /// The vector is sized exactly once, in initialize(), to
    /// kDefaultModuleCapacity. No code path resizes it afterward. If a
    /// future change needs a larger capacity, initialize() must be
    /// re-run (after shutdown()) so the JIT re-registers the new address.
    std::vector<void*> m_instanceTable;
    bool m_initialized = false;
};

} // namespace interpreter