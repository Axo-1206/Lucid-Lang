/// @file core/JSONFormatter.hpp
/// @brief JSON pretty-printing formatter (like VS Code's Ctrl+S).
///
/// This module provides a formatter that takes a raw JSON string and
/// formats it with proper indentation, similar to VS Code's built-in
/// JSON formatter (which uses the jsonc-parser library).
///
/// The formatter handles:
///   - Proper indentation for nested objects and arrays
///   - Space after colons
///   - Newlines after commas
///   - Preserves string contents (doesn't format inside strings)
///   - Handles escaped characters correctly
///
/// Usage:
///   std::string raw = "{\"name\":\"test\",\"age\":42}";
///   std::string pretty = JSONFormatter::format(raw, 2);
///
/// Output:
///   {
///     "name": "test",
///     "age": 42
///   }

#pragma once

#include <string>

/**
 * @brief JSON pretty-printing formatter.
 * 
 * This formatter mimics VS Code's JSON formatting behavior (Ctrl+S),
 * which is powered by the jsonc-parser library.
 */
class JSONFormatter {
public:
    /**
     * @brief Format a raw JSON string with proper indentation.
     * 
     * @param input The raw JSON string (can be minified or already formatted)
     * @param tabSize Number of spaces per indentation level (default: 2)
     * @return std::string The formatted JSON string
     * 
     * @example
     *   // Input: {"name":"test","age":42,"items":["a","b"]}
     *   // Output:
     *   // {
     *   //   "name": "test",
     *   //   "age": 42,
     *   //   "items": [
     *   //     "a",
     *   //     "b"
     *   //   ]
     *   // }
     */
    static std::string format(const std::string& input, int tabSize = 2);

private:
    /**
     * @brief Check if a character is whitespace (space, tab, newline, etc.)
     */
    static bool isWhitespace(char c);

    /**
     * @brief Check if a character is a digit (0-9)
     */
    static bool isDigit(char c);

    /**
     * @brief Check if a character is a letter (a-z, A-Z)
     */
    static bool isLetter(char c);
};
