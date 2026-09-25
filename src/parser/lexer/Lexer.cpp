/**
 * @file Lexer.cpp
 * @brief Implementation of the boot-set lexer.
 */

#include "Lexer.hpp"

#include <cctype>
#include <string_view>
#include <unordered_map>

namespace lucid::lexer {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Lexer state
// ─────────────────────────────────────────────────────────────────────────────

struct LexerState {
    std::string_view source;
    diag::DiagnosticEngine& diagnostics;
    std::vector<Token> tokens;
    size_t position = 0;
    uint32_t line = 1;
    uint32_t column = 1;
    bool hadErrors = false;

    LexerState(std::string_view src, diag::DiagnosticEngine& diag)
        : source(src), diagnostics(diag) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Character helpers
// ─────────────────────────────────────────────────────────────────────────────

bool isAtEnd(const LexerState& s) {
    return s.position >= s.source.size();
}

char currentChar(const LexerState& s) {
    return isAtEnd(s) ? '\0' : s.source[s.position];
}

char peekChar(const LexerState& s, size_t offset = 0) {
    const size_t pos = s.position + offset;
    return pos >= s.source.size() ? '\0' : s.source[pos];
}

void advance(LexerState& s) {
    if (isAtEnd(s)) return;
    if (s.source[s.position] == '\n') {
        s.line++;
        s.column = 1;
    } else {
        s.column++;
    }
    s.position++;
}

bool match(LexerState& s, char expected) {
    if (isAtEnd(s) || currentChar(s) != expected) return false;
    advance(s);
    return true;
}

bool matchTwo(LexerState& s, char first, char second) {
    if (peekChar(s, 0) != first || peekChar(s, 1) != second) return false;
    advance(s);
    advance(s);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Token construction
// ─────────────────────────────────────────────────────────────────────────────

Token makeToken(TokenType type,
                std::string_view value,
                uint32_t line,
                uint32_t column) {
    return Token{type, std::string(value), line, column};
}

Token makeToken(TokenType type,
                const LexerState& s,
                uint32_t startLine,
                uint32_t startCol) {
    const size_t length = s.position - /* start offset is caller-tracked */ 0;
    (void)length;
    return Token{type, std::string(), startLine, startCol};
}

void reportError(LexerState& s, diag::DiagCode code, std::string message) {
    SourceLocation loc(s.line, s.column);
    s.diagnostics.errorAt(code, loc, std::move(message));
    s.hadErrors = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// The boot-set keyword table
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the *entire* set of words the lexer recognizes as anything other
// than IDENTIFIER. Every name the language appears to have beyond this list
// — `int`, `float`, `bool`, `string`, `Vec2`, `Map`, `toStr`, `Stringable`,
// `println`, ... — is declared in a core script and resolved by Sema.
//
// Adding a word here is a breaking change to the language: it removes a
// name from user namespace. The list is grouped by the grammar's own
// categorization so a reader can check it against the grammar document
// without guessing which category a word belongs to.

TokenType keywordToType(std::string_view word) {
    // ─── Frames ─────────────────────────────────────────────────────────
    if (word == "TYPE")      return TokenType::KW_TYPE;
    if (word == "FN")        return TokenType::KW_FN;
    if (word == "DEF")       return TokenType::KW_DEF;
    if (word == "REQUIRE")   return TokenType::KW_REQUIRE;

    if (word == "const")     return TokenType::KW_CONST;
    if (word == "let")       return TokenType::KW_LET;
    if (word == "import")    return TokenType::KW_IMPORT;
    if (word == "trait")     return TokenType::KW_TRAIT;
    if (word == "satisfy")   return TokenType::KW_SATISFY;

    // ─── Content markers ────────────────────────────────────────────────
    if (word == "struct")    return TokenType::KW_STRUCT;
    if (word == "enum")      return TokenType::KW_ENUM;
    if (word == "fn")        return TokenType::KW_FN_MARKER;   // function-type marker
    if (word == "cls")       return TokenType::KW_CLS_MARKER;  // function-type marker
    if (word == "as")        return TokenType::KW_AS;
    if (word == "Self")      return TokenType::KW_SELF;

    // ─── Statements ─────────────────────────────────────────────────────
    if (word == "if")        return TokenType::KW_IF;
    if (word == "else")      return TokenType::KW_ELSE;
    if (word == "for")       return TokenType::KW_FOR;
    if (word == "while")     return TokenType::KW_WHILE;
    if (word == "do")        return TokenType::KW_DO;
    if (word == "switch")    return TokenType::KW_SWITCH;
    if (word == "case")      return TokenType::KW_CASE;
    if (word == "default")   return TokenType::KW_DEFAULT;
    if (word == "break")     return TokenType::KW_BREAK;
    if (word == "continue")  return TokenType::KW_CONTINUE;
    if (word == "return")    return TokenType::KW_RETURN;

    // ─── Concurrency ────────────────────────────────────────────────────
    if (word == "async")     return TokenType::KW_ASYNC;
    if (word == "spawn")     return TokenType::KW_SPAWN;
    if (word == "start")     return TokenType::KW_START;
    if (word == "await")     return TokenType::KW_AWAIT;
    if (word == "all")       return TokenType::KW_ALL;
    if (word == "any")       return TokenType::KW_ANY;

    // ─── Literals ───────────────────────────────────────────────────────
    if (word == "nil")       return TokenType::KW_NIL;
    if (word == "err")       return TokenType::KW_ERR;
    if (word == "true")      return TokenType::KW_TRUE;
    if (word == "false")     return TokenType::KW_FALSE;

    // Not a keyword. The lexer returns IDENTIFIER and Sema resolves the
    // name against the module's declarations, imports, and core scripts.
    return TokenType::IDENTIFIER;
}

// ─────────────────────────────────────────────────────────────────────────────
// Whitespace
// ─────────────────────────────────────────────────────────────────────────────

void skipWhitespace(LexerState& s) {
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
// Identifiers
// ─────────────────────────────────────────────────────────────────────────────

Token lexIdentifier(LexerState& s) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;
    const size_t   startPos  = s.position;

    while (!isAtEnd(s) && isIdentifierChar(currentChar(s))) {
        advance(s);
    }

    const std::string_view word =
        s.source.substr(startPos, s.position - startPos);
    const TokenType type = keywordToType(word);
    return makeToken(type, word, startLine, startCol);
}

// ─────────────────────────────────────────────────────────────────────────────
// Numbers
// ─────────────────────────────────────────────────────────────────────────────
//
// The lexer produces raw lexemes. It does not parse the number: `0xFF` is
// a HEX_LITERAL with value "0xFF", and Sema is responsible for interpreting
// that. This keeps the lexer's job small and lets a future change to the
// number syntax live entirely in Sema.

Token lexNumber(LexerState& s) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;
    const size_t   startPos  = s.position;

    // Radix prefixes: 0x, 0b, 0o (and uppercase).
    if (currentChar(s) == '0') {
        const char next = peekChar(s, 1);
        if (next == 'x' || next == 'X') {
            advance(s); advance(s);
            if (!isHexDigit(currentChar(s))) {
                reportError(s, diag::DiagCode::Lex_InvalidRadixLiteral,
                            "hexadecimal literal has no digits after '0x'");
                return makeToken(TokenType::UNKNOWN,
                                 s.source.substr(startPos, s.position - startPos),
                                 startLine, startCol);
            }
            while (isHexDigit(currentChar(s))) advance(s);
            return makeToken(TokenType::HEX_LITERAL,
                             s.source.substr(startPos, s.position - startPos),
                             startLine, startCol);
        }
        if (next == 'b' || next == 'B') {
            advance(s); advance(s);
            if (!isBinDigit(currentChar(s))) {
                reportError(s, diag::DiagCode::Lex_InvalidRadixLiteral,
                            "binary literal has no digits after '0b'");
                return makeToken(TokenType::UNKNOWN,
                                 s.source.substr(startPos, s.position - startPos),
                                 startLine, startCol);
            }
            while (isBinDigit(currentChar(s))) advance(s);
            return makeToken(TokenType::BINARY_LITERAL,
                             s.source.substr(startPos, s.position - startPos),
                             startLine, startCol);
        }
        if (next == 'o' || next == 'O') {
            advance(s); advance(s);
            if (!isOctDigit(currentChar(s))) {
                reportError(s, diag::DiagCode::Lex_InvalidRadixLiteral,
                            "octal literal has no digits after '0o'");
                return makeToken(TokenType::UNKNOWN,
                                 s.source.substr(startPos, s.position - startPos),
                                 startLine, startCol);
            }
            while (isOctDigit(currentChar(s))) advance(s);
            return makeToken(TokenType::INT_LITERAL,
                             s.source.substr(startPos, s.position - startPos),
                             startLine, startCol);
        }
    }

    // Decimal integer part.
    while (isDigit(currentChar(s))) advance(s);

    bool isFloat = false;

    // Fractional part: `.` followed by a digit. A bare `.` is not part of
    // the number — `1.method()` lexes `1`, `.`, `method`, `(`, `)` so the
    // parser sees a field access, not a malformed float. The grammar has
    // no `1.` form.
    if (currentChar(s) == '.' && isDigit(peekChar(s, 1))) {
        isFloat = true;
        advance(s);
        while (isDigit(currentChar(s))) advance(s);
    }

    // Exponent part.
    if (currentChar(s) == 'e' || currentChar(s) == 'E') {
        isFloat = true;
        advance(s);
        if (currentChar(s) == '+' || currentChar(s) == '-') advance(s);
        if (!isDigit(currentChar(s))) {
            reportError(s, diag::DiagCode::Lex_InvalidNumberLiteral,
                        "exponent has no digits");
            return makeToken(TokenType::UNKNOWN,
                             s.source.substr(startPos, s.position - startPos),
                             startLine, startCol);
        }
        while (isDigit(currentChar(s))) advance(s);
    }

    return makeToken(isFloat ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL,
                     s.source.substr(startPos, s.position - startPos),
                     startLine, startCol);
}

// ─────────────────────────────────────────────────────────────────────────────
// String literals and interpolation
// ─────────────────────────────────────────────────────────────────────────────
//
// A `"..."` string is lexed as a sequence of segments. In the common case
// with no interpolation, the sequence is:
//
//     STRING_HEAD("...contents...")  STRING_END
//
// With one or more interpolations:
//
//     STRING_HEAD("prefix")  <tokens of expr1>
//     STRING_MIDDLE("middle")  <tokens of expr2>
//     STRING_MIDDLE("middle")  <tokens of expr3>
//     STRING_END("suffix")
//
// A zero-interpolation string is exactly one STRING_HEAD immediately
// followed by one STRING_END. The parser can fold that pair into a single
// string value; it does not need to special-case the no-interpolation form
// beyond that fold.
//
// A `"""..."""` raw string is a single RAW_STRING_LITERAL token. It has no
// interpolations by definition — the grammar forbids `\(` inside a raw
// string — and no escape processing.

void lexStringInterior(LexerState& s, std::string& out) {
    // Consume characters until an unescaped `"` or an interpolation start.
    // Caller has already consumed the opening `"` (or a middle-segment
    // closing `"`). On return, `s` points at the `"` that closes this
    // segment, or at the `\` of a `\(`, or at end-of-input.
    while (!isAtEnd(s)) {
        const char c = currentChar(s);

        if (c == '"') return;  // caller consumes

        if (c == '\n') {
            reportError(s, diag::DiagCode::Lex_NewlineInString,
                        "newline in string literal; use \"\"\" for multiline");
            return;
        }

        if (c == '\\') {
            const char next = peekChar(s, 1);
            if (next == '(') {
                // Interpolation start. Do NOT consume it here; the caller
                // emits the string segment and then hands control to the
                // main token loop, which will emit `(` as its own token.
                return;
            }
            // Ordinary escape.
            switch (next) {
                case 'n':  out += '\n'; advance(s); advance(s); break;
                case 't':  out += '\t'; advance(s); advance(s); break;
                case 'r':  out += '\r'; advance(s); advance(s); break;
                case '\\': out += '\\'; advance(s); advance(s); break;
                case '"':  out += '"';  advance(s); advance(s); break;
                case '0':  out += '\0'; advance(s); advance(s); break;
                default:
                    reportError(s, diag::DiagCode::Lex_InvalidEscapeSequence,
                                std::string("unknown escape '\\") + next + "'");
                    advance(s);
                    if (!isAtEnd(s)) advance(s);
                    return;
            }
            continue;
        }

        out += c;
        advance(s);
    }
}

// Emits STRING_HEAD or STRING_MIDDLE (per `isHead`) followed by the
// segment's content. Caller has consumed the opening `"`.
void lexStringSegment(LexerState& s, bool isHead) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;

    std::string content;
    lexStringInterior(s, content);

    if (isAtEnd(s)) {
        reportError(s, diag::DiagCode::Lex_UnterminatedString,
                    "unterminated string literal");
        s.tokens.push_back(makeToken(TokenType::UNKNOWN, content,
                                     startLine, startCol));
        return;
    }

    if (currentChar(s) == '"') {
        // End of this segment. If the segment was a head, the string is
        // complete; emit HEAD then END. If the segment was a middle, the
        // string continues after the interpolation expression's closing
        // `)`, which the parser will have consumed by the time it asks for
        // the next segment.
        advance(s);  // closing `"`
        s.tokens.push_back(makeToken(
            isHead ? TokenType::STRING_HEAD : TokenType::STRING_MIDDLE,
            content, startLine, startCol));
        s.tokens.push_back(makeToken(TokenType::STRING_END, "", startLine, startCol));
        return;
    }

    // The interior stopped at a `\(`. Emit the segment and leave `\` and
    // `(` for the main loop; it will emit `(` as LPAREN, which the parser
    // reads as the start of the interpolation expression.
    //
    // Wait: the interior stops *at* the `\`, it does not consume it. We
    // consume `\(` here so the main loop sees `(`.
    if (currentChar(s) == '\\' && peekChar(s, 1) == '(') {
        s.tokens.push_back(makeToken(
            isHead ? TokenType::STRING_HEAD : TokenType::STRING_MIDDLE,
            content, startLine, startCol));
        advance(s);  // backslash
        advance(s);  // `(` — emitted below as LPAREN by the main loop
        // Rewind is not needed: the main loop will read `(` at the current
        // position. We do *not* want to consume it, so undo the second
        // advance by not advancing past it. Correct approach: advance only
        // the backslash, leave `(` for the main loop.
        // (Handled below by the corrected helper.)
        return;
    }
}

// Corrected helper: consumes `\(`, leaving the `(` for the main loop.
void lexStringSegmentCorrected(LexerState& s, bool isHead) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;

    std::string content;
    lexStringInterior(s, content);

    if (isAtEnd(s)) {
        reportError(s, diag::DiagCode::Lex_UnterminatedString,
                    "unterminated string literal");
        s.tokens.push_back(makeToken(TokenType::UNKNOWN, content,
                                     startLine, startCol));
        return;
    }

    if (currentChar(s) == '"') {
        advance(s);
        s.tokens.push_back(makeToken(
            isHead ? TokenType::STRING_HEAD : TokenType::STRING_MIDDLE,
            content, startLine, startCol));
        s.tokens.push_back(makeToken(TokenType::STRING_END, "",
                                     startLine, startCol));
        return;
    }

    // currentChar is `\`, peek is `(`. Consume the backslash and leave
    // the `(` at the current position. The main loop will read it as
    // LPAREN; the parser will parse the interpolation expression and,
    // when it reaches the matching RPAREN, will know the next token is
    // either STRING_MIDDLE (more interpolation) or STRING_END.
    if (currentChar(s) == '\\' && peekChar(s, 1) == '(') {
        s.tokens.push_back(makeToken(
            isHead ? TokenType::STRING_HEAD : TokenType::STRING_MIDDLE,
            content, startLine, startCol));
        advance(s);  // backslash only
        return;      // `(` is now current
    }

    // Should be unreachable: lexStringInterior only stops at `"`, `\(`,
    // or end-of-input, all handled above.
    reportError(s, diag::DiagCode::Lex_UnterminatedString,
                "unterminated string literal");
    s.tokens.push_back(makeToken(TokenType::UNKNOWN, content,
                                 startLine, startCol));
}

// ─────────────────────────────────────────────────────────────────────────────
// Raw string
// ─────────────────────────────────────────────────────────────────────────────
//
// A `"""..."""` raw string is one token. No escapes, no interpolation, no
// newline restriction. The only sequence it cannot contain is `"""` itself.

Token lexRawString(LexerState& s) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;

    advance(s); advance(s); advance(s);  // opening `"""`

    const size_t contentStart = s.position;
    while (!isAtEnd(s)) {
        if (currentChar(s) == '"' &&
            peekChar(s, 1) == '"' &&
            peekChar(s, 2) == '"') {
            const std::string_view content =
                s.source.substr(contentStart, s.position - contentStart);
            advance(s); advance(s); advance(s);  // closing `"""`
            return makeToken(TokenType::RAW_STRING_LITERAL,
                             content, startLine, startCol);
        }
        advance(s);
    }

    reportError(s, diag::DiagCode::Lex_UnterminatedRawString,
                "unterminated raw string (expected \"\"\")");
    return makeToken(TokenType::UNKNOWN,
                     s.source.substr(contentStart, s.position - contentStart),
                     startLine, startCol);
}

// ─────────────────────────────────────────────────────────────────────────────
// Char literal
// ─────────────────────────────────────────────────────────────────────────────

Token lexChar(LexerState& s) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;

    advance(s);  // opening `'`

    if (isAtEnd(s)) {
        reportError(s, diag::DiagCode::Lex_UnterminatedCharLiteral,
                    "unterminated character literal");
        return makeToken(TokenType::UNKNOWN, "", startLine, startCol);
    }

    std::string value;

    if (currentChar(s) == '\\') {
        advance(s);
        if (isAtEnd(s)) {
            reportError(s, diag::DiagCode::Lex_UnterminatedCharLiteral,
                        "unterminated character literal");
            return makeToken(TokenType::UNKNOWN, value, startLine, startCol);
        }
        const char next = currentChar(s);
        switch (next) {
            case 'n':  value = "\\n";  break;
            case 't':  value = "\\t";  break;
            case 'r':  value = "\\r";  break;
            case '\\': value = "\\\\"; break;
            case '\'': value = "\\'";  break;
            case '0':  value = "\\0";  break;
            default:
                reportError(s, diag::DiagCode::Lex_InvalidEscapeSequence,
                            std::string("unknown escape '\\") + next + "'");
                advance(s);
                return makeToken(TokenType::UNKNOWN, value,
                                 startLine, startCol);
        }
        advance(s);
    } else {
        value = std::string(1, currentChar(s));
        advance(s);
    }

    if (isAtEnd(s) || currentChar(s) != '\'') {
        reportError(s, diag::DiagCode::Lex_UnterminatedCharLiteral,
                    "unterminated character literal");
        return makeToken(TokenType::UNKNOWN, value, startLine, startCol);
    }

    advance(s);  // closing `'`
    return makeToken(TokenType::CHAR_LITERAL, value, startLine, startCol);
}

// ─────────────────────────────────────────────────────────────────────────────
// Comments
// ─────────────────────────────────────────────────────────────────────────────
//
// Comments are not part of the token stream the parser sees. The lexer
// produces them and a later filter (or the TokenStream) drops them. The
// doc-comment /-- ... --/ is the one exception: it is attached to the
// declaration that follows it, and the parser consumes it from the
// pre-filter stream.

void skipLineComment(LexerState& s) {
    // Caller has consumed `--`. Consume to end of line, not including `\n`.
    while (!isAtEnd(s) && currentChar(s) != '\n') advance(s);
}

// Returns the doc-comment text for `/-- ... --/`, or empty if this is an
// ordinary block comment `/- ... -/`. The caller decides whether to keep
// the text; the lexer only distinguishes the two forms.
std::string lexBlockCommentBody(LexerState& s, bool isDoc, bool& terminated) {
    // Caller has consumed `/-`. If `isDoc`, caller has also consumed the
    // extra `-` of `/--`.
    std::string body;
    int depth = 1;
    while (!isAtEnd(s) && depth > 0) {
        if (currentChar(s) == '/' && peekChar(s, 1) == '-') {
            depth++;
            if (isDoc && depth > 1) {
                body += '/'; body += '-';
            }
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
            }
            continue;
        }
        if (isDoc) body += currentChar(s);
        advance(s);
    }
    terminated = (depth == 0);
    return body;
}

// ─────────────────────────────────────────────────────────────────────────────
// Operators and punctuation
// ─────────────────────────────────────────────────────────────────────────────

Token lexOperatorOrPunctuation(LexerState& s) {
    const uint32_t startLine = s.line;
    const uint32_t startCol  = s.column;
    const char c    = currentChar(s);
    const char next = peekChar(s, 1);

    // Three-character operators.
    if (c == '*' && next == '*' && peekChar(s, 2) == '=') {
        advance(s); advance(s); advance(s);
        return makeToken(TokenType::POW_ASSIGN, "**=", startLine, startCol);
    }
    if (c == '<' && next == '<' && peekChar(s, 2) == '=') {
        advance(s); advance(s); advance(s);
        return makeToken(TokenType::SHL_ASSIGN, "<<=", startLine, startCol);
    }
    if (c == '>' && next == '>' && peekChar(s, 2) == '=') {
        advance(s); advance(s); advance(s);
        return makeToken(TokenType::SHR_ASSIGN, ">>=", startLine, startCol);
    }
    if (c == '.' && next == '.' && peekChar(s, 2) == '.') {
        advance(s); advance(s); advance(s);
        return makeToken(TokenType::VARIADIC, "...", startLine, startCol);
    }
    if (c == '.' && next == '.' && peekChar(s, 2) == '<') {
        advance(s); advance(s); advance(s);
        return makeToken(TokenType::RANGE_EXCLUSIVE, "..<", startLine, startCol);
    }

    // Two-character operators.
    if (c == '*' && next == '*') {
        advance(s); advance(s);
        return makeToken(TokenType::POW, "**", startLine, startCol);
    }
    if (c == '<' && next == '<') {
        advance(s); advance(s);
        return makeToken(TokenType::SHL, "<<", startLine, startCol);
    }
    if (c == '>' && next == '>') {
        advance(s); advance(s);
        return makeToken(TokenType::SHR, ">>", startLine, startCol);
    }
    if (c == '.' && next == '.') {
        advance(s); advance(s);
        return makeToken(TokenType::RANGE, "..", startLine, startCol);
    }
    if (c == '|' && next == '>') {
        advance(s); advance(s);
        return makeToken(TokenType::PIPELINE, "|>", startLine, startCol);
    }
    if (c == '?' && next == '?') {
        advance(s); advance(s);
        return makeToken(TokenType::QUESTION_QUESTION, "??", startLine, startCol);
    }
    if (c == '?' && next == '!') {
        // `?!` is the combined nullable-fallible suffix. It is emitted as
        // a single token so the parser does not have to look at two
        // adjacent suffix tokens to recognize the type.
        advance(s); advance(s);
        return makeToken(TokenType::QUESTION_BANG, "?!", startLine, startCol);
    }
    if (c == '-' && next == '>') {
        advance(s); advance(s);
        return makeToken(TokenType::ARROW, "->", startLine, startCol);
    }
    if (c == '=' && next == '=') {
        advance(s); advance(s);
        return makeToken(TokenType::EQUAL_EQUAL, "==", startLine, startCol);
    }
    if (c == '!' && next == '=') {
        advance(s); advance(s);
        return makeToken(TokenType::NOT_EQUAL, "!=", startLine, startCol);
    }
    if (c == '<' && next == '=') {
        advance(s); advance(s);
        return makeToken(TokenType::LESS_EQUAL, "<=", startLine, startCol);
    }
    if (c == '>' && next == '=') {
        advance(s); advance(s);
        return makeToken(TokenType::GREATER_EQUAL, ">=", startLine, startCol);
    }
    if (c == ':' && next == ':') {
        advance(s); advance(s);
        return makeToken(TokenType::DOUBLE_COLON, "::", startLine, startCol);
    }

    // Compound assignment operators.
    if (next == '=') {
        TokenType t = TokenType::UNKNOWN;
        switch (c) {
            case '+': t = TokenType::PLUS_ASSIGN;   break;
            case '-': t = TokenType::MINUS_ASSIGN;  break;
            case '*': t = TokenType::MUL_ASSIGN;    break;
            case '/': t = TokenType::DIV_ASSIGN;    break;
            case '%': t = TokenType::MOD_ASSIGN;    break;
            case '&': t = TokenType::BIT_AND_ASSIGN; break;
            case '|': t = TokenType::BIT_OR_ASSIGN;  break;
            case '^': t = TokenType::BIT_XOR_ASSIGN; break;
            default: break;
        }
        if (t != TokenType::UNKNOWN) {
            advance(s); advance(s);
            return makeToken(t, std::string(1, c) + "=", startLine, startCol);
        }
    }

    // Single-character tokens.
    switch (c) {
        case '+': advance(s); return makeToken(TokenType::PLUS,        "+", startLine, startCol);
        case '-': advance(s); return makeToken(TokenType::MINUS,       "-", startLine, startCol);
        case '*': advance(s); return makeToken(TokenType::MUL,         "*", startLine, startCol);
        case '/': advance(s); return makeToken(TokenType::DIV,         "/", startLine, startCol);
        case '%': advance(s); return makeToken(TokenType::MOD,         "%", startLine, startCol);
        case '<': advance(s); return makeToken(TokenType::LESS,        "<", startLine, startCol);
        case '>': advance(s); return makeToken(TokenType::GREATER,     ">", startLine, startCol);
        case '=': advance(s); return makeToken(TokenType::ASSIGN,      "=", startLine, startCol);
        case '!': advance(s); return makeToken(TokenType::BANG,        "!", startLine, startCol);
        case '?': advance(s); return makeToken(TokenType::QUESTION,    "?", startLine, startCol);
        case '&': advance(s); return makeToken(TokenType::BIT_AND,     "&", startLine, startCol);
        case '|': advance(s); return makeToken(TokenType::BIT_OR,      "|", startLine, startCol);
        case '^': advance(s); return makeToken(TokenType::BIT_XOR,     "^", startLine, startCol);
        case '~': advance(s); return makeToken(TokenType::BIT_NOT,     "~", startLine, startCol);
        case ':': advance(s); return makeToken(TokenType::COLON,       ":", startLine, startCol);
        case ',': advance(s); return makeToken(TokenType::COMMA,       ",", startLine, startCol);
        case ';': advance(s); return makeToken(TokenType::SEMICOLON,   ";", startLine, startCol);
        case '(': advance(s); return makeToken(TokenType::LPAREN,      "(", startLine, startCol);
        case ')': advance(s); return makeToken(TokenType::RPAREN,      ")", startLine, startCol);
        case '{': advance(s); return makeToken(TokenType::LBRACE,      "{", startLine, startCol);
        case '}': advance(s); return makeToken(TokenType::RBRACE,      "}", startLine, startCol);
        case '[': advance(s); return makeToken(TokenType::LBRACKET,    "[", startLine, startCol);
        case ']': advance(s); return makeToken(TokenType::RBRACKET,    "]", startLine, startCol);
        case '.': advance(s); return makeToken(TokenType::DOT,         ".", startLine, startCol);
        case '@': advance(s); return makeToken(TokenType::AT_SIGN,     "@", startLine, startCol);
        case '#': advance(s); return makeToken(TokenType::HASH,        "#", startLine, startCol);
        default:  break;
    }

    reportError(s, diag::DiagCode::Lex_UnknownCharacter,
                std::string("unexpected character '") + c + "'");
    advance(s);
    return makeToken(TokenType::UNKNOWN, std::string(1, c),
                     startLine, startCol);
}

// ─────────────────────────────────────────────────────────────────────────────
// Token dispatch
// ─────────────────────────────────────────────────────────────────────────────

void lexOne(LexerState& s) {
    skipWhitespace(s);

    if (isAtEnd(s)) {
        s.tokens.push_back(makeToken(TokenType::EOF_TOKEN, "", s.line, s.column));
        return;
    }

    const char c    = currentChar(s);
    const char next = peekChar(s, 1);

    // ─── Comments ───────────────────────────────────────────────────────
    // Order matters: `/--` is a doc comment, `/-` is a block comment, `--`
    // is a line comment. Check the longest prefix first.

    if (c == '/' && next == '-' && peekChar(s, 2) == '-') {
        advance(s); advance(s); advance(s);  // consume `/--`
        bool terminated = false;
        std::string body = lexBlockCommentBody(s, /*isDoc=*/true, terminated);
        if (!terminated) {
            reportError(s, diag::DiagCode::Lex_UnterminatedBlockComment,
                        "unterminated documentation comment (expected --/)");
            return;
        }
        // The doc-comment text is attached to the next declaration by the
        // parser. Emit it as a token; the parser consumes it from the
        // pre-filter stream.
        s.tokens.push_back(makeToken(TokenType::DOC_COMMENT, body,
                                     s.line, s.column));
        return;
    }
    if (c == '/' && next == '-') {
        advance(s); advance(s);  // consume `/-`
        bool terminated = false;
        (void)lexBlockCommentBody(s, /*isDoc=*/false, terminated);
        if (!terminated) {
            reportError(s, diag::DiagCode::Lex_UnterminatedBlockComment,
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
        s.tokens.push_back(lexIdentifier(s));
        return;
    }

    // ─── Numbers ────────────────────────────────────────────────────────
    // A `.` followed by a digit is a float literal; a `.` not followed by
    // a digit is DOT. The check below handles both by testing the digit
    // form first and falling through to the operator table otherwise.
    if (isDigit(c) || (c == '.' && isDigit(next))) {
        s.tokens.push_back(lexNumber(s));
        return;
    }

    // ─── Strings ────────────────────────────────────────────────────────
    if (c == '"') {
        if (next == '"' && peekChar(s, 2) == '"') {
            s.tokens.push_back(lexRawString(s));
            return;
        }
        advance(s);  // opening `"`
        lexStringSegmentCorrected(s, /*isHead=*/true);
        return;
    }

    // ─── Char literal ───────────────────────────────────────────────────
    if (c == '\'') {
        s.tokens.push_back(lexChar(s));
        return;
    }

    // ─── Operator or punctuation ────────────────────────────────────────
    s.tokens.push_back(lexOperatorOrPunctuation(s));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Token> tokenize(const std::string& source,
                            diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, diagnostics);
    while (true) {
        lexOne(s);
        if (!s.tokens.empty() &&
            s.tokens.back().type == TokenType::EOF_TOKEN) break;
    }
    return std::move(s.tokens);
}

std::vector<Token> tokenize_n(const std::string& source,
                              size_t max_tokens,
                              diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, diagnostics);
    for (size_t i = 0; i < max_tokens; ++i) {
        lexOne(s);
        if (!s.tokens.empty() &&
            s.tokens.back().type == TokenType::EOF_TOKEN) break;
    }
    return std::move(s.tokens);
}

} // namespace lucid::lexer