/**
 * @file ModuleResolver.hpp
 * @brief Module resolver for a single parse session.
 * 
 * ModuleResolver maintains state for a specific parse session:
 * - Module path resolution cache
 * - Parsed module cache
 * - Circular import detection stack
 * 
 * @design_decision Single Responsibility
 *   ModuleResolver handles three things: path resolution, module caching,
 *   and circular import detection. These are all related to "finding and
 *   tracking modules" and belong together.
 * 
 * @design_decision ParserContext holds reference, not ownership
 *   ModuleResolver is owned by the driver (parse session). ParserContext
 *   holds a raw pointer for access. This allows the resolver to outlive
 *   any individual parse call.
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
 * // Resolve import
 * InternedString filePath = resolver.resolveImportPath("std.io");
 * 
 * // Check cache
 * if (resolver.isModuleParsed(filePath)) {
 *     ModuleAST* ast = resolver.getParsedModule(filePath);
 * }
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
     * @param modulePath The resolved module path (e.g., "std/io.luc")
     * @param ast The parsed AST (owned by the session's arena)
     */
    void cacheModule(InternedString modulePath, ModuleAST* ast);
    
    /**
     * @brief Get all modules in dependency order (post-order).
     * 
     * A module's path is appended when it finishes parsing, after all
     * its dependencies. This means the order is safe for single-pass
     * semantic analysis.
     */
    const std::vector<InternedString>& getModuleOrder() const { return moduleOrder_; }
    
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
    
private:
    std::filesystem::path packageRoot_;
    StringPool& pool_;
    
    // ─── Internal State ──────────────────────────────────────────────────
    
    // Map from import path (e.g., "std.io") to resolved file path (e.g., "std/io.luc")
    std::unordered_map<InternedString, InternedString> importPathToFile_;
    
    // Map from resolved file path to parsed AST
    std::unordered_map<InternedString, ModuleAST*> parsedModules_;
    
    // Paths in order they were parsed (post-order / dependency order)
    std::vector<InternedString> moduleOrder_;
    
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