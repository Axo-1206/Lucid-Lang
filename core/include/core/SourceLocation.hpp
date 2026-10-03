/// @file SourceLocation.hpp
/// @brief Source location tracking for the compiler.
///
/// SourceLocation is a position in a source file, packed into 32 bits for
/// cheap storage in every AST node and every token. File identity is not
/// stored per location; the diagnostic engine tracks the current file, and
/// the AST root carries the file path.
///
/// ─── Bit layout ───────────────────────────────────────────────────────────
///   Bits 0-11:  column (0-4095)
///   Bits 12-31: line   (0-1,048,575)
///
/// 1-indexed: line 1, column 1 is the first character of the file. A value
/// of 0 means "unknown location" and is what a default-constructed
/// SourceLocation holds.
///
/// ─── Limits ───────────────────────────────────────────────────────────────
/// A line past 2^20 or a column past 2^12 is not representable. Lines that
/// long do not occur in hand-written source; columns that wide do, in
/// generated or minified files. The column is clamped to 4095 rather than
/// silently wrapped, so a diagnostic that lands past the limit reads as
/// "far to the right" instead of pointing at an unrelated column.
///
/// In debug builds, a line that exceeds the limit fires an assertion. A
/// column that exceeds the limit does not: the clamp is the intended
/// behavior, and the assertion would fire on correct code.

#pragma once

#include <cstdint>
#include <cassert>
#include <ostream>

/// @brief A position in a source file, packed into 32 bits.
struct SourceLocation {
    /// Packed line and column. Zero means unknown.
    uint32_t value = 0;

    static constexpr uint32_t kColumnBits = 12;
    static constexpr uint32_t kColumnMask = (1u << kColumnBits) - 1u;
    static constexpr uint32_t kMaxLine   = (1u << (32 - kColumnBits)) - 1u;
    static constexpr uint32_t kMaxColumn = kColumnMask;

    SourceLocation() = default;

    SourceLocation(uint32_t line, uint32_t column) {
        assert(line <= kMaxLine &&
               "SourceLocation: line exceeds 20-bit limit");
        const uint32_t col =
            column > kMaxColumn ? kMaxColumn : column;
        value = (line << kColumnBits) | (col & kColumnMask);
    }

    uint32_t line()   const noexcept { return value >> kColumnBits; }
    uint32_t column() const noexcept { return value & kColumnMask;  }
    bool isKnown()    const noexcept { return value > 0; }

    bool operator==(const SourceLocation& o) const noexcept {
        return value == o.value;
    }
    bool operator!=(const SourceLocation& o) const noexcept {
        return value != o.value;
    }
};

// ─── Streaming ──────────────────────────────────────────────────────────────
//
// The diagnostic formatter streams a SourceLocation directly. The format is
// "line:column" for a known location, "<unknown location>" for a zero.
//
// There is deliberately no `SourceLocation::toString()` member. A caller
// that wants the string can build it from `line()` and `column()`, and
// only that caller pays for <string>.

inline std::ostream& operator<<(std::ostream& os, const SourceLocation& loc) {
    if (loc.isKnown()) {
        os << loc.line() << ':' << loc.column();
    } else {
        os << "<unknown location>";
    }
    return os;
}