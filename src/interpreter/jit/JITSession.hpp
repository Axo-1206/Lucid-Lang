/// @file jit/JITSession.hpp
/// @brief ORC JIT session management.

#pragma once

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <unordered_map>

namespace interpreter {

class JITError : public std::runtime_error {
public:
    enum class Kind {
        InitFailed,
        ModuleAddFailed,
        ModuleRemoveFailed,
        LookupFailed,
    };

    JITError(Kind kind, const std::string& msg) : std::runtime_error(msg), m_kind(kind) {}
    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/// @brief Manages the ORC JIT session.
///
/// Wraps LLVM's ORC JIT API with InternedString support.
class JITSession {
public:
    explicit JITSession(StringPool& stringPool);
    ~JITSession();

    // Non-copyable
    JITSession(const JITSession&) = delete;
    JITSession& operator=(const JITSession&) = delete;

    /// @brief Initialize the JIT with the host target.
    /// @throws JITError if initialization fails.
    void initialize();

    /// @brief Add a module to the JIT.
    /// @param module The LLVM module (takes ownership).
    /// @param name The module name.
    /// @throws JITError if addition fails.
    void addModule(std::unique_ptr<llvm::Module> module, InternedString name);

    /// @brief Remove a module from the JIT.
    /// @param name The module name.
    /// @return true if the module was found and removed.
    /// @throws JITError if removal fails.
    bool removeModule(InternedString name);

    /// @brief Check if a module is loaded.
    bool hasModule(InternedString name) const;

    /// @brief Look up a symbol in the JIT.
    /// @param name The symbol name.
    /// @return Pointer to the symbol, or nullptr if not found.
    /// @throws JITError if lookup fails.
    void* lookupSymbol(const std::string& name);

    /// @brief Look up a symbol by InternedString.
    void* lookupSymbol(InternedString name);

    /// @brief Get the LLVM context.
    llvm::LLVMContext& getContext() { return *m_context; }

    /// @brief Get the underlying JIT.
    llvm::orc::LLJIT& getJIT() { return *m_jit; }

    /// @brief Get the string pool.
    StringPool& getStringPool() { return m_stringPool; }

    /// @brief Check if initialized.
    bool isInitialized() const { return m_initialized; }

private:
    StringPool& m_stringPool;
    std::unique_ptr<llvm::LLVMContext> m_context;
    std::unique_ptr<llvm::orc::LLJIT> m_jit;
    std::unordered_map<uint32_t, llvm::orc::ResourceTrackerSP> m_trackers;
    bool m_initialized = false;

    void setupTarget();
    void setupPlatformLibraries();
    void registerLibrarySymbols(const std::string& path, const std::string& name);
    std::string internedToString(InternedString name) const;
};

} // namespace interpreter