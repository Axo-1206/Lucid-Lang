/**
 * @file ModuleResolver.hpp
 * @brief Module resolver for a single parse session.
 * 
 * ModuleResolver maintains state for a specific parse session:
 * - Module path resolution cache
 * - Parsed module cache
 * - Circular import detection stack
 * - Dependency order computation
 * 
 * @design_decision Single Responsibility
 *   ModuleResolver handles module discovery, caching, import resolution,
 *   and dependency ordering. These are all related to "finding and
 *   tracking modules" and belong together.
 * 
 * @design_decision Resolved Imports Stored on ModuleAST
 *   After parsing, ModuleResolver populates ModuleAST::resolvedImports
 *   with the actual resolved module ASTs. This makes the AST self-contained
 *   and allows CodeGen to access imports without holding a reference to
 *   the resolver.
 * 
 * @design_decision Dependency Order Computed After All Modules Parsed
 *   The topological order is computed once after all modules are parsed,
 *   and stored in ModuleAST::dependencyOrder. This allows CodeGen to
 *   initialize globals in the correct order.
 * 
 * Each parse session should have its own ModuleResolver
 * to keep state isolated and allow parallel compilation.
 */

#pragma once

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "core/ast/BaseAST.hpp"

#include <unordered_map>
#include <vector>
#include <queue>
#include <filesystem>
#include <string>

namespace parser {

/**
 * @brief Resolves module imports and caches parsed modules.
 * 
 * The ModuleResolver handles:
 * - Converting import paths to file paths (e.g., "std.io" → "std/io.luc")
 * - Caching parsed modules to avoid re-parsing
 * - Detecting circular imports
 * - Computing dependency order for global initialization
 * - Populating ModuleAST with resolved imports
 * 
 * ## Path Resolution
 * 
 * 1. Check cache for previously resolved paths
 * 2. Convert import path to file path (replace '.' with '/')
 * 3. Try with .luc extension
 * 4. Search in package root
 * 
 * ## Usage Example
 * 
 * ```cpp
 * ModuleResolver resolver(packageRoot, pool);
 * 
 * // Parse a module (resolver caches it)
 * ModuleAST* ast = parseModule("main.luc", resolver);
 * 
 * // Check cache
 * if (resolver.isModuleParsed(filePath)) {
 *     ModuleAST* ast = resolver.getParsedModule(filePath);
 * }
 * 
 * // Get dependency order
 * for (ModuleAST* module : resolver.getModulesInOrder()) {
 *     // Initialize globals in this order
 * }
 * 
 * // Cross-module access
 * ModuleAST* target = resolver.findModuleByAlias(currentModule, "math");
 * 
 * // Track circular imports
 * ScopedParsingGuard guard(&resolver, filePath);
 * ModuleAST* ast = parseModule(filePath);
 * resolver.cacheModule(filePath, ast);
 * ```
 */
class ModuleResolver {
public:
    // ─── Construction ──────────────────────────────────────────────────────
    
    /**
     * @brief Create a module resolver.
     * 
     * @param packageRoot The package root directory (e.g., "./src")
     * @param pool String pool for interning paths
     */
    ModuleResolver(const std::filesystem::path& packageRoot, StringPool& pool);
    
    // ─── Path Resolution ──────────────────────────────────────────────────
    
    /**
     * @brief Resolve an import path to a file path.
     * 
     * Converts:
     *   "std.io"         → "std/io.luc"
     *   "math"           → "math.luc"
     *   "graphics.gl"    → "graphics/gl.luc"
     * 
     * @param importPath The import path (e.g., "std.io")
     * @return InternedString The resolved file path, or empty if not found
     */
    InternedString resolveImportPath(InternedString importPath);
    
    /**
     * @brief Get the full filesystem path for a resolved module.
     * 
     * @param modulePath The resolved module path (e.g., "std/io.luc")
     * @return std::filesystem::path The absolute filesystem path
     */
    std::filesystem::path getModuleFilePath(InternedString modulePath) const;
    
    /**
     * @brief Check if an import path is valid.
     * 
     * @param importPath The import path to check
     * @return true if the path resolves to an existing file
     */
    bool isValidImportPath(InternedString importPath) const;
    
    // ─── Module Caching ───────────────────────────────────────────────────
    
    /**
     * @brief Check if a module has already been parsed.
     */
    bool isModuleParsed(InternedString modulePath) const;
    
    /**
     * @brief Get a parsed module AST by its path.
     * 
     * @return ModuleAST* The parsed AST, or nullptr if not parsed
     */
    ModuleAST* getParsedModule(InternedString modulePath) const;
    
    /**
     * @brief Store a parsed module AST.
     * 
     * This also resolves and stores imports on the module, and updates
     * the module order list for dependency tracking.
     * 
     * @param modulePath The resolved module path (e.g., "std/io.luc")
     * @param ast The parsed AST (owned by the session's arena)
     */
    void cacheModule(InternedString modulePath, ModuleAST* ast);
    
    // ─── Dependency Order ──────────────────────────────────────────────────
    
    /**
     * @brief Get the topological order of a module.
     * 
     * @param modulePath The module path
     * @return int The order (0 = first, higher = later), or -1 if not set
     */
    int getModuleOrder(InternedString modulePath) const;
    
    /**
     * @brief Get all modules sorted by dependency order (topological).
     * 
     * The order is computed once after all modules are parsed.
     * Modules that import others appear after their dependencies.
     * 
     * @return const std::vector<ModuleAST*>& Modules in dependency order
     */
    const std::vector<ModuleAST*>& getModulesInOrder() const;
    
    /**
     * @brief Find a module by alias from another module's context.
     * 
     * @param fromModule The module that contains the import
     * @param alias The import alias
     * @return ModuleAST* The resolved module, or nullptr if not found
     */
    ModuleAST* findModuleByAlias(ModuleAST* fromModule, InternedString alias) const;
    
    // ─── Circular Import Detection ───────────────────────────────────────
    
    /**
     * @brief Check if a module is currently being parsed.
     * 
     * @param modulePath The module path to check
     * @return true if the module is in the parsing stack (circular import)
     */
    bool isParsing(InternedString modulePath) const;
    
    /**
     * @brief Push a module onto the parsing stack.
     * 
     * Call before starting to parse a module.
     * Prefer ScopedParsingGuard over calling this directly.
     */
    void pushParsing(InternedString modulePath);
    
    /**
     * @brief Pop a module from the parsing stack.
     * 
     * Call after finishing parsing a module.
     * Prefer ScopedParsingGuard over calling this directly.
     */
    void popParsing();
    
    // ─── File Operations ──────────────────────────────────────────────────
    
    /**
     * @brief Read the source code of a module.
     * 
     * @param filePath The file path to read
     * @return std::string The source code, or empty if file not found
     */
    std::string readModuleSource(InternedString filePath) const;
    
    /**
     * @brief Check if a module file exists.
     * 
     * @param filePath The file path to check
     * @return true if the file exists
     */
    bool moduleFileExists(InternedString filePath) const;

    // ─── Module Enumeration ──────────────────────────────────────────────────
    
    /**
     * @brief Get all parsed modules.
     * 
     * @return std::vector<ModuleAST*> All parsed modules (unordered)
     */
    std::vector<ModuleAST*> getAllModules() const;
    
    /**
     * @brief Get the number of parsed modules.
     */
    size_t getModuleCount() const;
    
private:
    std::filesystem::path packageRoot_;
    StringPool& pool_;
    
    // ─── Internal State ──────────────────────────────────────────────────
    
    // Map from import path (e.g., "std.io") to resolved file path (e.g., "std/io.luc")
    std::unordered_map<InternedString, InternedString> importPathToFile_;
    
    // Map from resolved file path to parsed AST
    std::unordered_map<InternedString, ModuleAST*> parsedModules_;
    
    // Modules in dependency order (topological)
    std::vector<ModuleAST*> moduleOrderList_;
    
    // Map from module path to dependency order
    std::unordered_map<InternedString, int> moduleOrderMap_;
    
    // Stack of modules currently being parsed (for circular detection)
    std::vector<InternedString> parsingStack_;
    
    // Cache of resolved filesystem paths (for performance)
    mutable std::unordered_map<InternedString, std::filesystem::path> resolvedPathCache_;
    
    // ─── Private Helpers ──────────────────────────────────────────────────
    
    /**
     * @brief Convert an import path to a relative file path.
     * 
     * @param importPath The import path (e.g., "std.io")
     * @return std::string The relative file path (e.g., "std/io.luc")
     */
    std::string importPathToRelativePath(InternedString importPath) const;
    
    /**
     * @brief Resolve a relative path to an absolute path.
     * 
     * @param relativePath The relative path (e.g., "std/io.luc")
     * @return std::filesystem::path The absolute path, or empty if not found
     */
    std::filesystem::path resolveRelativePath(const std::string& relativePath) const;
    
    /**
     * @brief Check if a file exists at the given path.
     * 
     * @param fullPath The full filesystem path
     * @return true if the file exists and is readable
     */
    bool fileExists(const std::filesystem::path& fullPath) const;
    
    /**
     * @brief Resolve and store imports on a ModuleAST.
     * 
     * Populates ModuleAST::resolvedImports with the actual resolved modules.
     * 
     * @param module The module to resolve imports for
     */
    void resolveAndStoreImports(ModuleAST* module);
    
    /**
     * @brief Compute the topological order of all parsed modules.
     * 
     * Uses Kahn's algorithm on the import dependency graph.
     * Results are stored in moduleOrderMap_ and moduleOrderList_,
     * and on each ModuleAST::dependencyOrder.
     */
    void computeTopologicalOrder();
    
    /**
     * @brief Build the import dependency graph.
     * 
     * @param graph Output: adjacency list (dependency → dependent)
     * @param indegree Output: indegree count for each module
     */
    void buildDependencyGraph(
        std::unordered_map<InternedString, std::vector<InternedString>>& graph,
        std::unordered_map<InternedString, int>& indegree
    ) const;
    
    /**
     * @brief Find a module by its alias from another module's context.
     * 
     * @param fromModule The module that contains the import
     * @param alias The import alias
     * @return ModuleAST* The resolved module, or nullptr if not found
     */
    ModuleAST* findModuleByAliasInternal(ModuleAST* fromModule, InternedString alias) const;
};

/**
 * @brief RAII guard for ModuleResolver's circular-import tracking.
 * 
 * Pushes a module onto the parsing stack on construction and pops it on
 * destruction - on every exit path, including early returns.
 * 
 * ## Usage
 * 
 * ```cpp
 * if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
 *     // circular import - report and return before constructing the guard
 *     return nullptr;
 * }
 * ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
 * // every return below this point pops correctly, automatically
 * ```
 * 
 * Non-copyable, non-movable.
 */
struct ScopedParsingGuard {
    ScopedParsingGuard(ModuleResolver* resolver, InternedString filePath)
        : resolver_(resolver)
    {
        if (resolver_) {
            resolver_->pushParsing(filePath);
        }
    }

    ~ScopedParsingGuard() {
        if (resolver_) {
            resolver_->popParsing();
        }
    }

    ScopedParsingGuard(const ScopedParsingGuard&) = delete;
    ScopedParsingGuard& operator=(const ScopedParsingGuard&) = delete;
    ScopedParsingGuard(ScopedParsingGuard&&) = delete;
    ScopedParsingGuard& operator=(ScopedParsingGuard&&) = delete;

private:
    ModuleResolver* resolver_;
};

} // namespace parser