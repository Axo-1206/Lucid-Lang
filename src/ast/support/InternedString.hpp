#pragma once

#include <cstdint>

/**
 * @brief A lightweight, 32-bit identifier for an interned string.
 *
 * InternedString replaces `std::string` for all names, identifiers, and string
 * literals in the AST. Instead of storing duplicate copies of "Vec2" in every
 * node that references it, every occurrence shares a single canonical copy
 * owned by a StringPool. This identifier is just an index into that pool.
 *
 * ## Why this exists
 *
 * - **Memory**: a `std::string` is 32+ bytes. This is 4 bytes — an 8x reduction
 *   for the most common fields in the AST (names, type names, field names).
 * - **Comparisons**: `a == b` is a single integer comparison, not a string
 *   comparison. Symbol table lookups, pattern matching, and type equality all
 *   become faster.
 * - **Hashing**: trivial — just hash the `uint32_t`. No string hashing needed.
 * - **Arena compatibility**: no destructor, no heap allocation — safe to use
 *   inside arena-allocated AST nodes without leaking memory.
 *
 * ## ID 0 convention
 *
 * ID 0 is reserved for the empty / invalid string. A default-constructed
 * InternedString has id=0. `isValid()` returns true for any non-zero ID.
 * The StringPool never returns ID 0 for a non-empty string.
 *
 * ## Usage
 *
 * - **Parser**: call `pool.intern(tokenText)` on every identifier token and
 *   store the result in AST nodes.
 * - **Semantic pass / codegen**: use `pool.lookup(id)` to recover the
 *   `std::string_view` when you need display text or LLVM IR names.
 * - **Comparison**: just use `==` — it compares the ID.
 *
 * @note InternedString intentionally has no `str()` method. This forces every
 *       subsystem to explicitly accept a StringPool&, making the dependency
 *       visible and avoiding hidden global state.
 */
struct InternedString {
    uint32_t id = 0;

    InternedString() = default;
    explicit InternedString(uint32_t id) : id(id) {}

    bool operator==(InternedString other) const { return id == other.id; }
    bool operator!=(InternedString other) const { return id != other.id; }

    /** True for any string that was actually interned (id != 0). */
    bool isValid() const { return id != 0; }
};