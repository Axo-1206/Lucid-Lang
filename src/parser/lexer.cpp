/**
 * @file Lexer.hpp
 * 
 * @responsibility Converts source text into a stream of tokens.
 * Pure functions - no state, no side effects.
 * 
 * @design Each token carries its own location information.
 * The lexer is stateless - it just processes input and returns tokens.
 */

#pragma once
#include <string>
#include <vector>
#include "core/Tokens.hpp"

namespace lexer {

// ─── Main Entry Points ──────────────────────────────────────────────────

/**
 * Tokenize a complete source file into a vector of tokens.
 * Each token carries its own line/column/filename information.
 * 
 * @param source The source code to tokenize
 * @param filename The source file name (for error reporting)
 * @return Vector of tokens
 */
std::vector<Token> tokenize(const std::string& source, 
                            const std::string& filename = "<unknown>");

/**
 * Tokenize a source file, but stop after N tokens.
 * Useful for testing or partial parsing.
 */
std::vector<Token> tokenize_n(const std::string& source, 
                              size_t max_tokens,
                              const std::string& filename = "<unknown>");

// ─── Token Stream (For incremental consumption) ──────────────────────

/**
 * A lightweight token stream for incremental parsing.
 * This is the only stateful part, but it's minimal and transparent.
 */
struct TokenStream {
    std::vector<Token> tokens;
    size_t position;
    std::string filename;
    
    TokenStream(const std::string& source, const std::string& file = "<unknown>");
    
    // Get next token (advances position)
    Token next();
    
    // Peek at token without consuming (offset 0 = current, 1 = next, etc.)
    Token peek(int offset = 0) const;
    
    // Check if we've consumed all tokens
    bool is_at_end() const;
    
    // Reset to beginning
    void reset();
    
    // Get current position info
    int current_line() const;
    int current_column() const;
    size_t current_position() const { return position; }
};

} // namespace lexer