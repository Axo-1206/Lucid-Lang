/**
 * @file Lexer.hpp
 * 
 * @responsibility Converts source text into a stream of tokens.
 *
 * @fundamental The lexer is the first stage of the compiler/interpreter.
 * It reads source code character by character and produces tokens for the parser.
 */

#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

#include "core/tokens.hpp"

// ─── Lexer Class ──────────────────────────────────────────────────────

class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename = "<unknown>");
    
    // Get the next token from the source
    Token next_token();
    
    // Get all tokens (useful for testing)
    std::vector<Token> tokenize_all();
    
    // Peek at the next token without consuming it
    Token peek_token();
    
    // Get current position
    int get_line() const { return line; }
    int get_column() const { return column; }
    
    // Reset lexer state
    void reset(const std::string& new_source);

private:
    // ─── Source Management ──────────────────────────────────────────
    std::string source;
    std::string filename;
    size_t position;
    int line;
    int column;
    size_t token_start;  // Start of current token for error reporting
    
    // ─── Character Helpers ──────────────────────────────────────────
    char current_char() const;
    char peek_char(int offset = 0) const;
    void advance();
    void skip_whitespace();
    bool is_at_end() const;
    
    // ─── Token Creation ─────────────────────────────────────────────
    Token make_token(TokenType type, const std::string& value);
    Token error_token(const std::string& message);
    
    // ─── Lexer Functions ────────────────────────────────────────────
    Token lex_identifier();
    Token lex_number();
    Token lex_string();
    Token lex_raw_string();
    Token lex_char();
    Token lex_comment();
    Token lex_doc_comment();
    Token lex_operator_or_punctuation();
    
    // ─── Lookahead Helpers ──────────────────────────────────────────
    bool match(char expected);
    bool match_two(char first, char second);
    bool is_identifier_start(char c) const;
    bool is_identifier_char(char c) const;
    bool is_digit(char c) const;
    bool is_hex_digit(char c) const;
    bool is_bin_digit(char c) const;
    bool is_oct_digit(char c) const;
};

// ─── Implementation ──────────────────────────────────────────────────

Lexer::Lexer(const std::string& source, const std::string& filename)
    : source(source)
    , filename(filename)
    , position(0)
    , line(1)
    , column(1)
    , token_start(0) {}

void Lexer::reset(const std::string& new_source) {
    source = new_source;
    position = 0;
    line = 1;
    column = 1;
    token_start = 0;
}

char Lexer::current_char() const {
    if (is_at_end()) return '\0';
    return source[position];
}

char Lexer::peek_char(int offset) const {
    size_t peek_pos = position + offset;
    if (peek_pos >= source.length()) return '\0';
    return source[peek_pos];
}

void Lexer::advance() {
    if (is_at_end()) return;
    if (current_char() == '\n') {
        line++;
        column = 1;
    } else {
        column++;
    }
    position++;
}

bool Lexer::is_at_end() const {
    return position >= source.length();
}

bool Lexer::match(char expected) {
    if (is_at_end()) return false;
    if (current_char() != expected) return false;
    advance();
    return true;
}

bool Lexer::match_two(char first, char second) {
    if (is_at_end()) return false;
    if (current_char() != first) return false;
    if (peek_char(1) != second) return false;
    advance();
    advance();
    return true;
}

Token Lexer::make_token(TokenType type, const std::string& value) {
    return Token{type, value, line, column, filename};
}

Token Lexer::error_token(const std::string& message) {
    return Token{TokenType::UNKNOWN, message, line, column, filename};
}

// ─── Character Class Checks ─────────────────────────────────────────

bool Lexer::is_identifier_start(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::is_identifier_char(char c) const {
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

bool Lexer::is_digit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::is_hex_digit(char c) const {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool Lexer::is_bin_digit(char c) const {
    return c == '0' || c == '1';
}

bool Lexer::is_oct_digit(char c) const {
    return c >= '0' && c <= '7';
}

// ─── Skip Whitespace ─────────────────────────────────────────────────

void Lexer::skip_whitespace() {
    while (!is_at_end()) {
        char c = current_char();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();  // line count already handled in advance()
        } else {
            break;
        }
    }
}

// ─── Main Lexer Entry Point ─────────────────────────────────────────

Token Lexer::next_token() {
    skip_whitespace();
    
    if (is_at_end()) {
        return make_token(TokenType::EOF_TOKEN, "");
    }
    
    char c = current_char();
    
    // Comments
    if (c == '/' && peek_char(1) == '-') {
        if (peek_char(2) == '-') {
            return lex_doc_comment();
        }
        return lex_comment();
    }
    
    // Identifiers and keywords
    if (is_identifier_start(c)) {
        return lex_identifier();
    }
    
    // Numbers
    if (is_digit(c) || (c == '.' && is_digit(peek_char(1)))) {
        return lex_number();
    }
    
    // Strings
    if (c == '"') {
        if (peek_char(1) == '"' && peek_char(2) == '"') {
            return lex_raw_string();
        }
        return lex_string();
    }
    
    // Characters
    if (c == '\'') {
        return lex_char();
    }
    
    // Operators and punctuation
    return lex_operator_or_punctuation();
}

// ─── Lex Identifiers and Keywords ────────────────────────────────────

Token Lexer::lex_identifier() {
    token_start = position;
    std::string value;
    
    while (!is_at_end() && is_identifier_char(current_char())) {
        value += current_char();
        advance();
    }
    
    // Check if it's a keyword
    TokenType type = keyword_to_type(value);
    if (type != TokenType::IDENTIFIER) {
        return make_token(type, value);
    }
    
    return make_token(TokenType::IDENTIFIER, value);
}

// ─── Lex Numbers ─────────────────────────────────────────────────────

Token Lexer::lex_number() {
    token_start = position;
    std::string value;
    char c = current_char();
    
    // Hexadecimal: 0x...
    if (c == '0' && (peek_char(1) == 'x' || peek_char(1) == 'X')) {
        advance(); // consume '0'
        advance(); // consume 'x' or 'X'
        value = "0x";
        
        if (!is_hex_digit(current_char())) {
            return error_token("Invalid hexadecimal literal: expected hex digit");
        }
        
        while (!is_at_end() && is_hex_digit(current_char())) {
            value += current_char();
            advance();
        }
        
        return make_token(TokenType::HEX_LITERAL, value);
    }
    
    // Binary: 0b...
    if (c == '0' && (peek_char(1) == 'b' || peek_char(1) == 'B')) {
        advance(); // consume '0'
        advance(); // consume 'b' or 'B'
        value = "0b";
        
        if (!is_bin_digit(current_char())) {
            return error_token("Invalid binary literal: expected 0 or 1");
        }
        
        while (!is_at_end() && is_bin_digit(current_char())) {
            value += current_char();
            advance();
        }
        
        return make_token(TokenType::BINARY_LITERAL, value);
    }
    
    // Octal: 0o...
    if (c == '0' && (peek_char(1) == 'o' || peek_char(1) == 'O')) {
        advance(); // consume '0'
        advance(); // consume 'o' or 'O'
        value = "0o";
        
        if (!is_oct_digit(current_char())) {
            return error_token("Invalid octal literal: expected octal digit (0-7)");
        }
        
        while (!is_at_end() && is_oct_digit(current_char())) {
            value += current_char();
            advance();
        }
        
        return make_token(TokenType::INT_LITERAL, value);
    }
    
    // Decimal integer or float
    bool is_float = false;
    
    // Integer part
    while (!is_at_end() && is_digit(current_char())) {
        value += current_char();
        advance();
    }
    
    // Fractional part
    if (current_char() == '.' && is_digit(peek_char(1))) {
        is_float = true;
        value += current_char();
        advance();
        
        while (!is_at_end() && is_digit(current_char())) {
            value += current_char();
            advance();
        }
    }
    
    // Exponent part
    if (current_char() == 'e' || current_char() == 'E') {
        is_float = true;
        value += current_char();
        advance();
        
        if (current_char() == '+' || current_char() == '-') {
            value += current_char();
            advance();
        }
        
        if (!is_digit(current_char())) {
            return error_token("Invalid float literal: expected digit after exponent");
        }
        
        while (!is_at_end() && is_digit(current_char())) {
            value += current_char();
            advance();
        }
    }
    
    if (is_float) {
        return make_token(TokenType::FLOAT_LITERAL, value);
    }
    
    return make_token(TokenType::INT_LITERAL, value);
}

// ─── Lex Strings ─────────────────────────────────────────────────────

Token Lexer::lex_string() {
    token_start = position;
    std::string value;
    bool has_interpolation = false;
    
    advance(); // consume opening "
    
    while (!is_at_end() && current_char() != '"') {
        char c = current_char();
        
        // Handle interpolation: \(expr)
        if (c == '\\' && peek_char(1) == '(') {
            has_interpolation = true;
            value += c;
            value += peek_char(1);
            advance();
            advance();
            
            // Parse the expression inside interpolation
            // For now, we just consume until closing )
            int paren_count = 1;
            while (!is_at_end() && paren_count > 0) {
                char ch = current_char();
                if (ch == '(') paren_count++;
                if (ch == ')') paren_count--;
                value += ch;
                advance();
            }
            continue;
        }
        
        // Handle escape sequences
        if (c == '\\') {
            char next = peek_char(1);
            switch (next) {
                case 'n': value += '\n'; advance(); break;
                case 't': value += '\t'; advance(); break;
                case 'r': value += '\r'; advance(); break;
                case '\\': value += '\\'; advance(); break;
                case '"': value += '"'; advance(); break;
                case '0': value += '\0'; advance(); break;
                default:
                    // Invalid escape sequence
                    return error_token("Invalid escape sequence: \\" + std::string(1, next));
            }
            advance();
            continue;
        }
        
        // Handle newlines (not allowed in regular strings)
        if (c == '\n') {
            return error_token("Unterminated string literal: newline not allowed in regular strings (use \"\"\" for multiline)");
        }
        
        value += c;
        advance();
    }
    
    if (is_at_end()) {
        return error_token("Unterminated string literal");
    }
    
    advance(); // consume closing "
    
    // Return regular string (interpolation will be handled by parser)
    return make_token(TokenType::STRING_LITERAL, value);
}

// ─── Lex Raw Strings ─────────────────────────────────────────────────

Token Lexer::lex_raw_string() {
    token_start = position;
    std::string value;
    
    // Consume """
    advance(); // consume first "
    advance(); // consume second "
    advance(); // consume third "
    
    // Content
    while (!is_at_end()) {
        // Check for closing """
        if (current_char() == '"' && peek_char(1) == '"' && peek_char(2) == '"') {
            advance(); // consume first "
            advance(); // consume second "
            advance(); // consume third "
            return make_token(TokenType::RAW_STRING_LITERAL, value);
        }
        
        value += current_char();
        advance();
    }
    
    return error_token("Unterminated raw string literal (expected \"\"\")");
}

// ─── Lex Characters ─────────────────────────────────────────────────

Token Lexer::lex_char() {
    token_start = position;
    std::string value;
    
    advance(); // consume opening '
    
    if (is_at_end()) {
        return error_token("Unterminated character literal");
    }
    
    char c = current_char();
    
    // Handle escape sequences
    if (c == '\\') {
        advance();
        if (is_at_end()) {
            return error_token("Unterminated character literal");
        }
        
        char next = current_char();
        switch (next) {
            case 'n': value = "\\n"; break;
            case 't': value = "\\t"; break;
            case 'r': value = "\\r"; break;
            case '\\': value = "\\\\"; break;
            case '\'': value = "\\'"; break;
            case '0': value = "\\0"; break;
            default:
                return error_token("Invalid escape sequence in character literal: \\" + std::string(1, next));
        }
        advance();
    } else {
        value = c;
        advance();
    }
    
    if (is_at_end() || current_char() != '\'') {
        return error_token("Unterminated character literal");
    }
    
    advance(); // consume closing '
    
    return make_token(TokenType::CHAR_LITERAL, value);
}

// ─── Lex Comments ────────────────────────────────────────────────────

Token Lexer::lex_comment() {
    token_start = position;
    std::string value;
    
    // Consume /-
    advance(); // consume /
    advance(); // consume -
    
    while (!is_at_end()) {
        // Check for closing -/
        if (current_char() == '-' && peek_char(1) == '/') {
            advance(); // consume -
            advance(); // consume /
            return make_token(TokenType::LINE_COMMENT, value);
        }
        
        value += current_char();
        advance();
    }
    
    return error_token("Unterminated block comment (expected -/)");
}

// ─── Lex Doc Comments ────────────────────────────────────────────────

Token Lexer::lex_doc_comment() {
    token_start = position;
    std::string value;
    
    // Consume /--
    advance(); // consume /
    advance(); // consume -
    advance(); // consume -
    
    while (!is_at_end()) {
        // Check for closing --/
        if (current_char() == '-' && peek_char(1) == '-' && peek_char(2) == '/') {
            advance(); // consume -
            advance(); // consume -
            advance(); // consume /
            return make_token(TokenType::DOC_COMMENT, value);
        }
        
        value += current_char();
        advance();
    }
    
    return error_token("Unterminated documentation comment (expected --/)");
}

// ─── Lex Operators and Punctuation ──────────────────────────────────

Token Lexer::lex_operator_or_punctuation() {
    token_start = position;
    char c = current_char();
    
    // ─── Two-character operators ────────────────────────────────────
    
    // Exponentiation: ** and **=
    if (c == '*') {
        if (peek_char(1) == '*') {
            advance(); // consume first *
            if (peek_char(1) == '=') {
                advance(); // consume =
                return make_token(TokenType::POW_ASSIGN, "**=");
            }
            advance(); // consume second *
            return make_token(TokenType::POW, "**");
        }
    }
    
    // Bitwise shift: <<, <<=, >>, >>=
    if (c == '<' && peek_char(1) == '<') {
        advance();
        if (peek_char(1) == '=') {
            advance();
            return make_token(TokenType::SHL_ASSIGN, "<<=");
        }
        advance();
        return make_token(TokenType::SHL, "<<");
    }
    
    if (c == '>' && peek_char(1) == '>') {
        advance();
        if (peek_char(1) == '=') {
            advance();
            return make_token(TokenType::SHR_ASSIGN, ">>=");
        }
        advance();
        return make_token(TokenType::SHR, ">>");
    }
    
    // Ranges: .. and ..<
    if (c == '.' && peek_char(1) == '.') {
        advance();
        if (peek_char(1) == '<') {
            advance();
            return make_token(TokenType::RANGE_EXCLUSIVE, "..<");
        }
        advance();
        return make_token(TokenType::RANGE, "..");
    }
    
    // Composition: +>
    if (c == '+' && peek_char(1) == '>') {
        advance();
        advance();
        return make_token(TokenType::COMPOSE, "+>");
    }
    
    // Pipeline: |>
    if (c == '|' && peek_char(1) == '>') {
        advance();
        advance();
        return make_token(TokenType::PIPELINE, "|>");
    }
    
    // Nullable access: ?.
    if (c == '?' && peek_char(1) == '.') {
        advance();
        advance();
        return make_token(TokenType::QUESTION_DOT, "?.");
    }
    
    // Nullable/fallback: ??
    if (c == '?' && peek_char(1) == '?') {
        advance();
        advance();
        return make_token(TokenType::QUESTION_QUESTION, "??");
    }
    
    // Function arrow: ->
    if (c == '-' && peek_char(1) == '>') {
        advance();
        advance();
        return make_token(TokenType::ARROW, "->");
    }
    
    // ─── Single-character operators ─────────────────────────────────
    
    // Assignment and compound assignments
    if (c == '=') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::EQUAL_EQUAL, "==");
        }
        advance();
        return make_token(TokenType::ASSIGN, "=");
    }
    
    if (c == '+') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::PLUS_ASSIGN, "+=");
        }
        advance();
        return make_token(TokenType::PLUS, "+");
    }
    
    if (c == '-') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::MINUS_ASSIGN, "-=");
        }
        advance();
        return make_token(TokenType::MINUS, "-");
    }
    
    if (c == '*') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::MUL_ASSIGN, "*=");
        }
        advance();
        return make_token(TokenType::MUL, "*");
    }
    
    if (c == '/') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::DIV_ASSIGN, "/=");
        }
        advance();
        return make_token(TokenType::DIV, "/");
    }
    
    if (c == '%') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::MOD_ASSIGN, "%=");
        }
        advance();
        return make_token(TokenType::MOD, "%");
    }
    
    if (c == '^') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::BIT_XOR_ASSIGN, "^=");
        }
        advance();
        return make_token(TokenType::BIT_XOR, "^");
    }
    
    if (c == '&') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::BIT_AND_ASSIGN, "&=");
        }
        advance();
        return make_token(TokenType::BIT_AND, "&");
    }
    
    if (c == '|') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::BIT_OR_ASSIGN, "|=");
        }
        advance();
        return make_token(TokenType::BIT_OR, "|");
    }
    
    if (c == '~') {
        advance();
        return make_token(TokenType::BIT_NOT, "~");
    }
    
    // Comparison operators
    if (c == '!') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::NOT_EQUAL, "!=");
        }
        advance();
        return make_token(TokenType::BANG, "!");
    }
    
    if (c == '<') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::LESS_EQUAL, "<=");
        }
        advance();
        return make_token(TokenType::LESS, "<");
    }
    
    if (c == '>') {
        if (peek_char(1) == '=') {
            advance();
            advance();
            return make_token(TokenType::GREATER_EQUAL, ">=");
        }
        advance();
        return make_token(TokenType::GREATER, ">");
    }
    
    // ─── Punctuation ─────────────────────────────────────────────────
    
    if (c == '.') {
        advance();
        return make_token(TokenType::DOT, ".");
    }
    
    if (c == ':') {
        advance();
        return make_token(TokenType::COLON, ":");
    }
    
    if (c == ',') {
        advance();
        return make_token(TokenType::COMMA, ",");
    }
    
    if (c == ';') {
        advance();
        return make_token(TokenType::SEMICOLON, ";");
    }
    
    if (c == '(') {
        advance();
        return make_token(TokenType::LPAREN, "(");
    }
    
    if (c == ')') {
        advance();
        return make_token(TokenType::RPAREN, ")");
    }
    
    if (c == '{') {
        advance();
        return make_token(TokenType::LBRACE, "{");
    }
    
    if (c == '}') {
        advance();
        return make_token(TokenType::RBRACE, "}");
    }
    
    if (c == '[') {
        advance();
        return make_token(TokenType::LBRACKET, "[");
    }
    
    if (c == ']') {
        advance();
        return make_token(TokenType::RBRACKET, "]");
    }
    
    if (c == '@') {
        advance();
        return make_token(TokenType::AT_SIGN, "@");
    }
    
    if (c == '#') {
        advance();
        return make_token(TokenType::HASH, "#");
    }
    
    if (c == '_') {
        advance();
        return make_token(TokenType::UNDERSCORE, "_");
    }
    
    // Variadic: ...
    if (c == '.' && peek_char(1) == '.' && peek_char(2) == '.') {
        advance();
        advance();
        advance();
        return make_token(TokenType::VARIADIC, "...");
    }
    
    // Unknown character
    std::string msg = "Unexpected character: '";
    msg += c;
    msg += "'";
    advance();
    return error_token(msg);
}

// ─── Peek Token ──────────────────────────────────────────────────────

Token Lexer::peek_token() {
    // Save current state
    size_t saved_pos = position;
    int saved_line = line;
    int saved_col = column;
    
    // Get next token
    Token token = next_token();
    
    // Restore state
    position = saved_pos;
    line = saved_line;
    column = saved_col;
    
    return token;
}

// ─── Tokenize All ────────────────────────────────────────────────────

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> tokens;
    Token token;
    
    do {
        token = next_token();
        tokens.push_back(token);
    } while (token.type != TokenType::EOF_TOKEN);
    
    return tokens;
}