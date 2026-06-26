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
 * ## Ownership
 * 
 * ModuleResolver is owned by the CompilerSession (or ParseSession).
 * It is passed as a dependency to ParserState via pointer/reference.
 * 
 * ## Usage
 * 
 * ```cpp
 * // In CompilerSession:
 * ModuleResolver resolver(packageRoot, pool);
 * 
 * // Pass to ParserState:
 * ParserState state(stream, path, pool, arena);
 * state.moduleResolver = &resolver;
 * 
 * // Or pass via constructor (preferred):
 * ParserState state(stream, path, pool, arena, resolver);
 * ```
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


// ```
// ┌────────────────────────────────────────────────────────────────────────────┐
// │                         PARSE SESSION                                      │
// │                                                                            │
// │  ┌─────────────────────────────────────────────────────────────────────┐   │
// │  │                        MODULE RESOLVER                              │   │
// │  │                                                                     │   │
// │  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐   │   │
// │  │  │  usePathToFile_  │  │  parsedModules_  │  │  parsingStack_   │   │   │
// │  │  │  (Path Cache)    │  │  (AST Cache)     │  │  (Circular       │   │   │
// │  │  │                  │  │                  │  │   Detection)     │   │   │
// │  │  │  "std.io" →      │  │  "std/io.lucid"  │  │  [ "main.lucid"  │   │   │
// │  │  │   "std/io.lucid" │  │   → ProgramAST   │  │    "std/io.lucid"│   │   │
// │  │  │                  │  │                  │  │    "math.lucid"  │   │   │
// │  │  │  "math" →        │  │  "math.lucid"    │  │  ]               │   │   │
// │  │  │   "math.lucid"   │  │   → ProgramAST   │  │                  │   │   │
// │  │  └──────────────────┘  └──────────────────┘  └──────────────────┘   │   │
// │  │                                                                     │   │
// │  │  ┌─────────────────────────────────────────────────────────────┐    │   │
// │  │  │              PATH RESOLUTION ENGINE                         │    │   │
// │  │  │                                                             │    │   │
// │  │  │  1. Custom Mappings → 2. Cache → 3. Package Root →          │    │   │
// │  │  │  4. Search Paths → 5. Not Found                             │    │   │
// │  │  └─────────────────────────────────────────────────────────────┘    │   │
// │  └─────────────────────────────────────────────────────────────────────┘   │
// │                                    │                                       │
// │                                    ▼                                       │
// │  ┌─────────────────────────────────────────────────────────────────────┐   │
// │  │                      PARSER STATE                                   │   │
// │  │                                                                     │   │
// │  │  ┌─────────────────────────────────────────────────────────────┐    │   │
// │  │  │  importModule(usePath) →                                    │    │   │
// │  │  │    1. Check importedModules (local cache)                   │    │   │
// │  │  │    2. Call importCallback → ModuleResolver                  │    │   │
// │  │  │    3. Parse file if needed                                  │    │   │
// │  │  │    4. Cache result                                          │    │   │
// │  │  └─────────────────────────────────────────────────────────────┘    │   │
// │  └─────────────────────────────────────────────────────────────────────┘   │
// └────────────────────────────────────────────────────────────────────────────┘
// ```

// ## Data Flow: Import Resolution

// ```
//                     ┌─────────────────────┐
//                     │   PARSER ENCOUNTERS │
//                     │   "use std.io"      │
//                     └──────────┬──────────┘
//                                │
//                                ▼
//                     ┌─────────────────────┐
//                     │  ParserState:       │
//                     │  importModule()     │
//                     └──────────┬──────────┘
//                                │
//           ┌────────────────────┼────────────────────┐
//           │                    │                    │
//           ▼                    ▼                    ▼
// ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
// │ 1. Check Local  │ │ 2. Use Callback │ │ 3. Use Resolver │
// │    Cache        │ │    (if set)     │ │    (if set)     │
// │                 │ │                 │ │                 │
// │ importedModules │ │ importCallback  │ │ moduleResolver  │
// │ [usePath] → AST │ │ (usePath) → AST │ │ → resolveUsePath│
// └─────────────────┘ └─────────────────┘ └─────────────────┘
// ```

// ## Detailed Resolution Flow

// ### Step 1: `resolveUsePath(usePath)`

// ```
//                     ┌─────────────────────┐
//                     │  resolveUsePath()   │
//                     │  Input: "std.io"    │
//                     └──────────┬──────────┘
//                                │
//                                ▼
//                     ┌─────────────────────┐
//                     │ 1. Check Custom     │
//                     │    Mappings         │
//                     │                     │
//                     │ customMappings_     │
//                     │ ["std.io"] → ?      │
//                     └──────────┬──────────┘
//                                │
//                     ┌──────────▼──────────┐
//                     │ Found?              │
//                     └──────────┬──────────┘
//                                │
//               ┌────────────────┼────────────────┐
//               │ YES            │ NO             │
//               ▼                ▼                │
//     ┌─────────────────┐ ┌─────────────────────┐ │
//     │ Return cached   │ │ 2. Check Path       │ │
//     │ file path       │ │    Cache            │ │
//     │                 │ │                     │ │
//     │ "std/io.lucid"  │ │ usePathToFile_      │ │
//     └─────────────────┘ │ ["std.io"] → ?      │ │
//                         └──────────┬──────────┘ │
//                                    │            │
//                         ┌──────────▼──────────┐ │
//                         │ Found?              │ │
//                         └──────────┬──────────┘ │
//                                    │            │
//               ┌────────────────────┼────────────┘│
//               │ YES                │ NO          │
//               ▼                    ▼             │
//     ┌─────────────────┐ ┌─────────────────────┐  │
//     │ Return cached   │ │ 3. Convert to       │  │
//     │ file path       │ │    Relative Path    │  │
//     │                 │ │                     │  │
//     │ "std/io.lucid"  │ │ "std.io" →          │  │
//     └─────────────────┘ │ "std/io.lucid"      │  │
//                         └──────────┬──────────┘  │
//                                    │             │
//                         ┌──────────▼──────────┐  │
//                         │ 4. Search for File  │  │
//                         │    in Search Paths  │  │
//                         │                     │  │
//                         │ packageRoot/        │  │
//                         │ searchPaths_        │  │
//                         └──────────┬──────────┘  │
//                                    │             │
//                         ┌──────────▼──────────┐  │
//                         │ Found?              │  │
//                         └──────────┬──────────┘  │
//                                    │             │
//               ┌────────────────────┼─────────────┘
//               │ YES                │ NO
//               ▼                    ▼
//     ┌─────────────────┐ ┌─────────────────────┐
//     │ Cache & Return  │ │ 5. Try Without      │
//     │                 │ │    .lucid Extension │
//     │ "std/io.lucid"  │ │                     │
//     └─────────────────┘ │ "std/io"            │
//                         └──────────┬──────────┘
//                                    │
//                         ┌──────────▼──────────┐
//                         │ Found?              │
//                         └──────────┬──────────┘
//                                    │
//               ┌────────────────────┼─────────────┐
//               │ YES                │ NO          │
//               ▼                    ▼             │
//     ┌─────────────────┐ ┌─────────────────────┐  │
//     │ Cache & Return  │ │ 6. Return Empty     │  │
//     │                 │ │    (Not Found)      │  │
//     │ "std/io"        │ │                     │  │
//     └─────────────────┘ │ InternedString()    │  │
//                         └─────────────────────┘  │
// ```

// ## Cyclic Dependency Detection (The Critical Part)

// ### The Problem: Circular Imports

// ```
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │                          CIRCULAR IMPORT EXAMPLE                            │
// │                                                                             │
// │   file1.lucid                    file2.lucid                    file3.lucid │
// │   ┌─────────────┐               ┌─────────────┐               ┌────────────┐│
// │   │ use file2   │──────────────►│ use file3   │──────────────►│ use file1  ││
// │   └─────────────┘               └─────────────┘               └────────────┘│
// │         │                              │                              │     │
// │         └──────────────────────────────┴──────────────────────────────┘     │
// │                                    │                                        │
// │                                    ▼                                        │
// │                    ┌─────────────────────────────┐                          │
// │                    │   CIRCULAR DEPENDENCY!       │                         │
// │                    │   file1 → file2 → file3 →    │                         │
// │                    │   file1 (infinite loop)      │                         │
// │                    └─────────────────────────────┘                          │
// └─────────────────────────────────────────────────────────────────────────────┘
// ```

// ### How `parsingStack_` Detects This

// ```
//                     ┌─────────────────────────────────────────┐
//                     │         PARSING STACK (LIFO)            │
//                     │                                         │
//                     │  [ "file1.lucid" ]                      │
//                     │       │                                 │
//                     │       ▼                                 │
//                     │  [ "file1.lucid", "file2.lucid" ]       │
//                     │       │                                 │
//                     │       ▼                                 │
//                     │  [ "file1.lucid", "file2.lucid",        │
//                     │    "file3.lucid" ]                      │
//                     │       │                                 │
//                     │       ▼                                 │
//                     │ [WARNING] Attempt to push "file1.lucid" │
//                     │     → Already in stack!                 │
//                     │     → CIRCULAR IMPORT DETECTED!         │
//                     └─────────────────────────────────────────┘
// ```

// ### Step-by-Step: Parsing with Circular Detection

// ```
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  SCENARIO: Parsing file1.lucid that imports file2.lucid                     │
// └─────────────────────────────────────────────────────────────────────────────┘

// STEP 1: Start parsing file1.lucid
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  pushParsing("file1.lucid")                                                 │
// │  parsingStack_ = [ "file1.lucid" ]                                          │
// │  parse file1.lucid → encounters "use file2"                                 │
// └─────────────────────────────────────────────────────────────────────────────┘

// STEP 2: Import file2.lucid
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  isParsing("file2.lucid")?                                                  │
// │  Check: Is "file2.lucid" in [ "file1.lucid" ]?                              │
// │  Result: NO → Safe to parse                                                 │
// │                                                                             │
// │  pushParsing("file2.lucid")                                                 │
// │  parsingStack_ = [ "file1.lucid", "file2.lucid" ]                           │
// │  parse file2.lucid → encounters "use file3"                                 │
// └─────────────────────────────────────────────────────────────────────────────┘

// STEP 3: Import file3.lucid
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  isParsing("file3.lucid")?                                                  │
// │  Check: Is "file3.lucid" in [ "file1.lucid", "file2.lucid" ]?               │
// │  Result: NO → Safe to parse                                                 │
// │                                                                             │
// │  pushParsing("file3.lucid")                                                 │
// │  parsingStack_ = [ "file1.lucid", "file2.lucid", "file3.lucid" ]            │
// │  parse file3.lucid → encounters "use file1"                                 │
// └─────────────────────────────────────────────────────────────────────────────┘

// STEP 4: Attempt to import file1.lucid (CIRCULAR!)
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  isParsing("file1.lucid")?                                                  │
// │  Check: Is "file1.lucid" in                                                 │
// │         [ "file1.lucid", "file2.lucid", "file3.lucid" ]?                    │
// │  Result: YES → CIRCULAR IMPORT DETECTED!                                    │
// │                                                                             │
// │  [ERROR]: "Circular import detected: file1.lucid"                           │
// │  Do NOT push to stack                                                       │
// │  Do NOT parse file (would infinite loop)                                    │
// └─────────────────────────────────────────────────────────────────────────────┘

// STEP 5: Unwind the stack (pop as parsing completes)
// ┌─────────────────────────────────────────────────────────────────────────────┐
// │  After parsing file3.lucid: popParsing()                                    │
// │  parsingStack_ = [ "file1.lucid", "file2.lucid" ]                           │
// │                                                                             │
// │  After parsing file2.lucid: popParsing()                                    │
// │  parsingStack_ = [ "file1.lucid" ]                                          │
// │                                                                             │
// │  After parsing file1.lucid: popParsing()                                    │
// │  parsingStack_ = [ ]                                                        │
// └─────────────────────────────────────────────────────────────────────────────┘
// ```

// ## Complete Data Flow Diagram

// ```
// ┌────────────────────────────────────────────────────────────────────────────┐
// │                    MODULE RESOLVER DATA FLOW                               │
// │                                                                            │
// │  ┌─────────────────────────────────────────────────────────────────────┐   │
// │  │                    RESOLVE USE PATH FLOW                            │   │
// │  │                                                                     │   │
// │  │  usePath ──► resolveUsePath() ──► filePath                          │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 1. Custom   │    │ 4. Check    │                    │   │
// │  │      │        │   Mappings  │    │   Cache     │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 2. Convert  │    │ 5. Cache    │                    │   │
// │  │      │        │   to Path   │    │   Result    │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 3. Search   │    │ 6. Return   │                    │   │
// │  │      │        │   Paths     │    │   Result    │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  │      │              │                                               │   │
// │  │      │              ▼                                               │   │
// │  │      │        ┌─────────────┐                                       │   │
// │  │      │        │ 7. Not      │                                       │   │
// │  │      │        │   Found     │                                       │   │
// │  │      │        └─────────────┘                                       │   │
// │  └─────────────────────────────────────────────────────────────────────┘   │
// │                                                                            │
// │  ┌─────────────────────────────────────────────────────────────────────┐   │
// │  │                    PARSING & CACHING FLOW                           │   │
// │  │                                                                     │   │
// │  │  filePath ──► getModuleFilePath() ──► fullPath                      │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 1. Check    │    │ 4. Return   │                    │   │
// │  │      │        │   Cache     │    │   Path      │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 2. Build    │    │ 5. Parse    │                    │   │
// │  │      │        │   Path      │    │   File      │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  │      │              │                    │                          │   │
// │  │      │              ▼                    ▼                          │   │
// │  │      │        ┌─────────────┐    ┌─────────────┐                    │   │
// │  │      │        │ 3. Cache    │    │ 6. Cache    │                    │   │
// │  │      │        │   Path      │    │   AST       │                    │   │
// │  │      │        └─────────────┘    └─────────────┘                    │   │
// │  └─────────────────────────────────────────────────────────────────────┘   │
// └────────────────────────────────────────────────────────────────────────────┘
// ```

// ## State Transitions During Import

// ```
// ┌────────────────────────────────────────────────────────────────────────────┐
// │                    STATE TRANSITIONS                                       │
// │                                                                            │
// │  ┌─────────────────────────────────────────────────────────────────────┐   │
// │  │                    MODULE STATES                                    │   │
// │  │                                                                     │   │
// │  │  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐           │   │
// │  │  │   UNKNOWN    │───►│  RESOLVING   │───►│   PARSING    │           │   │
// │  │  │              │    │              │    │              │           │   │
// │  │  │ Not in cache │    │ pushed to    │    │ AST being    │           │   │
// │  │  │ Not resolved │    │ parsingStack │    │ constructed  │           │   │
// │  │  └──────────────┘    └──────────────┘    └──────────────┘           │   │
// │  │         │                   │                   │                   │   │
// │  │         │                   │                   │                   │   │
// │  │         │                   ▼                   ▼                   │   │
// │  │         │            ┌─────────────────────────────────┐            │   │
// │  │         │            │     CIRCULAR DETECTED           │            │   │
// │  │         │            │                                 │            │   │
// │  │         │            │ Module already in parsingStack  │            │   │
// │  │         │            │ → ERROR: Circular Import        │            │   │
// │  │         │            └─────────────────────────────────┘            │   │
// │  │         │                                              │            │   │
// │  │         │                                              ▼            │   │
// │  │         │                                    ┌──────────────┐       │   │
// │  │         │                                    │   PARSED     │       │   │
// │  │         │                                    │              │       │   │
// │  │         │                                    │ AST cached   │       │   │
// │  │         │                                    │ popped from  │       │   │
// │  │         │                                    │ parsingStack │       │   │
// │  │         │                                    └──────────────┘       │   │
// │  └─────────────────────────────────────────────────────────────────────┘   │
// └────────────────────────────────────────────────────────────────────────────┘
// ```

// ## Key Implementation Notes

// ### 1. When to Check for Circular Imports

// ```cpp
// // In ParserState::importModule():
// ProgramAST* importModule(InternedString usePath) {
//     // 1. Resolve path
//     InternedString filePath = moduleResolver->resolveUsePath(usePath);
//     if (!filePath.isValid()) {
//         error("Module '", usePath, "' not found");
//         return nullptr;
//     }
    
//     // 2. CRITICAL: Check circular import BEFORE parsing
//     if (moduleResolver->isParsing(filePath)) {
//         error("Circular import detected: '", usePath, "'");
//         return nullptr;
//     }
    
//     // 3. Check cache
//     ProgramAST* cached = moduleResolver->getParsedModule(filePath);
//     if (cached) return cached;
    
//     // 4. Push to stack, parse, pop, cache
//     moduleResolver->pushParsing(filePath);
//     ProgramAST* ast = parseFile(filePath);
//     moduleResolver->popParsing();
//     if (ast) {
//         moduleResolver->cacheModule(filePath, ast);
//     }
//     return ast;
// }
// ```

// ### 2. Stack Management Rules

// | Operation | When | Purpose |
// |-----------|------|---------|
// | `pushParsing()` | Before parsing a module | Mark module as "currently parsing" |
// | `isParsing()` | Before importing | Check if module is already in stack |
// | `popParsing()` | After parsing completes | Remove module from stack |
// | Stack is LIFO | Always | Matches recursion depth |

// ### 3. Cache Lookup Order

// 1. **Local Cache** (`importedModules`) - File-local cache
// 2. **Module Cache** (`parsedModules_`) - Global cache across files
// 3. **Path Cache** (`usePathToFile_`) - Resolved path cache
// 4. **Filesystem** - Last resort, check disk

// ## Summary

// | Component | Purpose | Data Stored |
// |-----------|---------|-------------|
// | `usePathToFile_` | Path resolution cache | usePath → filePath |
// | `parsedModules_` | AST cache | filePath → ProgramAST* |
// | `parsingStack_` | Circular detection | Currently parsing modules (LIFO) |
// | `resolvedPathCache_` | Filesystem path cache | filePath → full filesystem path |
// | `customMappings_` | Build manifest overrides | usePath → filePath |