/**
 * @file Lexer.hpp
 * 
 * @responsibility Converts source text into tokens.
 * Pure functions with no state - called by TokenStream.
 * 
 * @design_decision Lexer is a pure function, not a class
 *   It takes source and returns tokens. No state is retained.
 *   This makes it easy to test and reason about.
 * 
 * @design_decision Errors are reported via DiagnosticEngine
 *   The lexer reports errors directly to the diagnostic system.
 *   Unknown characters are skipped and parsing continues.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include <string>
#include <vector>

namespace lexer {

/**
 * @brief Tokenize a source file into a vector of tokens.
 * 
 * This is a pure function - it takes source and returns tokens.
 * Errors are reported via the DiagnosticEngine.
 * 
 * @param source The source code to tokenize
 * @param diagnostics The diagnostic engine for error reporting
 * @return std::vector<Token> The token stream
 */
std::vector<Token> tokenize(const std::string& source, 
                            DiagnosticEngine& diagnostics);

/**
 * @brief Tokenize a source file, stopping after N tokens.
 * 
 * @param source The source code to tokenize
 * @param max_tokens Maximum number of tokens to produce
 * @param diagnostics The diagnostic engine for error reporting
 * @return std::vector<Token> The token stream (up to max_tokens)
 */
std::vector<Token> tokenize_n(const std::string& source, 
                              size_t max_tokens,
                              DiagnosticEngine& diagnostics);

/**
 * @brief Check if a character is a valid identifier start.
 * 
 * @param c The character to check
 * @return true if the character can start an identifier
 */
inline bool isIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * @brief Check if a character is a valid identifier character.
 * 
 * @param c The character to check
 * @return true if the character can be part of an identifier
 */
inline bool isIdentifierChar(char c) {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

/**
 * @brief Check if a character is a digit.
 */
inline bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

/**
 * @brief Check if a character is a hex digit.
 */
inline bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * @brief Check if a character is a binary digit.
 */
inline bool isBinDigit(char c) {
    return c == '0' || c == '1';
}

/**
 * @brief Check if a character is an octal digit.
 */
inline bool isOctDigit(char c) {
    return c >= '0' && c <= '7';
}

} // namespace lexer