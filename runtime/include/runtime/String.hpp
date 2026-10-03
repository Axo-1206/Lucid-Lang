/**
 * @file runtime/String.hpp
 *
 * @responsibility A refcounted UTF-8 string. The heap object a
 *                 ValueTag::String Value points at.
 *
 * ─── Design: refcounted, not interned ─────────────────────────────────────
 * Every string value is a StringObject with a refcount. Copying a
 * string (EmitCopy's job) retains; dropping it (EmitDrop's job)
 * releases. When the refcount hits zero the buffer is freed. This is
 * the same model as the host handles, and it is what the RuntimeOp
 * set assumes: Retain / Release operate on a handle-or-string
 * uniformly, CopyString produces a fresh buffer, FreeString releases
 * one.
 *
 * ─── Design: immutable, UTF-8, length-prefixed ────────────────────────────
 * A StringObject is immutable once constructed. Concatenation allocates
 * a new StringObject; it never mutates. The buffer is UTF-8 bytes, not
 * null-terminated (though a null byte is written past the end for
 * convenience when the host wants a C string). The length is in bytes,
 * not code points; Lucid's string type has no character-indexing
 * operation (§8), so a code-point index is never needed.
 *
 * ─── Design: the runtime owns allocation, not the interpreter ─────────────
 * The interpreter calls allocString / retainString / releaseString /
 * copyString / concatString. It does not touch the refcount field
 * directly. This keeps the allocation policy (arena, malloc, pool)
 * a runtime decision, and keeps the interpreter's code free of
 * allocation details.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lucid::runtime {

/// @brief A refcounted UTF-8 string.
///
/// The layout is deliberately simple: a refcount, a byte length, and a
/// pointer to the byte buffer. The buffer is separately allocated so
/// that a StringObject can be small (24 bytes on a 64-bit target) and
/// a string's payload can be any size.
struct StringObject {
    /// The reference count. Managed by retainString / releaseString.
    /// Never zero while the object is alive; reaching zero is the
    /// release that frees the object.
    uint32_t refcount;

    /// The byte length of the string, not counting a trailing null.
    /// (The buffer may still have a null past the end; see below.)
    uint32_t length;

    /// The UTF-8 bytes. Not null-terminated in general; the buffer
    /// may hold a trailing null byte for host convenience, but the
    /// length field is authoritative. Never null for a live string,
    /// even for the empty string (which points at a shared empty
    /// buffer).
    const char* data;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Allocate a new string with the given bytes. The bytes are
///        copied; the caller's buffer is not retained.
///
/// The returned string has refcount 1. The caller owns that reference
/// and must eventually call releaseString.
StringObject* allocString(std::string_view bytes);

/// @brief Allocate a new string by concatenating two strings. The
///        inputs are not modified and their refcounts are unchanged;
///        the result is a fresh StringObject with refcount 1.
StringObject* concatStrings(const StringObject* a, const StringObject* b);

// ─────────────────────────────────────────────────────────────────────────────
// Refcounting
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Increment a string's refcount.
void retainString(StringObject* s) noexcept;

/// @brief Decrement a string's refcount. If it reaches zero, free the
///        buffer and the StringObject.
void releaseString(StringObject* s) noexcept;

/// @brief Allocate a fresh StringObject with the same bytes as `s`,
///        refcount 1. `s` is unchanged.
///
/// This is the runtime implementation of RuntimeOp::CopyString.
StringObject* copyString(const StringObject* s);

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The string's bytes as a string_view. Valid until the
///        string is released.
inline std::string_view stringView(const StringObject* s) noexcept {
    return std::string_view(s->data, s->length);
}

/// @brief Equality: same length, same bytes. This is the runtime
///        implementation of Eq_Str / Ne_Str.
bool stringEquals(const StringObject* a, const StringObject* b) noexcept;

/// @brief Lexicographic ordering (UTF-8 bytes, locale-independent).
///        This is the runtime implementation of Lt_Str / Le_Str /
///        Gt_Str / Ge_Str.
///
/// Returns negative if a < b, zero if equal, positive if a > b.
int stringCompare(const StringObject* a, const StringObject* b) noexcept;

} // namespace lucid::runtime