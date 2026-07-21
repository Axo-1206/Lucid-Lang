/**
 * @file DiagnosticSeverity.hpp
 * @brief Numeric severity levels for diagnostics.
 *
 * Uses integer levels compatible with LSP and editor tooling.
 * Higher number = more severe.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace diagnostic {

/**
 * @brief Severity levels as numeric values.
 *
 * Matches LSP's DiagnosticSeverity convention:
 *   1 = Error, 2 = Warning, 3 = Information, 4 = Hint
 *
 * Extended with Fatal (5) for unrecoverable errors.
 */
enum class Severity : uint8_t {
    Hint    = 0,  ///< Informational, no action required
    Note    = 1,  ///< Informational with context
    Warning = 2,  ///< Potential issue, build continues
    Error   = 3,  ///< Issue that prevents correct compilation
    Fatal   = 4,  ///< Unrecoverable, compilation aborts
};

/**
 * @brief Convert Severity to a human-readable string.
 *
 * Returns a string literal (const char*), not std::string_view,
 * so it works seamlessly with std::ostringstream and string concatenation.
 */
constexpr const char* severityName(Severity s) {
    switch (s) {
        case Severity::Hint:    return "HINT";
        case Severity::Note:    return "NOTE";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

/**
 * @brief Convert Severity to a short label (for compact output).
 */
constexpr const char* severityLabel(Severity s) {
    switch (s) {
        case Severity::Hint:    return "H";
        case Severity::Note:    return "N";
        case Severity::Warning: return "W";
        case Severity::Error:   return "E";
        case Severity::Fatal:   return "F";
    }
    return "?";
}

/**
 * @brief Check if a severity is an error (Error or Fatal).
 */
constexpr bool isError(Severity s) {
    return s >= Severity::Error;
}

/**
 * @brief Check if a severity is a warning or above.
 */
constexpr bool isWarningOrAbove(Severity s) {
    return s >= Severity::Warning;
}

} // namespace diagnostic