/**
 * @file JIT.hpp
 * @brief ORC JIT session management for the Lucid interpreter.
 * 
 * @responsibility Wraps LLVM's ORC JIT API, providing module management,
 *                 symbol lookup, and hot-reload support.
 * 
 * @related_files
 *   - src/interpreter/Interpreter.hpp - uses JITSession for execution
 *   - src/interpreter/DynLink.hpp - registers foreign symbols
 *   - src/codegen/IRLowering.hpp - produces IR modules for this JIT
 *   - src/core/memory/StringPool.hpp - String interning for names
 */

#pragma once

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include "../core/memory/StringPool.hpp"
#include "../core/memory/InternedString.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace interpreter {

/**
 * @brief Exception thrown when JIT operations fail.
 */
class JITError : public std::runtime_error {
public:
    enum class Kind {
        InitFailed,          // JIT initialization failed
        ModuleAddFailed,     // Failed to add module to JIT
        ModuleRemoveFailed,  // Failed to remove module
        SymbolNotFound,      // Requested symbol not found
        LookupFailed,        // Symbol lookup operation failed
    };

    JITError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/**
 * @brief Manages the ORC JIT session for Lucid execution.
 * 
 * Owns the LLVM context, the JIT instance, and tracks loaded modules
 * with resource trackers for hot-reload support.
 * 
 * @par String Pool Integration
 * The JITSession shares a StringPool with the rest of the compiler.
 * All symbol and module names are stored as InternedString for efficient
 * lookup and comparison.
 * 
 * @par Usage Example
 * @code
 *   StringPool stringPool;
 *   JITSession jit(stringPool);
 *   if (!jit.initialize()) {
 *       // Handle initialization failure
 *   }
 *   
 *   auto module = std::make_unique<llvm::Module>("mymod", jit.getContext());
 *   // ... populate module with IR ...
 *   
 *   InternedString moduleName = stringPool.intern("mymod");
 *   if (jit.addModule(std::move(module), moduleName)) {
 *       auto* fn = jit.lookupSymbol("main");
 *       if (fn) {
 *           auto main = reinterpret_cast<int(*)()>(fn);
 *           return main();
 *       }
 *   }
 * @endcode
 * 
 * @par Hot-Reload Support
 * Each module is added with a ResourceTracker. To reload:
 *   1. Generate new IR with a versioned name
 *   2. Add the new module (gets a new tracker)
 *   3. Remove the old module using its tracker
 *   4. Update the version mapping
 */
class JITSession {
public:
    /**
     * @brief Construct a JITSession with a StringPool.
     * 
     * @param stringPool The string pool for name interning
     */
    explicit JITSession(StringPool& stringPool);
    ~JITSession();

    // Non-copyable
    JITSession(const JITSession&) = delete;
    JITSession& operator=(const JITSession&) = delete;

    // Moveable
    JITSession(JITSession&&) = default;
    JITSession& operator=(JITSession&&) = delete;

    /**
     * @brief Initialize the JIT with the host target.
     * 
     * Sets up the LLJIT instance and registers any platform-specific
     * libraries (e.g., luc_kernel.dll on Windows).
     * 
     * @return true on success, false on failure
     * @throws JITError if initialization fails
     */
    bool initialize();

    /**
     * @brief Add a compiled LLVM module to the JIT.
     * 
     * The module is added with a ResourceTracker that enables
     * safe removal during hot-reload.
     * 
     * @param module The LLVM module to add (takes ownership)
     * @param moduleName The module name as an InternedString
     * @return true on success
     * @throws JITError if module addition fails
     */
    bool addModule(std::unique_ptr<llvm::Module> module,
                   InternedString moduleName);

    /**
     * @brief Add a compiled LLVM module to the JIT with a string name.
     * 
     * Convenience wrapper that interns the name automatically.
     * 
     * @param module The LLVM module to add (takes ownership)
     * @param moduleName The module name as a string
     * @return true on success
     * @throws JITError if module addition fails
     */
    bool addModule(std::unique_ptr<llvm::Module> module,
                   const std::string& moduleName);

    /**
     * @brief Remove a module by name (hot-reload).
     * 
     * Uses the stored ResourceTracker to safely remove the module
     * and all its compiled code from the JIT.
     * 
     * @param moduleName The module name as an InternedString
     * @return true if the module was found and removed
     * @throws JITError if removal fails
     */
    bool removeModule(InternedString moduleName);

    /**
     * @brief Remove a module by name with a string.
     * 
     * Convenience wrapper that interns the name automatically.
     * 
     * @param moduleName The module name as a string
     * @return true if the module was found and removed
     * @throws JITError if removal fails
     */
    bool removeModule(const std::string& moduleName);

    /**
     * @brief Check if a module is loaded.
     * 
     * @param moduleName The module name as an InternedString
     * @return true if the module is loaded
     */
    bool hasModule(InternedString moduleName) const;

    /**
     * @brief Check if a module is loaded with a string.
     * 
     * Convenience wrapper that interns the name automatically.
     * 
     * @param moduleName The module name as a string
     * @return true if the module is loaded
     */
    bool hasModule(const std::string& moduleName) const;

    /**
     * @brief Look up a symbol in the JIT.
     * 
     * Searches all loaded modules and foreign symbols for the symbol.
     * 
     * @param symbolName The symbol name (mangled or unmangled) as a string
     * @return Pointer to the symbol, or nullptr if not found
     * @throws JITError if the lookup operation fails
     */
    void* lookupSymbol(const std::string& symbolName);

    /**
     * @brief Look up a symbol in the JIT.
     * 
     * Searches all loaded modules and foreign symbols for the symbol.
     * 
     * @param symbolName The symbol name as an InternedString
     * @return Pointer to the symbol, or nullptr if not found
     * @throws JITError if the lookup operation fails
     */
    void* lookupSymbol(InternedString symbolName);

    /**
     * @brief Get the LLVM context.
     * 
     * @return Reference to the LLVM context used by this JIT
     */
    llvm::LLVMContext& getContext() { return *m_context; }

    /**
     * @brief Get the underlying LLJIT instance.
     * 
     * @return Reference to the LLJIT instance
     */
    llvm::orc::LLJIT& getJIT() { return *m_jit; }

    /**
     * @brief Get the string pool.
     * 
     * @return Reference to the string pool
     */
    StringPool& getStringPool() { return m_stringPool; }

    /**
     * @brief Check if the JIT is initialized.
     */
    bool isInitialized() const { return m_initialized; }

private:
    /**
     * @brief Handle LLVM Error types, converting to JITError.
     * 
     * @tparam T The return type (void or value type)
     * @param error The LLVM Error to handle
     * @param errorKind The kind of JITError to throw
     * @param message Additional error message
     * @return T The value if no error (for non-void returns)
     * @throws JITError if the error is present
     */
    template <typename T = void>
    T handleError(llvm::Error error, JITError::Kind errorKind,
                  const std::string& message = "");

    /**
     * @brief Convert InternedString to std::string for LLVM operations.
     * 
     * @param name The interned string
     * @return std::string The string representation
     */
    std::string internedToString(InternedString name) const;

    /**
     * @brief Setup the host target machine.
     * 
     * Initializes LLVM target registration for the current platform.
     */
    void setupTarget();

    /**
     * @brief Register platform-specific libraries.
     * 
     * On Windows, loads luc_kernel.dll and registers its symbols.
     * On POSIX, loads libluc_kernel.so/dylib.
     */
    void setupPlatformLibraries();

    /**
     * @brief Register a library's symbols with the JIT.
     * 
     * @param libraryPath Path to the library
     * @param libraryName Name for tracking
     */
    void registerLibrarySymbols(const std::string& libraryPath,
                                const std::string& libraryName);

    // ─── Members ─────────────────────────────────────────────────────────────

    StringPool& m_stringPool;
    std::unique_ptr<llvm::LLVMContext> m_context;
    std::unique_ptr<llvm::orc::LLJIT> m_jit;
    std::unordered_map<uint32_t, llvm::orc::ResourceTrackerSP> m_trackers;
    bool m_initialized = false;
};

} // namespace interpreter