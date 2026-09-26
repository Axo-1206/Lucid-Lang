/**
 * @file Lexer.cpp
 * @brief Implementation of the Lucid lexer.
 *
 * ─── What this file implements ────────────────────────────────────────────
 * A single-pass lexer over one source file. Every function below lexes
 * one category of token; the top-level `lexOne` dispatches on the
 * current character.
 *
 * ─── The cursor ───────────────────────────────────────────────────────────
 * The lexer maintains a triple (position, line, column). Only one
 * operation, `advance()`, moves them, and it moves all three together so
 * they can never disagree. Every token constructor takes the cursor's
 * location *before* the token's first character was consumed.
 *
 * ─── No conditional keywording ────────────────────────────────────────────
 * Every keyword in Tokens.hpp is a keyword everywhere. There is no
 * context where `int` is an identifier and no context where `start` is
 * not a keyword. This is a deliberate property of the token set: it
 * makes the parser's dispatch a pure function of the current token
 * type, with no dependence on surrounding context.
 */

#include "Lexer.hpp"

#include <cstring>
#include <string>
#include <utility>

using namespace lucid::diag;

namespace lucid::lexer {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Lexer state
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The cursor and the diagnostic sink.
///
/// The cursor is a triple (position, line, column) where `position` is a
/// byte offset into the source and `line`/`column` are 1-indexed. The
/// cursor always points at the next character to be lexed.
struct LexerState {
    std::string_view              source;
    lucid::diag::DiagnosticEngine& diagnostics;
    std::vector<Token>            tokens;

    size_t   position = 0;   // byte offset into source
    uint32_t line     = 1;   // 1-indexed
    uint32_t column   = 1;   // 1-indexed

    LexerState(std::string_view src, lucid::diag::DiagnosticEngine& diag)
        : source(src), diagnostics(diag) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Cursor operations
// ─────────────────────────────────────────────────────────────────────────────

bool isAtEnd(const LexerState& s) noexcept {
    return s.position >= s.source.size();
}

char currentChar(const LexerState& s) noexcept {
    return isAtEnd(s) ? '\0' : s.source[s.position];
}

char peekChar(const LexerState& s, size_t offset = 0) noexcept {
    const size_t pos = s.position + offset;
    return pos >= s.source.size() ? '\0' : s.source[pos];
}

void advance(LexerState& s) noexcept {
    if (isAtEnd(s)) return;
    if (s.source[s.position] == '\n') {
        s.line++;
        s.column = 1;
    } else {
        s.column++;
    }
    s.position++;
}

bool match(LexerState& s, char expected) noexcept {
    if (isAtEnd(s) || currentChar(s) != expected) return false;
    advance(s);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Token construction
// ─────────────────────────────────────────────────────────────────────────────

Token makeToken(TokenType type,
                std::string value,
                SourceLocation location) {
    return Token{type, std::move(value), location};
}

/// @brief Capture the cursor's current (line, column) as a SourceLocation.
SourceLocation currentLocation(const LexerState& s) noexcept {
    return SourceLocation{s.line, s.column};
}

void reportError(LexerState& s, DiagCode code, std::string message) {
    s.diagnostics.errorAt(code, currentLocation(s), std::move(message));
}

// ─────────────────────────────────────────────────────────────────────────────
// The keyword table
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the *entire* set of words the lexer recognizes as anything
// other than IDENTIFIER. Every entry corresponds to a `KW_*` value in
// Tokens.hpp.
//
// The table is a flat lookup; the lexer compares the identifier's lexeme
// against each spelling. Fifty entries; a switch or a hash map would
// also work, but a linear scan of an array of string_views is fast
// enough and the table is a single readable list.

TokenType keywordToType(std::string_view word) noexcept {
    // ─── Declaration keywords ───────────────────────────────────────────
    if (word == "TABLE")            return TokenType::KW_TABLE;
    if (word == "FN")               return TokenType::KW_FN;
    if (word == "let")              return TokenType::KW_LET;
    if (word == "const")            return TokenType::KW_CONST;
    if (word == "import")           return TokenType::KW_IMPORT;
    if (word == "as")               return TokenType::KW_AS;
    if (word == "host")             return TokenType::KW_HOST;

    // ─── Primitive type keywords ────────────────────────────────────────
    if (word == "bool")             return TokenType::KW_BOOL;
    if (word == "char")             return TokenType::KW_CHAR;
    if (word == "string")           return TokenType::KW_STRING;
    if (word == "unit")             return TokenType::KW_UNIT;

    if (word == "int8")             return TokenType::KW_INT8;
    if (word == "int16")            return TokenType::KW_INT16;
    if (word == "int32")            return TokenType::KW_INT32;
    if (word == "int64")            return TokenType::KW_INT64;
    if (word == "uint8")            return TokenType::KW_UINT8;
    if (word == "uint16")           return TokenType::KW_UINT16;
    if (word == "uint32")           return TokenType::KW_UINT32;
    if (word == "uint64")           return TokenType::KW_UINT64;
    if (word == "float32")          return TokenType::KW_FLOAT32;
    if (word == "float64")          return TokenType::KW_FLOAT64;

    // Sized aliases
    if (word == "int")              return TokenType::KW_INT;
    if (word == "long")             return TokenType::KW_LONG;
    if (word == "uint")             return TokenType::KW_UINT;
    if (word == "ulong")            return TokenType::KW_ULONG;
    if (word == "float")            return TokenType::KW_FLOAT;
    if (word == "double")           return TokenType::KW_DOUBLE;

    // ─── Statement keywords ─────────────────────────────────────────────
    if (word == "if")               return TokenType::KW_IF;
    if (word == "else")             return TokenType::KW_ELSE;
    if (word == "switch")           return TokenType::KW_SWITCH;
    if (word == "case")             return TokenType::KW_CASE;
    if (word == "default")          return TokenType::KW_DEFAULT;
    if (word == "for")              return TokenType::KW_FOR;
    if (word == "in")               return TokenType::KW_IN;
    if (word == "while")            return TokenType::KW_WHILE;
    if (word == "return")           return TokenType::KW_RETURN;
    if (word == "break")            return TokenType::KW_BREAK;
    if (word == "continue")         return TokenType::KW_CONTINUE;

    // ─── Sequence keywords ──────────────────────────────────────────────
    if (word == "wait")             return TokenType::KW_WAIT;
    if (word == "waitFrames")       return TokenType::KW_WAIT_FRAMES;
    if (word == "waitUntil")        return TokenType::KW_WAIT_UNTIL;
    if (word == "waitForEvent")     return TokenType::KW_WAIT_FOR_EVENT;
    if (word == "waitForRequest")   return TokenType::KW_WAIT_FOR_REQUEST;
    if (word == "start")            return TokenType::KW_START;

    // ─── Operator keywords ──────────────────────────────────────────────
    if (word == "and")              return TokenType::KW_AND;
    if (word == "or")               return TokenType::KW_OR;
    if (word == "not")              return TokenType::KW_NOT;

    // ─── Literal keywords ───────────────────────────────────────────────
    if (word == "true")             return TokenType::KW_TRUE;
    if (word == "false")            return TokenType::KW_FALSE;
    if (word == "nil")              return TokenType::KW_NIL;

    // Not a keyword. Sema resolves the name against declarations.
    return TokenType::IDENTIFIER;
}

// ─────────────────────────────────────────────────────────────────────────────
// Whitespace
// ─────────────────────────────────────────────────────────────────────────────

void skipWhitespace(LexerState& s) noexcept {
    while (!isAtEnd(s)) {
        const char c = currentChar(s);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(s);
        } else {
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Identifiers and keywords
// ─────────────────────────────────────────────────────────────────────────────

void lexIdentifier(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const size_t         startPos = s.position;

    while (!isAtEnd(s) && isIdentifierChar(currentChar(s))) {
        advance(s);
    }

    const std::string_view word =
        s.source.substr(startPos, s.position - startPos);
    const TokenType type = keywordToType(word);

    s.tokens.push_back(makeToken(type, std::string(word), startLoc));
}

// ─────────────────────────────────────────────────────────────────────────────
// Numbers
// ─────────────────────────────────────────────────────────────────────────────
//
// The lexer produces raw lexemes. It does not parse the number: `0xFF`
// is a HEX_LITERAL with value "0xFF", and Sema interprets it.

void lexNumber(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const size_t         startPos = s.position;

    // Radix prefixes: 0x, 0b, 0o (and uppercase).
    if (currentChar(s) == '0') {
        const char next = peekChar(s, 1);
        if (next == 'x' || next == 'X') {
            advance(s); advance(s);  // consume 0x
            if (!isHexDigit(currentChar(s))) {
                reportError(s, DiagCode::Lex_InvalidRadixLiteral,
                            "hexadecimal literal has no digits after '0x'");
                s.tokens.push_back(makeToken(
                    TokenType::UNKNOWN,
                    std::string(s.source.substr(startPos, s.position - startPos)),
                    startLoc));
                return;
            }
            while (isHexDigit(currentChar(s))) advance(s);
            s.tokens.push_back(makeToken(
                TokenType::HEX_LITERAL,
                std::string(s.source.substr(startPos, s.position - startPos)),
                startLoc));
            return;
        }
        if (next == 'b' || next == 'B') {
            advance(s); advance(s);
            if (!isBinDigit(currentChar(s))) {
                reportError(s, DiagCode::Lex_InvalidRadixLiteral,
                            "binary literal has no digits after '0b'");
                s.tokens.push_back(makeToken(
                    TokenType::UNKNOWN,
                    std::string(s.source.substr(startPos, s.position - startPos)),
                    startLoc));
                return;
            }
            while (isBinDigit(currentChar(s))) advance(s);
            s.tokens.push_back(makeToken(
                TokenType::BINARY_LITERAL,
                std::string(s.source.substr(startPos, s.position - startPos)),
                startLoc));
            return;
        }
        if (next == 'o' || next == 'O') {
            advance(s); advance(s);
            if (!isOctDigit(currentChar(s))) {
                reportError(s, DiagCode::Lex_InvalidRadixLiteral,
                            "octal literal has no digits after '0o'");
                s.tokens.push_back(makeToken(
                    TokenType::UNKNOWN,
                    std::string(s.source.substr(startPos, s.position - startPos)),
                    startLoc));
                return;
            }
            while (isOctDigit(currentChar(s))) advance(s);
            s.tokens.push_back(makeToken(
                TokenType::OCTAL_LITERAL,
                std::string(s.source.substr(startPos, s.position - startPos)),
                startLoc));
            return;
        }
    }

    // Decimal integer part.
    while (isDigit(currentChar(s))) advance(s);

    bool isFloat = false;

    // Fractional part: `.` followed by a digit. A bare `.` is not part
    // of the number — `1.method()` lexes as `1`, `.`, `method`, `(`, `)`.
    if (currentChar(s) == '.' && isDigit(peekChar(s, 1))) {
        isFloat = true;
        advance(s);  // '.'
        while (isDigit(currentChar(s))) advance(s);
    }

    // Exponent part.
    if (currentChar(s) == 'e' || currentChar(s) == 'E') {
        isFloat = true;
        advance(s);
        if (currentChar(s) == '+' || currentChar(s) == '-') advance(s);
        if (!isDigit(currentChar(s))) {
            reportError(s, DiagCode::Lex_InvalidNumberLiteral,
                        "exponent has no digits");
            s.tokens.push_back(makeToken(
                TokenType::UNKNOWN,
                std::string(s.source.substr(startPos, s.position - startPos)),
                startLoc));
            return;
        }
        while (isDigit(currentChar(s))) advance(s);
    }

    s.tokens.push_back(makeToken(
        isFloat ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL,
        std::string(s.source.substr(startPos, s.position - startPos)),
        startLoc));
}

// ─────────────────────────────────────────────────────────────────────────────
// Strings
// ─────────────────────────────────────────────────────────────────────────────
//
// Two forms:
//
//   "..."          a normal string; escapes are processed, no newlines.
//   """..."""      a raw string; no escapes, no interpolation, newlines
//                  allowed. The only sequence it cannot contain is `"""`.
//
// The grammar has no string interpolation, so a normal string is one
// token and its content is the fully-processed text.

/// @brief Lex a `"..."` normal string.
///
/// The cursor is on the opening `"`. On return, the cursor is past the
/// closing `"` (or at the offending newline/EOF for a malformed string).
void lexString(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s);  // opening `"`

    std::string content;

    while (!isAtEnd(s)) {
        const char c = currentChar(s);

        if (c == '"') {
            advance(s);  // closing `"`
            s.tokens.push_back(makeToken(TokenType::STRING_LITERAL,
                                         std::move(content), startLoc));
            return;
        }

        if (c == '\n') {
            reportError(s, DiagCode::Lex_NewlineInString,
                        "a normal string literal cannot contain a newline; "
                        "use \"\"\" for a multi-line raw string");
            s.tokens.push_back(makeToken(TokenType::UNKNOWN,
                                         std::move(content), startLoc));
            return;
        }

        if (c == '\\') {
            const char next = peekChar(s, 1);
            switch (next) {
                case 'n':  content += '\n'; advance(s); advance(s); break;
                case 't':  content += '\t'; advance(s); advance(s); break;
                case 'r':  content += '\r'; advance(s); advance(s); break;
                case '\\': content += '\\'; advance(s); advance(s); break;
                case '"':  content += '"';  advance(s); advance(s); break;
                case '\'': content += '\''; advance(s); advance(s); break;
                case '0':  content += '\0'; advance(s); advance(s); break;
                default:
                    reportError(s, DiagCode::Lex_InvalidEscapeSequence,
                                std::string("unknown escape '\\") + next + "'");
                    advance(s);
                    if (!isAtEnd(s)) advance(s);
                    return;
            }
            continue;
        }

        content += c;
        advance(s);
    }

    reportError(s, DiagCode::Lex_UnterminatedString,
                "unterminated string literal");
    s.tokens.push_back(makeToken(TokenType::UNKNOWN,
                                 std::move(content), startLoc));
}

/// @brief Lex a `"""..."""` raw string.
///
/// One token, no escape processing, no newline restriction. The only
/// sequence it cannot contain is `"""` itself.
void lexRawString(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s); advance(s); advance(s);  // opening `"""`

    const size_t contentStart = s.position;
    while (!isAtEnd(s)) {
        if (currentChar(s) == '"' &&
            peekChar(s, 1) == '"' &&
            peekChar(s, 2) == '"') {
            const std::string_view content =
                s.source.substr(contentStart, s.position - contentStart);
            advance(s); advance(s); advance(s);  // closing `"""`
            s.tokens.push_back(makeToken(TokenType::RAW_STRING_LITERAL,
                                         std::string(content), startLoc));
            return;
        }
        advance(s);
    }

    reportError(s, DiagCode::Lex_UnterminatedRawString,
                "unterminated raw string (expected \"\"\")");
    s.tokens.push_back(makeToken(
        TokenType::UNKNOWN,
        std::string(s.source.substr(contentStart, s.position - contentStart)),
        startLoc));
}

// ─────────────────────────────────────────────────────────────────────────────
// Character literals
// ─────────────────────────────────────────────────────────────────────────────

void lexChar(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s);  // opening `'`

    if (isAtEnd(s)) {
        reportError(s, DiagCode::Lex_UnterminatedCharLiteral,
                    "unterminated character literal");
        s.tokens.push_back(makeToken(TokenType::UNKNOWN, std::string{},
                                     startLoc));
        return;
    }

    std::string value;

    if (currentChar(s) == '\\') {
        advance(s);
        if (isAtEnd(s)) {
            reportError(s, DiagCode::Lex_UnterminatedCharLiteral,
                        "unterminated character literal");
            s.tokens.push_back(makeToken(TokenType::UNKNOWN,
                                         std::move(value), startLoc));
            return;
        }
        const char next = currentChar(s);
        switch (next) {
            case 'n':  value = "\\n";  break;
            case 't':  value = "\\t";  break;
            case 'r':  value = "\\r";  break;
            case '\\': value = "\\\\"; break;
            case '\'': value = "\\'";  break;
            case '"':  value = "\\\""; break;
            case '0':  value = "\\0";  break;
            default:
                reportError(s, DiagCode::Lex_InvalidEscapeSequence,
                            std::string("unknown escape '\\") + next + "'");
                advance(s);
                s.tokens.push_back(makeToken(TokenType::UNKNOWN,
                                             std::move(value), startLoc));
                return;
        }
        advance(s);
    } else {
        value = std::string(1, currentChar(s));
        advance(s);
    }

    if (isAtEnd(s) || currentChar(s) != '\'') {
        reportError(s, DiagCode::Lex_UnterminatedCharLiteral,
                    "unterminated character literal");
        s.tokens.push_back(makeToken(TokenType::UNKNOWN,
                                     std::move(value), startLoc));
        return;
    }

    advance(s);  // closing `'`
    s.tokens.push_back(makeToken(TokenType::CHAR_LITERAL, std::move(value),
                                 startLoc));
}

// ─────────────────────────────────────────────────────────────────────────────
// Comments
// ─────────────────────────────────────────────────────────────────────────────
//
// Line and block comments are dropped entirely; the token vector never
// contains them. The doc-comment form `/-- ... --/` is the one
// exception: it survives as a DOC_COMMENT token, because the parser's
// doc-comment harvester needs to see it.

/// @brief Consume to end of line (exclusive). The `--` has been consumed.
void skipLineComment(LexerState& s) noexcept {
    while (!isAtEnd(s) && currentChar(s) != '\n') advance(s);
}

/// @brief Consume a block comment. The `/-` has been consumed.
///
/// Returns the comment's text if `isDoc` is true (the caller has also
/// consumed the extra `-` of `/--`), otherwise returns an empty string
/// and just advances the cursor. `terminated` is set to true if a
/// matching `-/` was found.
std::string readBlockComment(LexerState& s, bool isDoc, bool& terminated) {
    std::string body;
    int depth = 1;
    terminated = false;

    while (!isAtEnd(s)) {
        if (currentChar(s) == '/' && peekChar(s, 1) == '-') {
            depth++;
            if (isDoc) { body += '/'; body += '-'; }
            advance(s); advance(s);
            continue;
        }
        if (currentChar(s) == '-' && peekChar(s, 1) == '/') {
            depth--;
            if (depth > 0) {
                if (isDoc) { body += '-'; body += '/'; }
                advance(s); advance(s);
            } else {
                advance(s); advance(s);
                terminated = true;
                return body;
            }
            continue;
        }
        if (isDoc) body += currentChar(s);
        advance(s);
    }

    return body;
}

// ─────────────────────────────────────────────────────────────────────────────
// Operators and punctuation
// ─────────────────────────────────────────────────────────────────────────────

void lexOperatorOrPunctuation(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const char c    = currentChar(s);
    const char next = peekChar(s, 1);

    // Helper: consume `n` characters and emit a token of the given type
    // with the given spelling.
    auto emit = [&](TokenType type, const char* spelling, size_t n) {
        for (size_t i = 0; i < n; ++i) advance(s);
        s.tokens.push_back(makeToken(type, std::string(spelling), startLoc));
    };

    // ─── Three-character operators ──────────────────────────────────────
    if (c == '<' && next == '<' && peekChar(s, 2) == '=') {
        emit(TokenType::SHL_ASSIGN, "<<=", 3); return;
    }
    if (c == '>' && next == '>' && peekChar(s, 2) == '=') {
        emit(TokenType::SHR_ASSIGN, ">>=", 3); return;
    }
    if (c == '.' && next == '.' && peekChar(s, 2) == '.') {
        emit(TokenType::VARIADIC, "...", 3); return;
    }
    if (c == '.' && next == '.' && peekChar(s, 2) == '<') {
        emit(TokenType::RANGE_EXCLUSIVE, "..<", 3); return;
    }

    // ─── Two-character operators ────────────────────────────────────────
    if (c == '*' && next == '*') { emit(TokenType::POW, "**", 2); return; }
    if (c == '<' && next == '<') { emit(TokenType::SHL, "<<", 2); return; }
    if (c == '>' && next == '>') { emit(TokenType::SHR, ">>", 2); return; }
    if (c == '.' && next == '.') { emit(TokenType::RANGE, "..", 2); return; }
    if (c == '-' && next == '>') { emit(TokenType::ARROW, "->", 2); return; }
    if (c == '=' && next == '=') { emit(TokenType::EQUAL_EQUAL, "==", 2); return; }
    if (c == '!' && next == '=') { emit(TokenType::NOT_EQUAL, "!=", 2); return; }
    if (c == '<' && next == '=') { emit(TokenType::LESS_EQUAL, "<=", 2); return; }
    if (c == '>' && next == '=') { emit(TokenType::GREATER_EQUAL, ">=", 2); return; }
    if (c == '?' && next == '?') { emit(TokenType::QUESTION_QUESTION, "??", 2); return; }

    // Compound assignment: single-char operator followed by `=`.
    if (next == '=') {
        switch (c) {
            case '+': emit(TokenType::PLUS_ASSIGN,    "+=", 2); return;
            case '-': emit(TokenType::MINUS_ASSIGN,   "-=", 2); return;
            case '*': emit(TokenType::MUL_ASSIGN,     "*=", 2); return;
            case '/': emit(TokenType::DIV_ASSIGN,     "/=", 2); return;
            case '%': emit(TokenType::MOD_ASSIGN,     "%=", 2); return;
            case '&': emit(TokenType::BIT_AND_ASSIGN, "&=", 2); return;
            case '|': emit(TokenType::BIT_OR_ASSIGN,  "|=", 2); return;
            case '^': emit(TokenType::BIT_XOR_ASSIGN, "^=", 2); return;
            default: break;
        }
    }

    // ─── Single-character tokens ────────────────────────────────────────
    switch (c) {
        case '+': emit(TokenType::PLUS,        "+", 1); return;
        case '-': emit(TokenType::MINUS,       "-", 1); return;
        case '*': emit(TokenType::MUL,         "*", 1); return;
        case '/': emit(TokenType::DIV,         "/", 1); return;
        case '%': emit(TokenType::MOD,         "%", 1); return;
        case '<': emit(TokenType::LESS,        "<", 1); return;
        case '>': emit(TokenType::GREATER,     ">", 1); return;
        case '=': emit(TokenType::ASSIGN,      "=", 1); return;
        case '&': emit(TokenType::BIT_AND,     "&", 1); return;
        case '|': emit(TokenType::BIT_OR,      "|", 1); return;
        case '^': emit(TokenType::BIT_XOR,     "^", 1); return;
        case '~': emit(TokenType::BIT_NOT,     "~", 1); return;
        case ':': emit(TokenType::COLON,       ":", 1); return;
        case ',': emit(TokenType::COMMA,       ",", 1); return;
        case ';': emit(TokenType::SEMICOLON,   ";", 1); return;
        case '(': emit(TokenType::LPAREN,      "(", 1); return;
        case ')': emit(TokenType::RPAREN,      ")", 1); return;
        case '{': emit(TokenType::LBRACE,      "{", 1); return;
        case '}': emit(TokenType::RBRACE,      "}", 1); return;
        case '[': emit(TokenType::LBRACKET,    "[", 1); return;
        case ']': emit(TokenType::RBRACKET,    "]", 1); return;
        case '.': emit(TokenType::DOT,         ".", 1); return;
        case '@': emit(TokenType::AT_SIGN,     "@", 1); return;
        default:  break;
    }

    // Unknown character.
    reportError(s, DiagCode::Lex_UnknownCharacter,
                std::string("unexpected character '") + c + "'");
    advance(s);
    s.tokens.push_back(makeToken(TokenType::UNKNOWN, std::string(1, c),
                                 startLoc));
}

// ─────────────────────────────────────────────────────────────────────────────
// The main dispatch
// ─────────────────────────────────────────────────────────────────────────────
//
// Lexes one token and appends it to `s.tokens`. The main loop in
// `tokenize` calls this until the cursor reaches end-of-input, at which
// point it appends the EOF token.

void lexOne(LexerState& s) {
    skipWhitespace(s);

    if (isAtEnd(s)) {
        s.tokens.push_back(makeToken(TokenType::EOF_TOKEN, std::string{},
                                     currentLocation(s)));
        return;
    }

    const char c    = currentChar(s);
    const char next = peekChar(s, 1);

    // ─── Comments ───────────────────────────────────────────────────────
    // Order matters: `/--` is a doc comment, `/-` is a block comment, `--`
    // is a line comment. Check the longest prefix first.

    if (c == '/' && next == '-' && peekChar(s, 2) == '-') {
        const SourceLocation startLoc = currentLocation(s);
        advance(s); advance(s); advance(s);  // consume `/--`
        bool terminated = false;
        std::string body = readBlockComment(s, /*isDoc=*/true, terminated);
        if (!terminated) {
            reportError(s, DiagCode::Lex_UnterminatedBlockComment,
                        "unterminated documentation comment (expected --/)");
            return;
        }
        s.tokens.push_back(makeToken(TokenType::DOC_COMMENT, std::move(body),
                                     startLoc));
        return;
    }
    if (c == '/' && next == '-') {
        advance(s); advance(s);  // consume `/-`
        bool terminated = false;
        (void)readBlockComment(s, /*isDoc=*/false, terminated);
        if (!terminated) {
            reportError(s, DiagCode::Lex_UnterminatedBlockComment,
                        "unterminated block comment (expected -/)");
        }
        return;  // ordinary block comments are dropped
    }
    if (c == '-' && next == '-') {
        advance(s); advance(s);  // consume `--`
        skipLineComment(s);
        return;  // line comments are dropped
    }

    // ─── Identifiers and keywords ───────────────────────────────────────
    if (isIdentifierStart(c)) {
        lexIdentifier(s);
        return;
    }

    // ─── Numbers ────────────────────────────────────────────────────────
    // A `.` followed by a digit is a float literal; a `.` not followed by
    // a digit is DOT. The check below handles both.
    if (isDigit(c) || (c == '.' && isDigit(next))) {
        lexNumber(s);
        return;
    }

    // ─── Strings ────────────────────────────────────────────────────────
    if (c == '"') {
        if (next == '"' && peekChar(s, 2) == '"') {
            lexRawString(s);
            return;
        }
        lexString(s);
        return;
    }

    // ─── Char literal ───────────────────────────────────────────────────
    if (c == '\'') {
        lexChar(s);
        return;
    }

    // ─── Operator or punctuation ────────────────────────────────────────
    lexOperatorOrPunctuation(s);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Token> tokenize(std::string_view source,
                            lucid::diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, diagnostics);

    while (true) {
        lexOne(s);
        if (!s.tokens.empty() && s.tokens.back().type == TokenType::EOF_TOKEN) {
            break;
        }
    }

    return std::move(s.tokens);
}

std::vector<Token> tokenize_n(std::string_view source,
                              size_t max_tokens,
                              lucid::diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, diagnostics);

    for (size_t i = 0; i < max_tokens; ++i) {
        lexOne(s);
        if (!s.tokens.empty() && s.tokens.back().type == TokenType::EOF_TOKEN) {
            break;
        }
    }

    return std::move(s.tokens);
}

} // namespace lucid::lexer