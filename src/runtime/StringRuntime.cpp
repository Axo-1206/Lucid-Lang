/// @file StringRuntime.cpp
/// @brief Implementation of string operation runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" entry points for string operations
/// that are called by JIT-compiled and AOT-compiled Lucid code.
///
/// ─── String Layout ──────────────────────────────────────────────────────────
/// A Lucid string is a 3-field struct: { ptr, len, cap } where:
///   - ptr: pointer to UTF-8 encoded data on the heap
///   - len: length in bytes (not characters)
///   - cap: capacity in bytes; 0 means static (do not free)
///
/// ─── By-Pointer Convention ──────────────────────────────────────────────────
/// Every function in this file takes its `LucidString` parameters by
/// pointer. See the "By-Pointer Convention" section of
/// runtime-abi/functions.def for why: LLVM and the platform C ABI disagree
/// about passing a 24-byte struct by value, and the disagreement is silent.
///
/// The out-parameter is always the first argument. Functions that produce
/// a `LucidString` write the result through it; the caller allocates the
/// slot and passes its address. Functions that only read take their inputs
/// by pointer and never modify them, even though the C++ type system does
/// not mark them `const` — the by-pointer tags in `functions.def` do not
/// carry qualifiers, so the discipline is code review, not the compiler.
///
/// ─── Signature Contract ─────────────────────────────────────────────────────
/// The signatures in this file are checked against `functions.def` by
/// `runtime/exports.cpp`. If a signature here does not match the
/// corresponding row's tag expansion, the compiler reports a conflicting
/// declaration at build time. Changing a signature here without changing
/// the row (or vice versa) fails the build.

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>

// ─── String Layout ──────────────────────────────────────────────────────────
// Matches the canonical string type in lucid_abi.h
// (`lucid::abi::LucidString`) and in codegen/Types.cpp's `stringType()`.
//
// This definition is a duplicate of the one in lucid_abi.h. It is here so
// that this file does not need to include lucid_abi.h, which would pull in
// the C++ static_assert machinery and the ABI's constexpr helpers. In
// practice the two definitions must have identical layout, and the
// static_asserts in lucid_abi.h are what catch a divergence.
struct LucidString {
    void*    ptr;    // Pointer to UTF-8 data
    uint64_t len;    // Length in bytes
    uint64_t cap;    // Capacity in bytes; 0 == static (do not free)
};

// ─── Helper: Allocate a new string ──────────────────────────────────────────
//
// Returns a LucidString with a fresh heap buffer of `len` bytes (plus the
// null terminator) and `cap == len + 1`. The caller owns the buffer; it
// will be freed by the ownership layer when the string's binding dies.
static LucidString allocString(const char* data, uint64_t len) {
    LucidString result;
    result.len = len;
    result.cap = len + 1;  // +1 for null terminator
    result.ptr = std::malloc(result.cap);
    if (result.ptr) {
        std::memcpy(result.ptr, data, len);
        static_cast<char*>(result.ptr)[len] = '\0';
    }
    return result;
}

extern "C" {

// ─── String Operations ──────────────────────────────────────────────────────

/// @brief Concatenate two strings.
///
/// Writes the concatenation of `*a` and `*b` into `*out`. `*out` receives
/// a fresh heap allocation that the caller owns. On allocation failure,
/// `out->ptr` is null and `out->len` is 0; the caller's ownership layer
/// treats a null-`ptr` string as an empty string.
void __lucid_str_concat(LucidString* out,
                        LucidString* a,
                        LucidString* b) {
    if (!out) return;

    // Initialize the out-slot to a safe default.
    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    if (!a || !b) return;

    uint64_t totalLen = a->len + b->len;
    LucidString result = allocString("", totalLen);
    if (!result.ptr) {
        return;
    }
    if (a->ptr && a->len > 0) {
        std::memcpy(result.ptr, a->ptr, a->len);
    }
    if (b->ptr && b->len > 0) {
        std::memcpy(static_cast<char*>(result.ptr) + a->len, b->ptr, b->len);
    }
    *out = result;
}

/// @brief Extract a substring.
///
/// Writes `s[from..to)` into `*out`. The result is a fresh heap allocation
/// that the caller owns.
///
/// If `from > to`, `from > s->len`, or `to > s->len`, the out-slot is left
/// as an empty string. The caller's emitter performs the bounds check
/// before calling, so an out-of-range call here is a codegen bug; the safe
/// default keeps the runtime robust in case it happens.
void __lucid_str_slice(LucidString* out,
                       LucidString* s,
                       uint64_t from,
                       uint64_t to) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    if (!s) return;

    if (from > to || from > s->len || to > s->len) {
        return;
    }
    uint64_t len = to - from;
    LucidString result = allocString("", len);
    if (!result.ptr) {
        return;
    }
    if (s->ptr && len > 0) {
        std::memcpy(result.ptr, static_cast<const char*>(s->ptr) + from, len);
    }
    *out = result;
}

/// @brief Compare two strings for equality.
///
/// Returns 1 if equal, 0 otherwise. Length-then-bytes comparison; no
/// allocation.
int __lucid_str_eq(LucidString* a, LucidString* b) {
    if (!a || !b) {
        // Null pointers are equal only if both are null.
        return (a == b) ? 1 : 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    if (a->ptr == b->ptr) {
        return 1;  // Same pointer
    }
    if (a->len == 0 && b->len == 0) {
        return 1;  // Both empty
    }
    if (!a->ptr || !b->ptr) {
        // One has data, the other doesn't. Lengths matched (both 0) or
        // one is null-but-nonzero, which is a broken string. Not equal.
        return 0;
    }
    return std::memcmp(a->ptr, b->ptr, a->len) == 0 ? 1 : 0;
}

// ─── Formatters (#tostr / #ptrstr) ──────────────────────────────────────────

/// @brief Format a pointer as a hex string.
///
/// Writes "0x00000000..." into `*out`. The caller owns the buffer.
void __lucid_ptr_to_hex_string(LucidString* out, void* ptr) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(16)
        << reinterpret_cast<uintptr_t>(ptr);
    std::string str = oss.str();
    *out = allocString(str.c_str(), str.length());
}

/// @brief Convert a boolean to a string.
///
/// Writes "true" or "false" into `*out`.
void __lucid_bool_to_str(LucidString* out, uint8_t b) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    const char* str = b ? "true" : "false";
    *out = allocString(str, std::strlen(str));
}

/// @brief Convert a Unicode codepoint to a string.
///
/// Writes the UTF-8 encoding of `codepoint` into `*out`.
void __lucid_char_to_str(LucidString* out, uint32_t codepoint) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    // Simple UTF-8 encoding.
    char buffer[5] = {0};
    int len = 0;
    if (codepoint < 0x80) {
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
    *out = allocString(buffer, len);
}

/// @brief Convert a signed 64-bit integer to a string.
void __lucid_int_to_str(LucidString* out, int64_t v) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    std::string str = std::to_string(v);
    *out = allocString(str.c_str(), str.length());
}

/// @brief Convert an unsigned 64-bit integer to a string.
void __lucid_uint_to_str(LucidString* out, uint64_t v) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    std::string str = std::to_string(v);
    *out = allocString(str.c_str(), str.length());
}

/// @brief Convert a floating-point value to a string.
void __lucid_float_to_str(LucidString* out, double v) {
    if (!out) return;

    out->ptr = nullptr;
    out->len = 0;
    out->cap = 0;

    std::ostringstream oss;
    oss << v;
    std::string str = oss.str();
    *out = allocString(str.c_str(), str.length());
}

} // extern "C"