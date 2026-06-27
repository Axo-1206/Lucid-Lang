/**
 * @brief Module resolver for a single parse session.
 * 
 * ModuleResolver maintains state for a specific parse session:
 * - Module path resolution cache
 * - Parsed module cache
 * - Circular import detection stack
 * 
 * Each parse session should have its own ModuleResolver
 * to keep state isolated and allow parallel compilation.
 * 
 */

#pragma once

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "core/ast/BaseAST.hpp"

#include <unordered_map>
#include <vector>
#include <filesystem>
#include <string_view>

namespace parser {

/**
 * @brief Resolves module imports and caches parsed modules.
 * 
 * The ModuleResolver handles:
 * - Converting use paths to file paths (e.g., "std.io" → "std/io.lucid")
 * - Caching parsed modules to avoid re-parsing
 * - Detecting circular imports
 * - Managing module search paths
 * 
 * ## Path Resolution Algorithm
 * 
 * 1. Check custom mappings (from build manifest)
 * 2. Check cache for previously resolved paths
 * 3. Convert use path to file path (replace '.' with '/')
 * 4. Try with .lucid extension
 * 5. Search in package root and additional search paths
 * 6. Try without extension
 * 
 * ## Usage Example
 * 
 * ```cpp
 * ModuleResolver resolver(packageRoot, pool);
 * resolver.addSearchPath("./lib");
 * 
 * // Resolve import
 * InternedString filePath = resolver.resolveUsePath("std.io");
 * 
 * // Check cache
 * if (resolver.isModuleParsed(filePath)) {
 *     ProgramAST* ast = resolver.getParsedModule(filePath);
 * }
 * 
 * // Track circular imports
 * resolver.pushParsing(filePath);
 * ProgramAST* ast = parseModule(filePath);
 * resolver.popParsing();
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
     * @brief Resolve a use path to a file path.
     * 
     * Converts:
     *   "std.io"         → "std/io.lucid"
     *   "math"           → "math.lucid"
     *   "graphics.gl"    → "graphics/gl.lucid"
     * 
     * @param usePath The import path (e.g., "std.io")
     * @return InternedString The resolved file path, or empty if not found
     */
    InternedString resolveUsePath(InternedString usePath);
    
    /**
     * @brief Get the full filesystem path for a resolved module.
     * 
     * @param modulePath The resolved module path (e.g., "std/io.lucid")
     * @return std::filesystem::path The absolute filesystem path
     */
    std::filesystem::path getModuleFilePath(InternedString modulePath) const;
    
    /**
     * @brief Add a search path for module resolution.
     * 
     * Search paths are checked in order when resolving use paths.
     * The package root is always the first search path.
     * 
     * @param path Directory to search for modules
     */
    void addSearchPath(const std::filesystem::path& path);
    
    /**
     * @brief Check if a use path is valid (resolves to an existing file).
     * 
     * @param usePath The import path to check
     * @return true if the path resolves to an existing file
     */
    bool isValidUsePath(InternedString usePath) const;
    
    // ─── Module Caching ───────────────────────────────────────────────────
    
    /**
     * @brief Check if a module has already been parsed.
     */
    bool isModuleParsed(InternedString modulePath) const;
    
    /**
     * @brief Get a parsed module AST by its path.
     * 
     * @return ProgramAST* The parsed AST, or nullptr if not parsed
     */
    ProgramAST* getParsedModule(InternedString modulePath) const;
    
    /**
     * @brief Store a parsed module AST.
     * 
     * @param modulePath The resolved module path (e.g., "std/io.lucid")
     * @param ast The parsed AST (owned by the session's arena)
     */
    void cacheModule(InternedString modulePath, ProgramAST* ast);
    
    // ─── Circular Import Detection ───────────────────────────────────────
    
    /**
     * @brief Check if a module is currently being parsed (circular import).
     */
    bool isParsing(InternedString modulePath) const;
    
    /**
     * @brief Push a module onto the parsing stack.
     * 
     * Call before starting to parse a module.
     */
    void pushParsing(InternedString modulePath);
    
    /**
     * @brief Pop a module from the parsing stack.
     * 
     * Call after finishing parsing a module.
     */
    void popParsing();
    
    /**
     * @brief Get the current parsing stack (for debugging).
     */
    const std::vector<InternedString>& getParsingStack() const { return parsingStack_; }
    
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
    
    // ─── Module Registration ─────────────────────────────────────────────
    
    /**
     * @brief Register a mapping from use path to file path.
     * 
     * This is used to support explicit module mappings from the build manifest.
     */
    void registerModuleMapping(InternedString usePath, InternedString filePath);
    
    /**
     * @brief Get all parsed module paths.
     */
    std::vector<InternedString> getParsedModulePaths() const;
    
    /**
     * @brief Get the package root.
     */
    const std::filesystem::path& getPackageRoot() const { return packageRoot_; }
    
    /**
     * @brief Get the total number of parsed modules.
     */
    size_t getParsedModuleCount() const { return parsedModules_.size(); }
    
private:
    std::filesystem::path packageRoot_;
    StringPool& pool_;
    
    // Map from use path (e.g., "std.io") to resolved file path (e.g., "std/io.lucid")
    std::unordered_map<InternedString, InternedString> usePathToFile_;
    
    // Map from resolved file path to parsed AST
    std::unordered_map<InternedString, ProgramAST*> parsedModules_;
    
    // Stack of modules currently being parsed (for circular detection)
    std::vector<InternedString> parsingStack_;
    
    // Additional search paths (beyond package root)
    std::vector<std::filesystem::path> searchPaths_;
    
    // Custom module mappings (from build manifest)
    std::unordered_map<InternedString, InternedString> customMappings_;
    
    // Cache of resolved filesystem paths (for performance)
    mutable std::unordered_map<InternedString, std::filesystem::path> resolvedPathCache_;
    
    // ─── Private Helpers ──────────────────────────────────────────────────
    
    /**
     * @brief Normalize path separators to forward slashes.
     */
    InternedString normalizePath(std::string_view path) const;
    
    /**
     * @brief Find a file in the search paths.
     * 
     * @param relativePath The relative path to find
     * @return std::filesystem::path The full path, or empty if not found
     */
    std::filesystem::path findFileInSearchPaths(const std::string& relativePath) const;
    
    /**
     * @brief Convert a use path to a relative file path.
     * 
     * @param usePath The use path (e.g., "std.io")
     * @return std::string The relative file path (e.g., "std/io.lucid")
     */
    std::string usePathToRelativePath(InternedString usePath) const;
    
    /**
     * @brief Resolve a relative path to an absolute path.
     * 
     * @param relativePath The relative path (e.g., "std/io.lucid")
     * @return std::filesystem::path The absolute path, or empty if not found
     */
    std::filesystem::path resolveRelativePath(const std::string& relativePath) const;
};

} // namespace parser
