#pragma once

#include "InternedString.hpp"
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

/**
 * @brief Owns all interned string data for a compilation session.
 *
 * StringPool is the canonical store for every identifier, keyword, and string
 * literal seen during parsing. It maps arbitrary `std::string_view` inputs
 * to stable `InternedString` IDs (thin 32-bit handles) that can be copied
 * cheaply into AST nodes.
 *
 * @architectural_note Singleton Design
 *   StringPool is implemented as a singleton with static `instance()` and
 *   `initialize()` methods. This eliminates the need to pass StringPool
 *   references through every function in the diagnostic system and other
 *   subsystems that need string lookup capabilities.
 *
 *   The singleton is initialized once at program startup with the real pool,
 *   and can be re-initialized for testing with a fresh pool.
 *
 * @architectural_note Single-threaded, on purpose
 *   Like the diagnostic system, StringPool is not thread-safe by design.
 *   The compiler pipeline is strictly sequential, so this is deliberate.
 */
class StringPool {
public:
    // ─── Singleton Access ────────────────────────────────────────────────

    /**
     * @brief Get the singleton instance.
     * 
     * Must be initialized with `initialize()` before first use.
     * 
     * @return StringPool& Reference to the singleton instance.
     */
    static StringPool& instance() {
        static StringPool pool;
        return pool;
    }

    /**
     * @brief Initialize the singleton with a new pool instance.
     * 
     * This is called once at program startup. For testing, it can be
     * called multiple times to reset the pool between test runs.
     * 
     * @return StringPool& Reference to the initialized pool.
     */
    static StringPool& initialize() {
        // Reset the static pool by reusing the existing instance
        // and clearing its state.
        StringPool& pool = instance();
        pool.clear();
        return pool;
    }

    // ─── Instance Methods ─────────────────────────────────────────────────

    StringPool();
    ~StringPool() = default;

    // Disallow copy/move to ensure stable references
    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    /**
     * @brief Intern a string, returning a stable ID.
     * 
     * - Empty string → ID 0
     * - Already interned → existing ID
     * - New string → allocated, given the next sequential ID
     */
    InternedString intern(std::string_view s);

    /**
     * @brief Look up an interned string by ID.
     * 
     * @param s The interned string ID.
     * @return std::string The string data (allocated copy).
     * 
     * @note Returns empty string for ID 0 or invalid IDs.
     */
    std::string lookup(InternedString s) const;

    /**
     * @brief Look up an interned string as a view.
     * 
     * This is useful for internal operations that don't need an allocation.
     * Most callers should use `lookup()` which returns a std::string.
     * 
     * @param s The interned string ID.
     * @return std::string_view The string view (valid as long as the pool lives).
     */
    std::string_view lookupView(InternedString s) const;

    /**
     * @brief Clear all interned strings.
     * 
     * For testing only. Resets the pool to its initial state.
     */
    void clear();

private:
    // ─── Internal ────────────────────────────────────────────────────────

    std::string_view allocateString(std::string_view s);

    // ─── Members ─────────────────────────────────────────────────────────

    std::unordered_map<std::string_view, uint32_t> internMap;
    std::vector<std::string_view> strings; // Maps ID -> string_view

    // Bump allocator for string data
    std::vector<std::unique_ptr<char[]>> blocks;
    char* currentBlock = nullptr;
    size_t currentOffset = 0;
    static constexpr size_t BLOCK_SIZE = 64 * 1024; // 64 KB blocks
};

// ─── Convenience Global Functions ────────────────────────────────────────

/**
 * @brief Convenience function: intern a string.
 * 
 * Equivalent to `StringPool::instance().intern(s)`.
 */
inline InternedString internString(std::string_view s) {
    return StringPool::instance().intern(s);
}

/**
 * @brief Convenience function: lookup an interned string.
 * 
 * Equivalent to `StringPool::instance().lookup(id)`.
 */
inline std::string lookupString(InternedString id) {
    return StringPool::instance().lookup(id);
}

/**
 * @brief Convenience function: lookup an interned string as a view.
 * 
 * Equivalent to `StringPool::instance().lookupView(id)`.
 */
inline std::string_view lookupStringView(InternedString id) {
    return StringPool::instance().lookupView(id);
}