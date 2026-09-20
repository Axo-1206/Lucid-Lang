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
///   - cap: capacity in bytes

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>

// ─── String Layout ──────────────────────────────────────────────────────────
// Matches the canonical string type in CodeGenContext::getStringType()
struct LucidString {
    void* ptr;      // Pointer to UTF-8 data
    uint64_t len;   // Length in bytes
    uint64_t cap;   // Capacity in bytes
};

// ─── Helper: Allocate a new string ──────────────────────────────────────────
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

// ─── Helper: Free a string ──────────────────────────────────────────────────
static void freeString(LucidString* str) {
    if (str && str->ptr) {
        std::free(str->ptr);
        str->ptr = nullptr;
        str->len = 0;
        str->cap = 0;
    }
}

// ─── Helper: Ensure string capacity ─────────────────────────────────────────
static bool ensureCapacity(LucidString* str, uint64_t needed) {
    if (str->cap >= needed) {
        return true;
    }
    uint64_t newCap = str->cap * 2;
    if (newCap < needed) newCap = needed;
    void* newPtr = std::realloc(str->ptr, newCap);
    if (!newPtr) {
        return false;
    }
    str->ptr = newPtr;
    str->cap = newCap;
    return true;
}

extern "C" {

// ─── String Operations ──────────────────────────────────────────────────────

/// @brief Concatenate two strings.
/// @param a First string.
/// @param b Second string.
/// @return New string containing a + b.
LucidString __lucid_str_concat(LucidString a, LucidString b) {
    uint64_t totalLen = a.len + b.len;
    LucidString result = allocString("", totalLen);
    if (!result.ptr) {
        return LucidString{nullptr, 0, 0};
    }
    if (a.ptr && a.len > 0) {
        std::memcpy(result.ptr, a.ptr, a.len);
    }
    if (b.ptr && b.len > 0) {
        std::memcpy(static_cast<char*>(result.ptr) + a.len, b.ptr, b.len);
    }
    return result;
}

/// @brief Extract a substring.
/// @param s Input string.
/// @param from Start index (inclusive).
/// @param to End index (exclusive).
/// @return New string containing s[from:to].
LucidString __lucid_str_slice(LucidString s, uint64_t from, uint64_t to) {
    if (from > to || from > s.len || to > s.len) {
        return LucidString{nullptr, 0, 0};
    }
    uint64_t len = to - from;
    LucidString result = allocString("", len);
    if (!result.ptr) {
        return LucidString{nullptr, 0, 0};
    }
    if (s.ptr && len > 0) {
        std::memcpy(result.ptr, static_cast<char*>(s.ptr) + from, len);
    }
    return result;
}

/// @brief Compare two strings for equality.
/// @param a First string.
/// @param b Second string.
/// @return 1 if equal, 0 otherwise.
int __lucid_str_eq(LucidString a, LucidString b) {
    if (a.len != b.len) {
        return 0;
    }
    if (a.ptr == b.ptr) {
        return 1;  // Same pointer
    }
    if (a.len == 0 && b.len == 0) {
        return 1;  // Both empty
    }
    return std::memcmp(a.ptr, b.ptr, a.len) == 0 ? 1 : 0;
}

/// @brief Format a pointer as a hex string.
/// @param ptr Pointer to format.
/// @return String containing "0x00000000..." representation.
LucidString __lucid_ptr_to_hex_string(void* ptr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(16)
        << reinterpret_cast<uintptr_t>(ptr);
    std::string str = oss.str();
    return allocString(str.c_str(), str.length());
}

/// @brief Convert a boolean to a string.
/// @param b Boolean value.
/// @return "true" or "false".
LucidString __lucid_bool_to_str(int b) {
    const char* str = b ? "true" : "false";
    return allocString(str, std::strlen(str));
}

/// @brief Convert a Unicode codepoint to a string.
/// @param codepoint Unicode codepoint (UTF-32).
/// @return String containing the UTF-8 encoded character.
LucidString __lucid_char_to_str(uint32_t codepoint) {
    // Simple UTF-8 encoding
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
    return allocString(buffer, len);
}

/// @brief Convert a signed 64-bit integer to a string.
/// @param v Integer value.
/// @return String representation.
LucidString __lucid_int_to_str(int64_t v) {
    std::string str = std::to_string(v);
    return allocString(str.c_str(), str.length());
}

/// @brief Convert an unsigned 64-bit integer to a string.
/// @param v Unsigned integer value.
/// @return String representation.
LucidString __lucid_uint_to_str(uint64_t v) {
    std::string str = std::to_string(v);
    return allocString(str.c_str(), str.length());
}

/// @brief Convert a floating-point value to a string.
/// @param v Double value.
/// @return String representation.
LucidString __lucid_float_to_str(double v) {
    std::ostringstream oss;
    // Use default formatting for now
    oss << v;
    std::string str = oss.str();
    return allocString(str.c_str(), str.length());
}

} // extern "C"