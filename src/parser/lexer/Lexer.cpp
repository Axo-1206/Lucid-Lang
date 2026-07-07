/**
 * @file Lexer.cpp
 */

#include "Lexer.hpp"
#include <cctype>
#include <unordered_map>

namespace lexer {

// ─── Internal State ──────────────────────────────────────────────────

namespace detail {

struct LexerState {
    const std::string& source;
    std::string filename;
    size_t position;
    int line;
    int column;
    bool hadErrors = false;
    
    LexerState(const std::string& src, const std::string& file)
        : source(src), filename(file), position(0), line(1), column(1) {}
};

// ─── Character Helpers ──────────────────────────────────────────────

inline bool is_identifier_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_identifier_char(char c) {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool is_bin_digit(char c) {
    return c == '0' || c == '1';
}

inline bool is_oct_digit(char c) {
    return c >= '0' && c <= '7';
}

inline bool is_ascii(char c) {
    return static_cast<unsigned char>(c) <= 0x7F;
}

inline bool is_at_end(const LexerState& state) {
    return state.position >= state.source.length();
}

inline char current_char(const LexerState& state) {
    if (is_at_end(state)) return '\0';
    return state.source[state.position];
}

inline char peek_char(const LexerState& state, int offset = 0) {
    size_t pos = state.position + offset;
    if (pos >= state.source.length()) return '\0';
    return state.source[pos];
}

inline void advance(LexerState& state) {
    if (is_at_end(state)) return;
    if (current_char(state) == '\n') {
        state.line++;
        state.column = 1;
    } else {
        state.column++;
    }
    state.position++;
}

inline bool match(LexerState& state, char expected) {
    if (is_at_end(state)) return false;
    if (current_char(state) != expected) return false;
    advance(state);
    return true;
}

inline bool match_two(LexerState& state, char first, char second) {
    if (is_at_end(state)) return false;
    if (current_char(state) != first) return false;
    if (peek_char(state, 1) != second) return false;
    advance(state);
    advance(state);
    return true;
}

inline Token make_token(TokenType type, const std::string& value, 
                        const LexerState& state) {
    return Token{type, value, state.line, state.column};
}

inline Token error_token(const std::string& message, const LexerState& state) {
    return Token{TokenType::UNKNOWN, message, state.line, state.column};
}

// ─── Skip Whitespace ─────────────────────────────────────────────────

void skip_whitespace(LexerState& state) {
    while (!is_at_end(state)) {
        char c = current_char(state);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(state);
        } else if (c == '\n') {
            advance(state);
        } else {
            break;
        }
    }
}

// ─── Lex Identifier ──────────────────────────────────────────────────

Token lex_identifier(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    while (!is_at_end(state) && is_identifier_char(current_char(state))) {
        value += current_char(state);
        advance(state);
    }
    
    TokenType type = keyword_to_type(value);
    if (type != TokenType::IDENTIFIER) {
        return Token{type, value, start_line, start_col};
    }
    
    return Token{TokenType::IDENTIFIER, value, start_line, start_col};
}

// ─── Lex Number ──────────────────────────────────────────────────────

Token lex_number(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    char c = current_char(state);
    
    // Hexadecimal: 0x...
    if (c == '0' && (peek_char(state, 1) == 'x' || peek_char(state, 1) == 'X')) {
        advance(state);
        advance(state);
        value = "0x";
        
        if (!is_hex_digit(current_char(state))) {
            return error_token("Invalid hex literal: expected hex digit", state);
        }
        
        while (!is_at_end(state) && is_hex_digit(current_char(state))) {
            value += current_char(state);
            advance(state);
        }
        return Token{TokenType::HEX_LITERAL, value, start_line, start_col};
    }
    
    // Binary: 0b...
    if (c == '0' && (peek_char(state, 1) == 'b' || peek_char(state, 1) == 'B')) {
        advance(state);
        advance(state);
        value = "0b";
        
        if (!is_bin_digit(current_char(state))) {
            return error_token("Invalid binary literal: expected 0 or 1", state);
        }
        
        while (!is_at_end(state) && is_bin_digit(current_char(state))) {
            value += current_char(state);
            advance(state);
        }
        return Token{TokenType::BINARY_LITERAL, value, start_line, start_col};
    }
    
    // Octal: 0o...
    if (c == '0' && (peek_char(state, 1) == 'o' || peek_char(state, 1) == 'O')) {
        advance(state);
        advance(state);
        value = "0o";
        
        if (!is_oct_digit(current_char(state))) {
            return error_token("Invalid octal literal: expected octal digit (0-7)", state);
        }
        
        while (!is_at_end(state) && is_oct_digit(current_char(state))) {
            value += current_char(state);
            advance(state);
        }
        return Token{TokenType::INT_LITERAL, value, start_line, start_col};
    }
    
    // Decimal integer or float
    bool is_float = false;
    
    // Integer part
    while (!is_at_end(state) && is_digit(current_char(state))) {
        value += current_char(state);
        advance(state);
    }
    
    // Fractional part
    if (current_char(state) == '.' && is_digit(peek_char(state, 1))) {
        is_float = true;
        value += current_char(state);
        advance(state);
        
        while (!is_at_end(state) && is_digit(current_char(state))) {
            value += current_char(state);
            advance(state);
        }
    }
    
    // Exponent part
    if (current_char(state) == 'e' || current_char(state) == 'E') {
        is_float = true;
        value += current_char(state);
        advance(state);
        
        if (current_char(state) == '+' || current_char(state) == '-') {
            value += current_char(state);
            advance(state);
        }
        
        if (!is_digit(current_char(state))) {
            return error_token("Invalid float literal: expected digit after exponent", state);
        }
        
        while (!is_at_end(state) && is_digit(current_char(state))) {
            value += current_char(state);
            advance(state);
        }
    }
    
    if (is_float) {
        return Token{TokenType::FLOAT_LITERAL, value, start_line, start_col};
    }
    
    return Token{TokenType::INT_LITERAL, value, start_line, start_col};
}

// ─── Lex Strings ─────────────────────────────────────────────────────

Token lex_string(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    advance(state); // consume opening "
    
    while (!is_at_end(state) && current_char(state) != '"') {
        char c = current_char(state);
        
        // Handle interpolation: \(expr)
        if (c == '\\' && peek_char(state, 1) == '(') {
            value += c;
            value += peek_char(state, 1);
            advance(state);
            advance(state);
            
            // Parse expression inside interpolation
            int paren_count = 1;
            while (!is_at_end(state) && paren_count > 0) {
                char ch = current_char(state);
                if (ch == '(') paren_count++;
                if (ch == ')') paren_count--;
                value += ch;
                advance(state);
            }
            continue;
        }
        
        // Handle escape sequences
        if (c == '\\') {
            char next = peek_char(state, 1);
            switch (next) {
                case 'n': value += '\n'; advance(state); break;
                case 't': value += '\t'; advance(state); break;
                case 'r': value += '\r'; advance(state); break;
                case '\\': value += '\\'; advance(state); break;
                case '"': value += '"'; advance(state); break;
                case '0': value += '\0'; advance(state); break;
                default:
                    return error_token("Invalid escape sequence: \\" + std::string(1, next), state);
            }
            advance(state);
            continue;
        }
        
        // Check for unescaped newline in string
        if (c == '\n') {
            return error_token("Unterminated string: newline not allowed (use \"\"\" for multiline)", state);
        }
        
        // Any character is valid inside a string (including non-ASCII)
        value += c;
        advance(state);
    }
    
    if (is_at_end(state)) {
        return error_token("Unterminated string literal", state);
    }
    
    advance(state); // consume closing "
    return Token{TokenType::STRING_LITERAL, value, start_line, start_col};
}

// ─── Lex Raw Strings ─────────────────────────────────────────────────

Token lex_raw_string(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    // Consume """
    advance(state);
    advance(state);
    advance(state);
    
    while (!is_at_end(state)) {
        // Check for closing """
        if (current_char(state) == '"' && peek_char(state, 1) == '"' && peek_char(state, 2) == '"') {
            advance(state);
            advance(state);
            advance(state);
            return Token{TokenType::RAW_STRING_LITERAL, value, start_line, start_col};
        }
        
        // Raw strings accept ANY character (including non-ASCII)
        value += current_char(state);
        advance(state);
    }
    
    return error_token("Unterminated raw string literal (expected \"\"\")", state);
}

// ─── Lex Character ──────────────────────────────────────────────────

Token lex_char(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    advance(state); // consume opening '
    
    if (is_at_end(state)) {
        return error_token("Unterminated character literal", state);
    }
    
    char c = current_char(state);
    
    // Handle escape sequences
    if (c == '\\') {
        advance(state);
        if (is_at_end(state)) {
            return error_token("Unterminated character literal", state);
        }
        
        char next = current_char(state);
        switch (next) {
            case 'n': value = "\\n"; break;
            case 't': value = "\\t"; break;
            case 'r': value = "\\r"; break;
            case '\\': value = "\\\\"; break;
            case '\'': value = "\\'"; break;
            case '0': value = "\\0"; break;
            default:
                return error_token("Invalid escape sequence: \\" + std::string(1, next), state);
        }
        advance(state);
    } else {
        value = c;
        advance(state);
    }
    
    if (is_at_end(state) || current_char(state) != '\'') {
        return error_token("Unterminated character literal", state);
    }
    
    advance(state); // consume closing '
    return Token{TokenType::CHAR_LITERAL, value, start_line, start_col};
}

// ─── Lex Comments ────────────────────────────────────────────────────

Token lex_comment(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    // Consume /-
    advance(state);
    advance(state);
    
    while (!is_at_end(state)) {
        if (current_char(state) == '-' && peek_char(state, 1) == '/') {
            advance(state);
            advance(state);
            return Token{TokenType::LINE_COMMENT, value, start_line, start_col};
        }
        
        value += current_char(state);
        advance(state);
    }
    
    return error_token("Unterminated block comment (expected -/)", state);
}

// ─── Lex Doc Comments ────────────────────────────────────────────────

Token lex_doc_comment(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    
    // Consume /--
    advance(state);
    advance(state);
    advance(state);
    
    while (!is_at_end(state)) {
        if (current_char(state) == '-' && peek_char(state, 1) == '-' && peek_char(state, 2) == '/') {
            advance(state);
            advance(state);
            advance(state);
            return Token{TokenType::DOC_COMMENT, value, start_line, start_col};
        }
        
        value += current_char(state);
        advance(state);
    }
    
    return error_token("Unterminated documentation comment (expected --/)", state);
}

// ─── Lex Block Comments ────────────────────────────────────────────────

Token lex_block_comment(LexerState& state) {
    std::string value;
    int start_line = state.line;
    int start_col = state.column;
    int nesting_depth = 1;  // We're already inside one /- ...
    
    // Consume /-
    advance(state);
    advance(state);
    
    while (!is_at_end(state) && nesting_depth > 0) {
        char c = current_char(state);
        
        // Check for nested block comment start: /-
        if (c == '/' && peek_char(state, 1) == '-') {
            nesting_depth++;
            value += c;
            value += peek_char(state, 1);
            advance(state);
            advance(state);
            continue;
        }
        
        // Check for block comment end: -/
        if (c == '-' && peek_char(state, 1) == '/') {
            nesting_depth--;
            if (nesting_depth > 0) {
                value += c;
                value += peek_char(state, 1);
                advance(state);
                advance(state);
            } else {
                // This is the closing of the outermost comment
                // Don't add it to value (we want the content only)
                advance(state);
                advance(state);
            }
            continue;
        }
        
        value += c;
        advance(state);
    }
    
    if (nesting_depth > 0) {
        return error_token("Unterminated block comment (expected -/)", state);
    }
    
    return Token{TokenType::BLOCK_COMMENT, value, start_line, start_col};
}

// ─── Lex Operators and Punctuation ──────────────────────────────────

Token lex_operator_or_punctuation(LexerState& state) {
    int start_line = state.line;
    int start_col = state.column;
    char c = current_char(state);
    
    // ─── Two-character operators ────────────────────────────────────
    
    // Exponentiation: ** and **=
    if (c == '*') {
        if (peek_char(state, 1) == '*') {
            advance(state);
            if (peek_char(state, 1) == '=') {
                advance(state);
                return Token{TokenType::POW_ASSIGN, "**=", start_line, start_col};
            }
            advance(state);
            return Token{TokenType::POW, "**", start_line, start_col};
        }
    }
    
    // Bitwise shift: <<, <<=, >>, >>=
    if (c == '<' && peek_char(state, 1) == '<') {
        advance(state);
        if (peek_char(state, 1) == '=') {
            advance(state);
            return Token{TokenType::SHL_ASSIGN, "<<=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::SHL, "<<", start_line, start_col};
    }
    
    if (c == '>' && peek_char(state, 1) == '>') {
        advance(state);
        if (peek_char(state, 1) == '=') {
            advance(state);
            return Token{TokenType::SHR_ASSIGN, ">>=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::SHR, ">>", start_line, start_col};
    }
    
    // Ranges: .. and ..<
    if (c == '.' && peek_char(state, 1) == '.') {
        advance(state);
        if (peek_char(state, 1) == '<') {
            advance(state);
            return Token{TokenType::RANGE_EXCLUSIVE, "..<", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::RANGE, "..", start_line, start_col};
    }
    
    // Composition: +>
    if (c == '+' && peek_char(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::COMPOSE, "+>", start_line, start_col};
    }
    
    // Pipeline: |>
    if (c == '|' && peek_char(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::PIPELINE, "|>", start_line, start_col};
    }
    
    // Nullable access: ?.
    if (c == '?' && peek_char(state, 1) == '.') {
        advance(state);
        advance(state);
        return Token{TokenType::QUESTION_DOT, "?.", start_line, start_col};
    }
    
    // Nullable/fallback: ??
    if (c == '?' && peek_char(state, 1) == '?') {
        advance(state);
        advance(state);
        return Token{TokenType::QUESTION_QUESTION, "??", start_line, start_col};
    }
    
    // Function arrow: ->
    if (c == '-' && peek_char(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::ARROW, "->", start_line, start_col};
    }
    
    // ─── Single-character operators ─────────────────────────────────
    
    if (c == '=') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::EQUAL_EQUAL, "==", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::ASSIGN, "=", start_line, start_col};
    }
    
    if (c == '+') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::PLUS_ASSIGN, "+=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::PLUS, "+", start_line, start_col};
    }
    
    if (c == '-') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MINUS_ASSIGN, "-=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::MINUS, "-", start_line, start_col};
    }
    
    if (c == '*') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MUL_ASSIGN, "*=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::MUL, "*", start_line, start_col};
    }
    
    if (c == '/') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::DIV_ASSIGN, "/=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::DIV, "/", start_line, start_col};
    }
    
    if (c == '%') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MOD_ASSIGN, "%=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::MOD, "%", start_line, start_col};
    }
    
    if (c == '^') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_XOR_ASSIGN, "^=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::BIT_XOR, "^", start_line, start_col};
    }
    
    if (c == '&') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_AND_ASSIGN, "&=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::BIT_AND, "&", start_line, start_col};
    }
    
    if (c == '|') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_OR_ASSIGN, "|=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::BIT_OR, "|", start_line, start_col};
    }
    
    if (c == '~') {
        advance(state);
        return Token{TokenType::BIT_NOT, "~", start_line, start_col};
    }
    
    if (c == '!') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::NOT_EQUAL, "!=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::BANG, "!", start_line, start_col};
    }
    
    if (c == '<') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::LESS_EQUAL, "<=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::LESS, "<", start_line, start_col};
    }
    
    if (c == '>') {
        if (peek_char(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::GREATER_EQUAL, ">=", start_line, start_col};
        }
        advance(state);
        return Token{TokenType::GREATER, ">", start_line, start_col};
    }
    
    // ─── Punctuation ─────────────────────────────────────────────────
    
    if (c == '.') {
        advance(state);
        return Token{TokenType::DOT, ".", start_line, start_col};
    }
    
    if (c == ':') {
        advance(state);
        return Token{TokenType::COLON, ":", start_line, start_col};
    }
    
    if (c == ',') {
        advance(state);
        return Token{TokenType::COMMA, ",", start_line, start_col};
    }
    
    if (c == ';') {
        advance(state);
        return Token{TokenType::SEMICOLON, ";", start_line, start_col};
    }
    
    if (c == '(') {
        advance(state);
        return Token{TokenType::LPAREN, "(", start_line, start_col};
    }
    
    if (c == ')') {
        advance(state);
        return Token{TokenType::RPAREN, ")", start_line, start_col};
    }
    
    if (c == '{') {
        advance(state);
        return Token{TokenType::LBRACE, "{", start_line, start_col};
    }
    
    if (c == '}') {
        advance(state);
        return Token{TokenType::RBRACE, "}", start_line, start_col};
    }
    
    if (c == '[') {
        advance(state);
        return Token{TokenType::LBRACKET, "[", start_line, start_col};
    }
    
    if (c == ']') {
        advance(state);
        return Token{TokenType::RBRACKET, "]", start_line, start_col};
    }
    
    if (c == '@') {
        advance(state);
        return Token{TokenType::AT_SIGN, "@", start_line, start_col};
    }
    
    if (c == '#') {
        advance(state);
        return Token{TokenType::HASH, "#", start_line, start_col};
    }
    
    if (c == '_') {
        advance(state);
        return Token{TokenType::UNDERSCORE, "_", start_line, start_col};
    }
    
    // Variadic: ...
    if (c == '.' && peek_char(state, 1) == '.' && peek_char(state, 2) == '.') {
        advance(state);
        advance(state);
        advance(state);
        return Token{TokenType::VARIADIC, "...", start_line, start_col};
    }
    
    // Unknown character
    std::string msg = "Unexpected character: '";
    msg += c;
    msg += "'";
    advance(state);
    return Token{TokenType::UNKNOWN, msg, start_line, start_col};
}

// ─── Main Tokenization Loop ─────────────────────────────────────────

Token next_token(detail::LexerState& state) {
    detail::skip_whitespace(state);
    
    if (detail::is_at_end(state)) {
        return Token{TokenType::EOF_TOKEN, "", state.line, state.column};
    }
    
    char c = detail::current_char(state);
    
    // Comments
    if (c == '/' && detail::peek_char(state, 1) == '-') {
        // Check for doc comment: /--
        if (detail::peek_char(state, 2) == '-') {
            return detail::lex_doc_comment(state);
        }
        // Block comment: /- (not followed by another -)
        // Note: Doc comment check must come first, then block comment
        return detail::lex_block_comment(state);
    }
    
    // Identifiers and keywords
    if (detail::is_identifier_start(c)) {
        return detail::lex_identifier(state);
    }
    
    // Numbers
    if (detail::is_digit(c) || (c == '.' && detail::is_digit(detail::peek_char(state, 1)))) {
        return detail::lex_number(state);
    }
    
    // Strings
    if (c == '"') {
        if (detail::peek_char(state, 1) == '"' && detail::peek_char(state, 2) == '"') {
            return detail::lex_raw_string(state);
        }
        return detail::lex_string(state);
    }
    
    // Characters
    if (c == '\'') {
        return detail::lex_char(state);
    }
    
    // Operators and punctuation
    return detail::lex_operator_or_punctuation(state);
}

} // namespace detail

// ─── Public API Implementation ──────────────────────────────────────

std::vector<Token> tokenize(const std::string& source, const std::string& filename) {
    detail::LexerState state(source, filename);
    std::vector<Token> tokens;
    
    while (true) {
        Token token = detail::next_token(state);
        tokens.push_back(token);
        if (token.type == TokenType::EOF_TOKEN) break;
    }
    
    return tokens;
}

std::vector<Token> tokenize_n(const std::string& source, size_t max_tokens, 
                               const std::string& filename) {
    detail::LexerState state(source, filename);
    std::vector<Token> tokens;
    
    for (size_t i = 0; i < max_tokens; ++i) {
        Token token = detail::next_token(state);
        tokens.push_back(token);
        if (token.type == TokenType::EOF_TOKEN) break;
    }
    
    return tokens;
}

} // namespace lexer