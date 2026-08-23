/**
 * @file Lexer.cpp
 * @brief Implementation of pure lexing functions.
 */

#include "Lexer.hpp"
#include <cctype>
#include <unordered_map>

namespace lexer {

// ─── Internal Lexer State ──────────────────────────────────────────────

namespace detail {

struct LexerState {
    const std::string& source;
    DiagnosticEngine& diagnostics;
    std::vector<Token> tokens;
    size_t position = 0;
    unsigned int line = 1;
    unsigned short column = 1;
    bool hadErrors = false;
    
    LexerState(const std::string& src, DiagnosticEngine& diag)
        : source(src), diagnostics(diag) {}
};

// ─── Character Helpers ──────────────────────────────────────────────────

inline bool isAtEnd(const LexerState& state) {
    return state.position >= state.source.length();
}

inline char currentChar(const LexerState& state) {
    if (isAtEnd(state)) return '\0';
    return state.source[state.position];
}

inline char peekChar(const LexerState& state, int offset = 0) {
    size_t pos = state.position + offset;
    if (pos >= state.source.length()) return '\0';
    return state.source[pos];
}

inline void advance(LexerState& state) {
    if (isAtEnd(state)) return;
    if (currentChar(state) == '\n') {
        state.line++;
        state.column = 1;
    } else {
        state.column++;
    }
    state.position++;
}

inline bool match(LexerState& state, char expected) {
    if (isAtEnd(state)) return false;
    if (currentChar(state) != expected) return false;
    advance(state);
    return true;
}

inline bool matchTwo(LexerState& state, char first, char second) {
    if (isAtEnd(state)) return false;
    if (currentChar(state) != first) return false;
    if (peekChar(state, 1) != second) return false;
    advance(state);
    advance(state);
    return true;
}

inline Token makeToken(TokenType type, const std::string& value, 
                       const LexerState& state) {
    return Token{type, value, state.line, state.column};
}

inline void reportError(LexerState& state, DiagCode code, 
                        const std::string& message) {
    SourceLocation loc(state.line, state.column);
    state.diagnostics.errorAt(code, loc, message);
    state.hadErrors = true;
}

// ─── Skip Whitespace ─────────────────────────────────────────────────────

void skipWhitespace(LexerState& state) {
    while (!isAtEnd(state)) {
        char c = currentChar(state);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(state);
        } else {
            break;
        }
    }
}

// ─── Lex Identifier ──────────────────────────────────────────────────────

Token lexIdentifier(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    while (!isAtEnd(state) && isIdentifierChar(currentChar(state))) {
        value += currentChar(state);
        advance(state);
    }
    
    TokenType type = keyword_to_type(value);
    if (type != TokenType::IDENTIFIER) {
        return Token{type, value, startLine, startCol};
    }
    
    return Token{TokenType::IDENTIFIER, value, startLine, startCol};
}

// ─── Lex Number ──────────────────────────────────────────────────────────

Token lexNumber(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    char c = currentChar(state);
    
    // Hexadecimal: 0x...
    if (c == '0' && (peekChar(state, 1) == 'x' || peekChar(state, 1) == 'X')) {
        advance(state);
        advance(state);
        value = "0x";
        
        if (!isHexDigit(currentChar(state))) {
            reportError(state, DiagCode::Lex_InvalidNumberLiteral,
                        "Invalid hex literal: expected hex digit");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        while (!isAtEnd(state) && isHexDigit(currentChar(state))) {
            value += currentChar(state);
            advance(state);
        }
        return Token{TokenType::HEX_LITERAL, value, startLine, startCol};
    }
    
    // Binary: 0b...
    if (c == '0' && (peekChar(state, 1) == 'b' || peekChar(state, 1) == 'B')) {
        advance(state);
        advance(state);
        value = "0b";
        
        if (!isBinDigit(currentChar(state))) {
            reportError(state, DiagCode::Lex_InvalidNumberLiteral,
                        "Invalid binary literal: expected 0 or 1");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        while (!isAtEnd(state) && isBinDigit(currentChar(state))) {
            value += currentChar(state);
            advance(state);
        }
        return Token{TokenType::BINARY_LITERAL, value, startLine, startCol};
    }
    
    // Octal: 0o...
    if (c == '0' && (peekChar(state, 1) == 'o' || peekChar(state, 1) == 'O')) {
        advance(state);
        advance(state);
        value = "0o";
        
        if (!isOctDigit(currentChar(state))) {
            reportError(state, DiagCode::Lex_InvalidNumberLiteral,
                        "Invalid octal literal: expected octal digit (0-7)");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        while (!isAtEnd(state) && isOctDigit(currentChar(state))) {
            value += currentChar(state);
            advance(state);
        }
        return Token{TokenType::INT_LITERAL, value, startLine, startCol};
    }
    
    // Decimal integer or float
    bool isFloat = false;
    
    // Integer part
    while (!isAtEnd(state) && isDigit(currentChar(state))) {
        value += currentChar(state);
        advance(state);
    }
    
    // Fractional part
    if (currentChar(state) == '.' && isDigit(peekChar(state, 1))) {
        isFloat = true;
        value += currentChar(state);
        advance(state);
        
        while (!isAtEnd(state) && isDigit(currentChar(state))) {
            value += currentChar(state);
            advance(state);
        }
    }
    
    // Exponent part
    if (currentChar(state) == 'e' || currentChar(state) == 'E') {
        isFloat = true;
        value += currentChar(state);
        advance(state);
        
        if (currentChar(state) == '+' || currentChar(state) == '-') {
            value += currentChar(state);
            advance(state);
        }
        
        if (!isDigit(currentChar(state))) {
            reportError(state, DiagCode::Lex_InvalidNumberLiteral,
                        "Invalid float literal: expected digit after exponent");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        while (!isAtEnd(state) && isDigit(currentChar(state))) {
            value += currentChar(state);
            advance(state);
        }
    }
    
    if (isFloat) {
        return Token{TokenType::FLOAT_LITERAL, value, startLine, startCol};
    }
    
    return Token{TokenType::INT_LITERAL, value, startLine, startCol};
}

// ─── Lex String ──────────────────────────────────────────────────────────

Token lexString(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    advance(state); // consume opening "
    
    while (!isAtEnd(state) && currentChar(state) != '"') {
        char c = currentChar(state);
        
        // Handle interpolation: \(expr)
        if (c == '\\' && peekChar(state, 1) == '(') {
            value += c;
            value += peekChar(state, 1);
            advance(state);
            advance(state);
            
            int parenCount = 1;
            while (!isAtEnd(state) && parenCount > 0) {
                char ch = currentChar(state);
                if (ch == '(') parenCount++;
                if (ch == ')') parenCount--;
                value += ch;
                advance(state);
            }
            continue;
        }
        
        // Handle escape sequences
        if (c == '\\') {
            char next = peekChar(state, 1);
            switch (next) {
                case 'n': value += '\n'; advance(state); break;
                case 't': value += '\t'; advance(state); break;
                case 'r': value += '\r'; advance(state); break;
                case '\\': value += '\\'; advance(state); break;
                case '"': value += '"'; advance(state); break;
                case '0': value += '\0'; advance(state); break;
                default:
                    reportError(state, DiagCode::Lex_InvalidEscapeSequence,
                                "Invalid escape sequence: \\" + std::string(1, next));
                    return Token{TokenType::UNKNOWN, value, startLine, startCol};
            }
            advance(state);
            continue;
        }
        
        // Check for unescaped newline
        if (c == '\n') {
            reportError(state, DiagCode::Lex_UnterminatedString,
                        "Unterminated string: newline not allowed (use \"\"\" for multiline)");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        value += c;
        advance(state);
    }
    
    if (isAtEnd(state)) {
        reportError(state, DiagCode::Lex_UnterminatedString,
                    "Unterminated string literal");
        return Token{TokenType::UNKNOWN, value, startLine, startCol};
    }
    
    advance(state); // consume closing "
    return Token{TokenType::STRING_LITERAL, value, startLine, startCol};
}

// ─── Lex Raw String ──────────────────────────────────────────────────────

Token lexRawString(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    advance(state);
    advance(state);
    advance(state); // consume """
    
    while (!isAtEnd(state)) {
        if (currentChar(state) == '"' && peekChar(state, 1) == '"' && peekChar(state, 2) == '"') {
            advance(state);
            advance(state);
            advance(state);
            return Token{TokenType::RAW_STRING_LITERAL, value, startLine, startCol};
        }
        
        value += currentChar(state);
        advance(state);
    }
    
    reportError(state, DiagCode::Lex_UnterminatedRawString,
                "Unterminated raw string literal (expected \"\"\")");
    return Token{TokenType::UNKNOWN, value, startLine, startCol};
}

// ─── Lex Character ───────────────────────────────────────────────────────

Token lexChar(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    advance(state); // consume opening '
    
    if (isAtEnd(state)) {
        reportError(state, DiagCode::Lex_UnterminatedCharLiteral,
                    "Unterminated character literal");
        return Token{TokenType::UNKNOWN, value, startLine, startCol};
    }
    
    char c = currentChar(state);
    
    if (c == '\\') {
        advance(state);
        if (isAtEnd(state)) {
            reportError(state, DiagCode::Lex_UnterminatedCharLiteral,
                        "Unterminated character literal");
            return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        
        char next = currentChar(state);
        switch (next) {
            case 'n': value = "\\n"; break;
            case 't': value = "\\t"; break;
            case 'r': value = "\\r"; break;
            case '\\': value = "\\\\"; break;
            case '\'': value = "\\'"; break;
            case '0': value = "\\0"; break;
            default:
                reportError(state, DiagCode::Lex_InvalidEscapeSequence,
                            "Invalid escape sequence: \\" + std::string(1, next));
                return Token{TokenType::UNKNOWN, value, startLine, startCol};
        }
        advance(state);
    } else {
        value = c;
        advance(state);
    }
    
    if (isAtEnd(state) || currentChar(state) != '\'') {
        reportError(state, DiagCode::Lex_UnterminatedCharLiteral,
                    "Unterminated character literal");
        return Token{TokenType::UNKNOWN, value, startLine, startCol};
    }
    
    advance(state); // consume closing '
    return Token{TokenType::CHAR_LITERAL, value, startLine, startCol};
}

// ─── Lex Line Comment ────────────────────────────────────────────────────

Token lexLineComment(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    advance(state);
    advance(state); // consume --
    
    while (!isAtEnd(state) && currentChar(state) != '\n') {
        value += currentChar(state);
        advance(state);
    }
    
    return Token{TokenType::LINE_COMMENT, value, startLine, startCol};
}

// ─── Lex Block Comment ───────────────────────────────────────────────────

Token lexBlockComment(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    int nestingDepth = 1;
    
    advance(state);
    advance(state); // consume /-
    
    while (!isAtEnd(state) && nestingDepth > 0) {
        char c = currentChar(state);
        
        if (c == '/' && peekChar(state, 1) == '-') {
            nestingDepth++;
            value += c;
            value += peekChar(state, 1);
            advance(state);
            advance(state);
            continue;
        }
        
        if (c == '-' && peekChar(state, 1) == '/') {
            nestingDepth--;
            if (nestingDepth > 0) {
                value += c;
                value += peekChar(state, 1);
                advance(state);
                advance(state);
            } else {
                advance(state);
                advance(state);
            }
            continue;
        }
        
        value += c;
        advance(state);
    }
    
    if (nestingDepth > 0) {
        reportError(state, DiagCode::Lex_UnterminatedBlockComment,
                    "Unterminated block comment (expected -/)");
        return Token{TokenType::UNKNOWN, value, startLine, startCol};
    }
    
    return Token{TokenType::BLOCK_COMMENT, value, startLine, startCol};
}

// ─── Lex Doc Comment ─────────────────────────────────────────────────────

Token lexDocComment(LexerState& state) {
    std::string value;
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    
    advance(state);
    advance(state);
    advance(state); // consume /--
    
    while (!isAtEnd(state)) {
        if (currentChar(state) == '-' && peekChar(state, 1) == '-' && peekChar(state, 2) == '/') {
            advance(state);
            advance(state);
            advance(state);
            return Token{TokenType::DOC_COMMENT, value, startLine, startCol};
        }
        
        value += currentChar(state);
        advance(state);
    }
    
    reportError(state, DiagCode::Lex_UnterminatedBlockComment,
                "Unterminated documentation comment (expected --/)");
    return Token{TokenType::UNKNOWN, value, startLine, startCol};
}

// ─── Lex Operator or Punctuation ────────────────────────────────────────

Token lexOperatorOrPunctuation(LexerState& state) {
    unsigned int startLine = state.line;
    unsigned short startCol = state.column;
    char c = currentChar(state);
    
    // ─── Two-character operators ──────────────────────────────────────
    
    // ** and **=
    if (c == '*') {
        if (peekChar(state, 1) == '*') {
            advance(state);
            if (peekChar(state, 1) == '=') {
                advance(state);
                return Token{TokenType::POW_ASSIGN, "**=", startLine, startCol};
            }
            advance(state);
            return Token{TokenType::POW, "**", startLine, startCol};
        }
    }
    
    // <<, <<=, >>, >>=
    if (c == '<' && peekChar(state, 1) == '<') {
        advance(state);
        if (peekChar(state, 1) == '=') {
            advance(state);
            return Token{TokenType::SHL_ASSIGN, "<<=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::SHL, "<<", startLine, startCol};
    }
    
    if (c == '>' && peekChar(state, 1) == '>') {
        advance(state);
        if (peekChar(state, 1) == '=') {
            advance(state);
            return Token{TokenType::SHR_ASSIGN, ">>=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::SHR, ">>", startLine, startCol};
    }
    
    // .. and ..<
    if (c == '.' && peekChar(state, 1) == '.') {
        advance(state);
        if (peekChar(state, 1) == '<') {
            advance(state);
            return Token{TokenType::RANGE_EXCLUSIVE, "..<", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::RANGE, "..", startLine, startCol};
    }
    
    // +>
    if (c == '+' && peekChar(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::COMPOSE, "+>", startLine, startCol};
    }
    
    // |>
    if (c == '|' && peekChar(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::PIPELINE, "|>", startLine, startCol};
    }
    
    // ?.
    if (c == '?' && peekChar(state, 1) == '.') {
        advance(state);
        advance(state);
        return Token{TokenType::QUESTION_DOT, "?.", startLine, startCol};
    }
    
    // ??
    if (c == '?' && peekChar(state, 1) == '?') {
        advance(state);
        advance(state);
        return Token{TokenType::QUESTION_QUESTION, "??", startLine, startCol};
    }
    
    // ->
    if (c == '-' && peekChar(state, 1) == '>') {
        advance(state);
        advance(state);
        return Token{TokenType::ARROW, "->", startLine, startCol};
    }
    
    // ─── Single-character operators ──────────────────────────────────
    
    if (c == '=') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::EQUAL_EQUAL, "==", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::ASSIGN, "=", startLine, startCol};
    }
    
    if (c == '+') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::PLUS_ASSIGN, "+=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::PLUS, "+", startLine, startCol};
    }
    
    if (c == '-') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MINUS_ASSIGN, "-=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::MINUS, "-", startLine, startCol};
    }
    
    if (c == '*') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MUL_ASSIGN, "*=", startLine, startCol};
        }
        // ** is handled above
        advance(state);
        return Token{TokenType::MUL, "*", startLine, startCol};
    }
    
    if (c == '/') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::DIV_ASSIGN, "/=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::DIV, "/", startLine, startCol};
    }
    
    if (c == '%') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::MOD_ASSIGN, "%=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::MOD, "%", startLine, startCol};
    }
    
    if (c == '^') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_XOR_ASSIGN, "^=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::BIT_XOR, "^", startLine, startCol};
    }
    
    if (c == '&') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_AND_ASSIGN, "&=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::BIT_AND, "&", startLine, startCol};
    }
    
    if (c == '|') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::BIT_OR_ASSIGN, "|=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::BIT_OR, "|", startLine, startCol};
    }
    
    if (c == '~') {
        advance(state);
        return Token{TokenType::BIT_NOT, "~", startLine, startCol};
    }
    
    if (c == '!') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::NOT_EQUAL, "!=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::BANG, "!", startLine, startCol};
    }
    
    if (c == '<') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::LESS_EQUAL, "<=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::LESS, "<", startLine, startCol};
    }
    
    if (c == '>') {
        if (peekChar(state, 1) == '=') {
            advance(state);
            advance(state);
            return Token{TokenType::GREATER_EQUAL, ">=", startLine, startCol};
        }
        advance(state);
        return Token{TokenType::GREATER, ">", startLine, startCol};
    }
    
    if (c == '?') {
        advance(state);
        return Token{TokenType::QUESTION, "?", startLine, startCol};
    }
    
    // ─── Punctuation ─────────────────────────────────────────────────
    
    if (c == '.') {
        // ... is handled below
        advance(state);
        return Token{TokenType::DOT, ".", startLine, startCol};
    }
    
    if (c == ':') {
        advance(state);
        return Token{TokenType::COLON, ":", startLine, startCol};
    }
    
    if (c == ',') {
        advance(state);
        return Token{TokenType::COMMA, ",", startLine, startCol};
    }
    
    if (c == ';') {
        advance(state);
        return Token{TokenType::SEMICOLON, ";", startLine, startCol};
    }
    
    if (c == '(') {
        advance(state);
        return Token{TokenType::LPAREN, "(", startLine, startCol};
    }
    
    if (c == ')') {
        advance(state);
        return Token{TokenType::RPAREN, ")", startLine, startCol};
    }
    
    if (c == '{') {
        advance(state);
        return Token{TokenType::LBRACE, "{", startLine, startCol};
    }
    
    if (c == '}') {
        advance(state);
        return Token{TokenType::RBRACE, "}", startLine, startCol};
    }
    
    if (c == '[') {
        advance(state);
        return Token{TokenType::LBRACKET, "[", startLine, startCol};
    }
    
    if (c == ']') {
        advance(state);
        return Token{TokenType::RBRACKET, "]", startLine, startCol};
    }
    
    if (c == '@') {
        advance(state);
        return Token{TokenType::AT_SIGN, "@", startLine, startCol};
    }
    
    if (c == '#') {
        advance(state);
        return Token{TokenType::HASH, "#", startLine, startCol};
    }
    
    if (c == '_') {
        advance(state);
        return Token{TokenType::UNDERSCORE, "_", startLine, startCol};
    }
    
    // Variadic: ...
    if (c == '.' && peekChar(state, 1) == '.' && peekChar(state, 2) == '.') {
        advance(state);
        advance(state);
        advance(state);
        return Token{TokenType::VARIADIC, "...", startLine, startCol};
    }
    
    // Unknown character
    std::string msg = "Unexpected character: '";
    msg += c;
    msg += "'";
    reportError(state, DiagCode::Lex_InvalidCharacter, msg);
    advance(state);
    return Token{TokenType::UNKNOWN, msg, startLine, startCol};
}

// ─── Main Tokenization Loop ─────────────────────────────────────────────

Token nextToken(LexerState& state) {
    skipWhitespace(state);
    
    if (isAtEnd(state)) {
        return Token{TokenType::EOF_TOKEN, "EOF", state.line, state.column};
    }
    
    char c = currentChar(state);
    
    // Line comment: --
    if (c == '-' && peekChar(state, 1) == '-') {
        // Check for doc comment /--
        if (peekChar(state, 2) == '/') {
            return lexDocComment(state);
        }
        return lexLineComment(state);
    }
    
    // Block comment: /-
    if (c == '/' && peekChar(state, 1) == '-') {
        // Check for doc comment /--
        if (peekChar(state, 2) == '-') {
            return lexDocComment(state);
        }
        return lexBlockComment(state);
    }
    
    // Identifiers and keywords
    if (isIdentifierStart(c)) {
        return lexIdentifier(state);
    }
    
    // Numbers
    if (isDigit(c) || (c == '.' && isDigit(peekChar(state, 1)))) {
        return lexNumber(state);
    }
    
    // Strings
    if (c == '"') {
        if (peekChar(state, 1) == '"' && peekChar(state, 2) == '"') {
            return lexRawString(state);
        }
        return lexString(state);
    }
    
    // Characters
    if (c == '\'') {
        return lexChar(state);
    }
    
    // Operators and punctuation
    return lexOperatorOrPunctuation(state);
}

} // namespace detail

// ─── Public API ─────────────────────────────────────────────────────────

std::vector<Token> tokenize(const std::string& source, 
                            DiagnosticEngine& diagnostics) {
    detail::LexerState state(source, diagnostics);
    
    while (true) {
        Token token = detail::nextToken(state);
        state.tokens.push_back(token);
        if (token.type == TokenType::EOF_TOKEN) break;
    }
    
    return state.tokens;
}

std::vector<Token> tokenize_n(const std::string& source, 
                              size_t max_tokens,
                              DiagnosticEngine& diagnostics) {
    detail::LexerState state(source, diagnostics);
    
    for (size_t i = 0; i < max_tokens; ++i) {
        Token token = detail::nextToken(state);
        state.tokens.push_back(token);
        if (token.type == TokenType::EOF_TOKEN) break;
    }
    
    return state.tokens;
}

} // namespace lexer