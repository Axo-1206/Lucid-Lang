#pragma once

#include "InternedString.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

/**
 * @brief Owns all interned string data for a compilation session.
 *
 * StringPool is the canonical store for every identifier, keyword, and string
 * literal seen during parsing. It maps arbitrary `std::string_view` inputs
 * to stable `InternedString` IDs (thin 32-bit handles) that can be copied
 * cheaply into AST nodes.
 *
 * ## Why this exists
 *
 * Without interning, every AST node that says "Vec2" stores its own copy of
 * that 4-byte string (plus 32+ bytes of std::string overhead). With thousands
 * of nodes, that memory waste adds up. Interning also makes equality checks
 * O(1) integer comparisons instead of O(n) string comparisons — critical for
 * symbol resolution, trait conformance checks, and type equality.
 *
 * ## Design
 *
 * - **Bump allocator**: string characters are allocated from 64 KB blocks.
 *   No per-string `malloc`/`free` — fast allocation and single-shot cleanup
 *   when the pool is destroyed.
 * - **Deduplication**: `intern()` checks a hash map before allocating. The
 *   same string passed twice returns the same ID.
 * - **Stable references**: the pool is never copied or moved, so raw
 *   `std::string_view` pointers into its blocks remain valid for the pool's
 *   lifetime.
 * - **Session-scoped**: one pool per CompilerSession, shared by all files in
 *   a compilation. This ensures the same name in different files gets the
 *   same ID — essential for cross-file symbol resolution.
 *
 * ## Usage
 *
 * ```cpp
 * // Parsing
 * InternedString name = pool.intern("Vec2");
 *
 * // Later, in codegen or diagnostics
 * std::string_view text = pool.lookup(name);  // "Vec2"
 * assert(text == "Vec2");
 * ```
 *
 * ## Thread safety
 *
 * Not thread-safe by default. Each CompilerSession owns its pool; parallel
 * sessions don't interfere.
 */
 
class StringPool {
    std::unordered_map<std::string_view, uint32_t> internMap;
    std::vector<std::string_view> strings; // Maps ID -> string_view

    // Bump allocator to store the actual string characters contiguously
    std::vector<std::unique_ptr<char[]>> blocks;
    char* currentBlock = nullptr;
    size_t currentOffset = 0;
    static constexpr size_t BLOCK_SIZE = 64 * 1024; // 64 KB blocks

    std::string_view allocateString(std::string_view s);

public:
    StringPool();
    ~StringPool() = default;

    // Disallow copy/move to ensure stable references if needed
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    InternedString intern(std::string_view s);
    
    /**
     * @brief Retrieves the string data for a previously interned ID.
     * 
     * Returns an empty string for ID 0 or any out-of-range ID.
     * 
     * @note This allocates a new std::string each call. For performance-critical
     *       paths, consider keeping the InternedString and only converting
     *       when actually needed (e.g., for diagnostics).
     */
    std::string lookup(InternedString s) const;
};