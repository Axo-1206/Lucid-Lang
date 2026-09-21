/// @file StringRuntime.cpp
/// @brief Implementation of string operation runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" entry points for string operations
/// that are called by JIT-compiled and AOT-compiled Lucid code.
///
/// ─── String Layout ──────────────────────────────────────────────────────────
/// A Lucid string is `lucid::abi::LucidString` from `runtime-abi/lucid_abi.h`:
/// `{ data, len, cap }`.
///   - data: pointer to UTF-8 encoded bytes
///   - len:  length in bytes (not characters)
///   - cap:  capacity in bytes; 0 means static (do not free)
///
/// This file does not define its own copy of the struct. A private copy is
/// how the runtime and the ABI header drifted apart (`ptr` vs `data`).
///
/// ─── By-Pointer Convention ──────────────────────────────────────────────────
/// Every function takes its `LucidString` parameters by pointer (tag
/// `StrPtr`); see functions.def, rule 1. The out-parameter is always the
/// first argument. Functions that produce a string write it through that
/// pointer; the caller owns the slot and, once a heap string is written into
/// it, owns the buffer. On failure the slot is left as `{null, 0, 0}`.
///
/// Inputs are only read. The `StrPtr` tag carries no `const`, so the
/// discipline is code review, not the compiler.
///
/// ─── Ownership ──────────────────────────────────────────────────────────────
/// Buffers come from `lucid::runtime::heapAlloc`, so `Ownership::drop` can
/// release them with `__lucid_free` and the leak report can see them.
/// Every buffer is `len + 1` bytes with a trailing NUL, and `cap == len + 1`.
///
/// ─── Signature Contract ─────────────────────────────────────────────────────
/// This file includes `runtime-abi/lucid_runtime.h`, so each definition below
/// is checked against its row in `functions.def` at compile time.

#include "RuntimeInternal.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using lucid::abi::LucidBool;
using lucid::abi::LucidF64;
using lucid::abi::LucidI32;
using lucid::abi::LucidI64;
using lucid::abi::LucidPtr;
using lucid::abi::LucidString;

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace {

void clearSlot(LucidString* s) {
    s->data = nullptr;
    s->len  = 0;
    s->cap  = 0;
}

/// Allocate a zeroed buffer for a `len`-byte string and publish it in `*out`.
///
/// Returns the writable buffer, or null (leaving `*out` empty) if `len` is
/// negative or the allocation fails. `heapAlloc` zeroes the block, so the
/// trailing NUL is already in place; callers only fill the first `len` bytes.
char* makeBuffer(LucidString* out, LucidI64 len) {
    clearSlot(out);
    if (len < 0 || len == INT64_MAX) {
        return nullptr;
    }
    char* buffer = static_cast<char*>(
        lucid::runtime::heapAlloc(static_cast<std::size_t>(len) + 1));
    if (!buffer) {
        return nullptr;
    }
    out->data = buffer;
    out->len  = len;
    out->cap  = len + 1;
    return buffer;
}

/// Publish a copy of `bytes[0..len)` in `*out`.
void setFromBytes(LucidString* out, const char* bytes, std::size_t len) {
    char* buffer = makeBuffer(out, static_cast<LucidI64>(len));
    if (buffer && len > 0) {
        std::memcpy(buffer, bytes, len);
    }
}

void setFromString(LucidString* out, const std::string& str) {
    setFromBytes(out, str.data(), str.size());
}

} // anonymous namespace

extern "C" {

// ─── String Operations ──────────────────────────────────────────────────────

/// @brief Concatenate two strings.
///
/// Writes the concatenation of `*a` and `*b` into `*out`. `*out` receives a
/// fresh heap allocation that the caller owns. On allocation failure, or a
/// malformed input, `*out` is `{null, 0, 0}`; the caller's ownership layer
/// treats that as an empty static string.
///
/// `out` may alias `a` or `b`: the inputs are read into locals before the
/// slot is touched.
void __lucid_str_concat(LucidString* out, LucidString* a, LucidString* b) {
    if (!out) return;
    if (!a || !b) {
        clearSlot(out);
        return;
    }

    const LucidString lhs = *a;
    const LucidString rhs = *b;

    // The result needs `len + 1` bytes, so the sum must stay below INT64_MAX.
    if (lhs.len < 0 || rhs.len < 0 || lhs.len > INT64_MAX - 1 - rhs.len) {
        clearSlot(out);
        return;
    }

    char* buffer = makeBuffer(out, lhs.len + rhs.len);
    if (!buffer) {
        return;
    }
    if (lhs.data && lhs.len > 0) {
        std::memcpy(buffer, lhs.data, static_cast<std::size_t>(lhs.len));
    }
    if (rhs.data && rhs.len > 0) {
        std::memcpy(buffer + lhs.len, rhs.data, static_cast<std::size_t>(rhs.len));
    }
}

/// @brief Extract a substring.
///
/// Writes `s[from..to)` into `*out`. The result is a fresh heap allocation
/// that the caller owns.
///
/// If `from < 0`, `from > to`, or `to > s->len`, the out-slot is left as
/// `{null, 0, 0}`. The caller's emitter performs the bounds check before
/// calling, so an out-of-range call here is a codegen bug; the safe default
/// keeps the runtime robust in case it happens.
void __lucid_str_slice(LucidString* out, LucidString* s, LucidI64 from, LucidI64 to) {
    if (!out) return;
    if (!s) {
        clearSlot(out);
        return;
    }

    const LucidString src = *s;

    if (from < 0 || from > to || to > src.len) {
        clearSlot(out);
        return;
    }

    const LucidI64 len = to - from;
    char* buffer = makeBuffer(out, len);
    if (!buffer) {
        return;
    }
    if (src.data && len > 0) {
        std::memcpy(buffer, src.data + from, static_cast<std::size_t>(len));
    }
}

/// @brief Construct a string by copying bytes from a raw pointer.
///
/// Allocates `len + 1` bytes via the one-allocator registry, copies
/// `len` bytes from `ptr`, appends a NUL terminator, and writes the
/// resulting `LucidString` into `*out`. The source buffer at `ptr`
/// remains owned by the caller — this function only reads it.
///
/// ─── Why a Copy ────────────────────────────────────────────────────
/// The caller's buffer may live anywhere: a scope arena, a stack
/// frame, a static C string, or a foreign allocation Lucid cannot
/// manage. Copying into a Lucid-owned buffer is what makes the
/// resulting string self-contained and gives it a normal Lucid
/// lifetime (drop frees the copy, not the source). It's also what the
/// grammar documents: `#str_from_ptr` is described as "compiler
/// copies and validates UTF-8", and the grammar's usage example frees
/// the source buffer immediately after constructing the string.
///
/// ─── Failure ───────────────────────────────────────────────────────
/// A null `out`, a null `ptr`, or a negative/zero `len` leaves the
/// out-slot as `{null, 0, 0}` — an empty static string. CodeGen does
/// not insert a check; the caller passes a valid pointer when
/// `len > 0`.
///
/// TODO(utf8): this function copies bytes verbatim; it does not
/// validate UTF-8. See the functions.def row for the policy question.
void __lucid_str_from_ptr(LucidString* out, LucidPtr ptr, LucidI64 len) {
    if (!out) return;
    if (!ptr || len <= 0) {
        clearSlot(out);
        return;
    }

    char* buffer = makeBuffer(out, len);
    if (!buffer) {
        return;
    }
    std::memcpy(buffer, ptr, static_cast<std::size_t>(len));
}

/// @brief Compare two strings for equality.
///
/// Returns 1 if equal, 0 otherwise. Length-then-bytes comparison; no
/// allocation.
LucidBool __lucid_str_eq(LucidString* a, LucidString* b) {
    if (!a || !b) {
        // Null pointers are equal only if both are null.
        return (a == b) ? 1 : 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    if (a->data == b->data) {
        return 1;  // Same pointer (or both null with equal length)
    }
    if (a->len <= 0) {
        return 1;  // Both empty
    }
    if (!a->data || !b->data) {
        // Same non-zero length but one has no data: a broken string.
        return 0;
    }
    return std::memcmp(a->data, b->data, static_cast<std::size_t>(a->len)) == 0 ? 1 : 0;
}

// ─── Formatters (#tostr / #ptrstr) ──────────────────────────────────────────

/// @brief Format a pointer as a hex string.
///
/// Writes "0x" and 16 lowercase hex digits into `*out`. The caller owns the buffer.
void __lucid_ptr_to_hex_string(LucidString* out, LucidPtr ptr) {
    if (!out) return;

    char text[2 + 16 + 1];
    std::snprintf(text, sizeof(text), "0x%016llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ptr)));
    setFromBytes(out, text, std::strlen(text));
}

/// @brief Convert a boolean to a string.
///
/// Writes "true" or "false" into `*out`. Any non-zero byte is true.
void __lucid_bool_to_str(LucidString* out, LucidBool b) {
    if (!out) return;

    const char* text = b ? "true" : "false";
    setFromBytes(out, text, std::strlen(text));
}

/// @brief Convert a Unicode codepoint to a string.
///
/// Writes the UTF-8 encoding of `codepoint` into `*out`. A negative or
/// out-of-range codepoint yields the empty string (still a heap allocation,
/// so the caller's drop logic is the same on every path).
void __lucid_char_to_str(LucidString* out, LucidI32 codepoint) {
    if (!out) return;

    // Simple UTF-8 encoding.
    char buffer[4] = {0};
    std::size_t len = 0;
    if (codepoint < 0) {
        len = 0;
    } else if (codepoint < 0x80) {
        buffer[0] = static_cast<char>(codepoint);
        len = 1;
    } else if (codepoint < 0x800) {
        buffer[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        buffer[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        len = 2;
    } else if (codepoint < 0x10000) {
        buffer[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        buffer[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buffer[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        len = 3;
    } else if (codepoint < 0x110000) {
        buffer[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        buffer[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        buffer[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buffer[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        len = 4;
    }
    setFromBytes(out, buffer, len);
}

/// @brief Convert a signed 64-bit integer to a string.
void __lucid_int_to_str(LucidString* out, LucidI64 v) {
    if (!out) return;
    setFromString(out, std::to_string(v));
}

/// @brief Convert an unsigned 64-bit integer to a string.
///
/// The row's tag is `I64` (LLVM integers have no signedness), so the value
/// arrives as its 64 bits in a signed type and is reinterpreted here.
void __lucid_uint_to_str(LucidString* out, LucidI64 v) {
    if (!out) return;
    setFromString(out, std::to_string(static_cast<std::uint64_t>(v)));
}

/// @brief Convert a floating-point value to a string.
void __lucid_float_to_str(LucidString* out, LucidF64 v) {
    if (!out) return;

    std::ostringstream oss;
    oss << v;
    setFromString(out, oss.str());
}

} // extern "C"