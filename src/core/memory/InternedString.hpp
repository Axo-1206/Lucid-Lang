/// @file InternedString.hpp
/// @brief A 32-bit handle to a string owned by a StringPool.
///
/// InternedString replaces std::string for every name, identifier, and
/// string literal in the AST. Instead of duplicating "Vec2" in every node
/// that mentions it, every occurrence holds a 4-byte index into a pool that
/// owns one canonical copy.
///
/// ─── Why this exists ──────────────────────────────────────────────────────
///   • Memory. A std::string is 32+ bytes. This is 4. For the most common
///     fields in the AST (names, type names, field names), that is an 8x
///     reduction.
///   • Comparison. `a == b` is one integer comparison, not a string
///     comparison.
///   • Hashing. Trivial: hash the uint32_t.
///   • Arena compatibility. No destructor, no heap allocation. Safe to
///     store inside arena-allocated AST nodes with no bookkeeping.
///
/// ─── ID 0 is reserved ─────────────────────────────────────────────────────
/// A default-constructed InternedString has id 0, which means "empty or
/// invalid". A StringPool never returns ID 0 for a non-empty string.
/// `isValid()` is true for any non-zero ID.
///
/// ─── No text access ───────────────────────────────────────────────────────
/// There is deliberately no method that recovers the string. Recovering
/// text requires a StringPool&, and requiring the caller to name it makes
/// the dependency visible at every call site that needs display text. A
/// method that reached a global pool would hide that dependency and let
/// text be recovered from a pool that is not the one that interned the ID.
///
/// ─── Usage ────────────────────────────────────────────────────────────────
///   • Parser: `pool.intern(tokenText)` on every identifier, keyword, and
///     string literal, and store the result in the AST.
///   • Sema / bytecode compiler: `pool.lookupView(id)` when you need text.
///   • Comparison: `==`, which compares IDs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct InternedString {
    uint32_t id = 0;

    InternedString() = default;
    explicit InternedString(uint32_t id) : id(id) {}

    bool operator==(InternedString o) const noexcept { return id == o.id; }
    bool operator!=(InternedString o) const noexcept { return id != o.id; }
    bool operator<(InternedString o)  const noexcept { return id < o.id; }

    /// True for any string that was actually interned.
    bool isValid() const noexcept { return id != 0; }

    /// True if this is the empty / invalid handle.
    bool isEmpty() const noexcept { return id == 0; }
};

// ─── std::hash ──────────────────────────────────────────────────────────────
//
// Specializing std::hash for a user type is legal regardless of the type's
// namespace. Defined here, in the header, so any translation unit that
// includes InternedString.hpp can use it in an unordered_map.

namespace std {
    template <>
    struct hash<InternedString> {
        size_t operator()(const InternedString& s) const noexcept {
            return hash<uint32_t>{}(s.id);
        }
    };
}