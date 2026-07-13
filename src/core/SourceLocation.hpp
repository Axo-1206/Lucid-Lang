/**
 * @file SourceLocation.hpp
 * @brief Source location tracking for the compiler.
 * 
 * SourceLocation represents a position in a source file (line and column).
 * It's packed into 32 bits for efficient storage in AST nodes.
 */

#pragma once

#include <cstdint>
#include <ostream>

/**
 * @brief Source location packed into 32 bits (20 bits line, 12 bits column).
 * 
 * File path is stored separately (e.g., in ModuleAST), not per node to save memory.
 * 
 * @par Memory Layout
 *   - Bits 0-11: Column (0-4095)
 *   - Bits 12-31: Line (0-1,048,575)
 * 
 * @note Line and column are 1-indexed (line 1, column 1 is the first character).
 *       Value 0 means unknown/empty location.
 */
struct SourceLocation {
    uint32_t value = 0;  // line in high 20 bits, column in low 12 bits

    SourceLocation() = default;
    SourceLocation(uint32_t line, uint32_t column) {
        value = (line << 12) | (column & 0xFFF);
    }

    uint32_t line()   const { return value >> 12; }
    uint32_t column() const { return value & 0xFFF; }
    bool isKnown()    const { return value > 0; }
};

// ─── Stream operator for SourceLocation ────────────────────────────────────
// Allows SourceLocation to be used with std::ostringstream and operator<<
// in variadic error reporting.
inline std::ostream& operator<<(std::ostream& os, const SourceLocation& loc) {
    if (loc.isKnown()) {
        os << loc.line() << ":" << loc.column();
    } else {
        os << "<unknown location>";
    }
    return os;
}