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
 * - Converting import paths to file paths (e.g., "std.io" → "std/io.lucid")
 * - Caching parsed modules to avoid re-parsing
 * - Detecting circular imports
 * - Managing module search paths
 * 
 * ## Path Resolution Algorithm
 * 
 * 1. Check custom mappings (from build manifest)
 * 2. Check cache for previously resolved paths
 * 3. Convert import path to file path (replace '.' with '/')
 * 4. Try with .lucid extension
 * 5. Search in package root and additional search paths
 * 6. Try without extension
 * 
 * ## Usage Example
 * 
 * ```cpp
 * ModuleResolver resolver(packageRoot, pool);
 * 
 * // Resolve import
 * InternedString filePath = resolver.resolveUsePath("std.io");
 * 
 * // Check cache
 * if (resolver.isModuleParsed(filePath)) {
 *     ModuleAST* ast = resolver.getParsedModule(filePath);
 * }
 * 
 * // Track circular imports — prefer ScopedParsingGuard over calling
 * // pushParsing/popParsing directly; see its own doc comment below.
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
     * @brief Resolve a import path to a file path.
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
     * @brief Check if a import path is valid (resolves to an existing file).
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
     * @return ModuleAST* The parsed AST, or nullptr if not parsed
     */
    ModuleAST* getParsedModule(InternedString modulePath) const;
    
    /**
     * @brief Store a parsed module AST.
     * 
     * On the first cache of a given path, also appends it to the module
     * order (see getModuleOrder()). A later cacheModule() call for the
     * same, already-cached path is a no-op — this only happens if a caller
     * calls parse() directly instead of going through the cache-check at
     * the top of parse(), and is intentionally ignored rather than
     * overwriting or re-ordering an already-completed module.
     * 
     * @param modulePath The resolved module path (e.g., "std/io.lucid")
     * @param ast The parsed AST (owned by the session's arena)
     */
    void cacheModule(InternedString modulePath, ModuleAST* ast);
    
    /**
     * @brief Get every module path in completion (post-)order.
     *
     * A module's path is appended here the first time it is cached — i.e.
     * the moment its own parse() call finishes, which is after every
     * module it `import`s has already finished and been appended. This means
     * the order is a valid dependency order: for any module M, every
     * module M depends on appears before M in this list. In particular,
     * the root/main file — which depends (transitively) on everything
     * else — is always last.
     *
     * This is what the driver should import to get the full set of parsed
     * modules in an order safe for single-pass semantic analysis:
     *
     * ```cpp
     * for (InternedString path : resolver.getModuleOrder()) {
     *     ModuleAST* mod = resolver.getParsedModule(path);
     *     analyze(mod);  // every module `mod` imports has already run
     * }
     * ```
     */
    const std::vector<InternedString>& getModuleOrder() const { return moduleOrder_; }
    
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
    
    // Map from import path (e.g., "std.io") to resolved file path (e.g., "std/io.lucid")
    std::unordered_map<InternedString, InternedString> usePathToFile_;
    
    // Map from resolved file path to parsed AST
    std::unordered_map<InternedString, ModuleAST*> parsedModules_;
    
    // Paths in the order they were first cached (post-order / dependency
    // order — see getModuleOrder()). A parallel index to parsedModules_,
    // kept in sync exclusively by cacheModule().
    std::vector<InternedString> moduleOrder_;
    
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
     * @brief Convert a import path to a relative file path.
     * 
     * @param usePath The import path (e.g., "std.io")
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

/**
 * @brief RAII guard for ModuleResolver's circular-import tracking.
 *
 * Pushes `filePath` onto the resolver's parsing stack on construction and
 * pops it on destruction — on every exit path, including early returns.
 * Without this, pushParsing()/popParsing() must be balanced by hand across
 * every early return in parse() (it currently is, across four separate
 * exit points), and a future exit path that forgets the matching pop would
 * silently corrupt circular-import detection for every file parsed
 * afterward, with no crash to flag it.
 *
 * `resolver` may be null (parsing without a resolver is valid — see
 * parse()'s existing `if (ctx.resolver)` checks); the guard no-ops in
 * that case rather than requiring every call site to branch on it.
 *
 * ## Usage
 *
 * ```cpp
 * if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
 *     // circular import — report and return before constructing the guard,
 *     // since nothing should be pushed for a parse that never starts
 *     return nullptr;
 * }
 * ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
 * // every return below this point pops correctly, automatically
 * ```
 *
 * Non-copyable, non-movable, for the same reason as ScopedContext: its
 * identity is tied to one specific parse() activation.
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