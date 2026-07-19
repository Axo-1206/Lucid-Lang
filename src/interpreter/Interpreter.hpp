/**
 * @file Interpreter.hpp
 * @brief Main interpreter engine for the Lucid compiler.
 * 
 * @responsibility Orchestrates the entire interpreter pipeline:
 *                 - Receives ModuleAST from the frontend
 *                 - Registers foreign libraries
 *                 - Lowers AST to LLVM IR via IRLowering
 *                 - JIT compiles via ORC
 *                 - Executes entry point
 *                 - Handles hot-reload
 * 
 * @related_files
 *   - src/interpreter/JIT.hpp - JIT session management
 *   - src/interpreter/DynLink.hpp - Foreign library loading
 *   - src/compiler/IRLowering.hpp - AST → LLVM IR lowering
 *   - src/compiler/TypeMapping.hpp - Lucid → LLVM type mapping
 *   - src/ast/BaseAST.hpp - AST root node
 *   - src/runtime/ - Runtime support (panics, memory, threading)
 */

#pragma once

#include "JIT.hpp"
#include "DynLink.hpp"
#include "../compiler/IRLowering.hpp"
#include "../compiler/TypeMapping.hpp"
#include "../core/ast/BaseAST.hpp"
#include "../core/ast/DeclAST.hpp"
#include "../core/diagnostics/Diagnostic.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace interpreter {

/**
 * @brief Exception thrown when interpreter operations fail.
 */
class InterpreterError : public std::runtime_error {
public:
    enum class Kind {
        InitFailed,          // Interpreter initialization failed
        ModuleLoadFailed,    // Failed to load module
        EntryPointNotFound,  // Entry point not found
        ExecutionFailed,     // Runtime execution failed
        HotReloadFailed,     // Hot-reload operation failed
        LibraryLoadFailed,   // Foreign library load failed
    };

    InterpreterError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/**
 * @brief Options for configuring the interpreter.
 */
struct InterpreterOptions {
    bool enableOptimizations = true;      // Enable LLVM optimizations
    bool enableDebugInfo = false;         // Enable debug info generation
    int optimizationLevel = 2;            // 0-3 (same as -O0 to -O3)
    std::string entryPoint = "main";      // Default entry point name
    std::vector<std::string> libraryPaths; // Additional library search paths
    bool enableHotReload = false;         // Enable hot-reload support
    bool verbose = false;                 // Enable verbose output
};

/**
 * @brief Result of executing a module.
 */
struct ExecutionResult {
    int exitCode = 0;                     // Exit code from the entry point
    bool success = true;                  // Whether execution succeeded
    std::string errorMessage;             // Error message if execution failed
    double executionTimeMs = 0.0;         // Execution time in milliseconds
};

/**
 * @brief Main interpreter engine.
 * 
 * Orchestrates the entire pipeline from ModuleAST to execution.
 * 
 * @par Usage Example
 * @code
 *   Interpreter interpreter;
 *   InterpreterOptions opts;
 *   opts.entryPoint = "main";
 *   opts.enableOptimizations = true;
 *   
 *   if (!interpreter.initialize(opts)) {
 *       // Handle initialization failure
 *   }
 *   
 *   // Parse and semantically analyze the module (frontend)
 *   ModuleAST* module = parseAndAnalyze("main.luc");
 *   
 *   // Run the module
 *   auto result = interpreter.runModule(module);
 *   if (result.success) {
 *       std::cout << "Exit code: " << result.exitCode << "\n";
 *   }
 * @endcode
 * 
 * @par Hot-Reload Support
 * The interpreter supports hot-reloading modules when source files change:
 * @code
 *   // On file change:
 *   ModuleAST* newModule = parseAndAnalyze("main.luc");
 *   interpreter.hotReload(newModule, "main");
 * @endcode
 */
class Interpreter {
public:
    Interpreter();
    ~Interpreter();

    // Non-copyable
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    // Moveable
    Interpreter(Interpreter&&) = default;
    Interpreter& operator=(Interpreter&&) = default;

    /**
     * @brief Initialize the interpreter with the given options.
     * 
     * @param options Configuration options
     * @return true on success
     * @throws InterpreterError if initialization fails
     */
    bool initialize(const InterpreterOptions& options = InterpreterOptions{});

    /**
     * @brief Run a module with an entry point.
     * 
     * @param module The ModuleAST to run (must be semantically validated)
     * @param entryPoint Override the entry point name (uses options if empty)
     * @return ExecutionResult with exit code and status
     * @throws InterpreterError if execution fails
     */
    ExecutionResult runModule(ModuleAST* module, const std::string& entryPoint = "");

    /**
     * @brief Load a module without executing it.
     * 
     * Useful for REPL or when you want to load multiple modules first.
     * 
     * @param module The ModuleAST to load
     * @return true on success
     * @throws InterpreterError if loading fails
     */
    bool loadModule(ModuleAST* module);

    /**
     * @brief Hot-reload a module from updated AST.
     * 
     * Replaces an existing module with a new version while the program runs.
     * 
     * @param module The new ModuleAST
     * @param moduleName The module name to replace
     * @return true on success
     * @throws InterpreterError if hot-reload fails
     */
    bool hotReload(ModuleAST* module, const std::string& moduleName);

    /**
     * @brief Execute a function by name.
     * 
     * @tparam Ret The return type
     * @tparam Args The argument types
     * @param name The function name
     * @param args The arguments
     * @return The function's return value
     * @throws InterpreterError if the function is not found
     */
    template <typename Ret, typename... Args>
    Ret execute(const std::string& name, Args... args) {
        auto* fnPtr = m_jit.lookupSymbol(name);
        if (!fnPtr) {
            throw InterpreterError(InterpreterError::Kind::EntryPointNotFound,
                                   "Function not found: " + name);
        }
        
        auto fn = reinterpret_cast<Ret(*)(Args...)>(fnPtr);
        return fn(args...);
    }

    /**
     * @brief Execute a function with a signature.
     * 
     * @param name The function name
     * @param signature The function signature (for validation)
     * @return The function pointer
     * @throws InterpreterError if the function is not found
     */
    template <typename Func>
    Func getFunction(const std::string& name) {
        auto* fnPtr = m_jit.lookupSymbol(name);
        if (!fnPtr) {
            throw InterpreterError(InterpreterError::Kind::EntryPointNotFound,
                                   "Function not found: " + name);
        }
        return reinterpret_cast<Func>(fnPtr);
    }

    /**
     * @brief Register a foreign library with the interpreter.
     * 
     * @param libraryName The library name (e.g., "opengl", "m")
     * @return true on success
     * @throws InterpreterError if loading fails
     */
    bool registerForeignLibrary(const std::string& libraryName);

    /**
     * @brief Register foreign libraries from a module's @[link] attributes.
     * 
     * @param module The module to scan for @[link] attributes
     */
    void registerForeignLibraries(ModuleAST* module);

    /**
     * @brief Get the JIT session.
     * 
     * @return Reference to the JIT session
     */
    JITSession& getJIT() { return m_jit; }

    /**
     * @brief Get the dynamic linker.
     * 
     * @return Reference to the dynamic linker
     */
    DynLink& getDynLink() { return m_dynLink; }

    /**
     * @brief Check if the interpreter is initialized.
     * 
     * @return true if initialized
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * @brief Get the current options.
     * 
     * @return The current interpreter options
     */
    const InterpreterOptions& getOptions() const { return m_options; }

    /**
     * @brief Set a custom IRLowering instance.
     * 
     * Useful for testing or customization.
     * 
     * @param lowerer The IRLowering instance (takes ownership)
     */
    void setIRLowerer(std::unique_ptr<IRLowering> lowerer);

private:
    /**
     * @brief Generate a unique module name with version.
     * 
     * @param module The module to generate a name for
     * @return The generated name
     */
    std::string generateModuleName(ModuleAST* module);

    /**
     * @brief Generate a versioned name for hot-reload.
     * 
     * @param baseName The base module name
     * @return The versioned name
     */
    std::string generateVersionedName(const std::string& baseName);

    /**
     * @brief Find the entry point in a module.
     * 
     * @param module The module to search
     * @param entryPoint The suggested entry point name
     * @return The name of the entry point, or empty if not found
     */
    std::string findEntryPoint(ModuleAST* module, const std::string& entryPoint);

    /**
     * @brief Check if a module has semantic errors.
     * 
     * @param module The module to check
     * @return true if the module has errors
     */
    bool hasErrors(const ModuleAST* module) const;

    /**
     * @brief Report errors from a module.
     * 
     * @param module The module with errors
     * @return Number of errors reported
     */
    size_t reportErrors(const ModuleAST* module) const;

    /**
     * @brief Setup LLVM optimization passes.
     * 
     * @param module The LLVM module to optimize
     */
    void setupOptimizations(llvm::Module* module);

    /**
     * @brief Handle a runtime panic.
     * 
     * @param exception The panic exception
     * @return The exit code to return
     */
    int handlePanic(const std::exception& exception);

    // ─── Members ─────────────────────────────────────────────────────────────

    JITSession m_jit;
    DynLink m_dynLink;
    std::unique_ptr<IRLowering> m_irLowerer;
    std::unique_ptr<TypeMapping> m_typeMapper;
    InterpreterOptions m_options;
    
    // Module version tracking for hot-reload
    std::unordered_map<std::string, std::string> m_moduleVersions;
    std::unordered_map<std::string, ModuleAST*> m_loadedModules;
    
    bool m_initialized = false;
    bool m_hasActiveModule = false;
    std::string m_activeModuleName;
};

} // namespace interpreter