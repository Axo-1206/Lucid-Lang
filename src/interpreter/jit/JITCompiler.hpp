/// @file jit/JITCompiler.hpp
/// @brief Compiles LLVM IR modules to executable code.

#pragma once

#include "JITSession.hpp"
#include "core/memory/InternedString.hpp"
#include <memory>

namespace llvm {
class Module;
}

namespace interpreter {

/// @brief Compiles LLVM IR modules for execution.
class JITCompiler {
public:
    explicit JITCompiler(JITSession& session);
    ~JITCompiler() = default;

    /// @brief Compile a module and add it to the JIT.
    /// @param module The LLVM module (takes ownership).
    /// @param name The module name.
    /// @throws InterpreterError if compilation fails.
    void compile(std::unique_ptr<llvm::Module> module, InternedString name);

    /// @brief Remove a compiled module.
    /// @param name The module name.
    /// @return true if the module was found and removed.
    bool remove(InternedString name);

    /// @brief Check if a module is compiled and loaded.
    bool isLoaded(InternedString name) const;

    /// @brief Look up a symbol in the compiled code.
    void* lookup(InternedString name);

    /// @brief Look up a symbol in the compiled code by string.
    void* lookup(const std::string& name);

    /// @brief Get the underlying JIT session.
    JITSession& getSession() { return m_session; }

private:
    JITSession& m_session;
};

} // namespace interpreter