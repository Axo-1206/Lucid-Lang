/**
 * @file Lexer.cpp
 *
 * @brief Implementation of the Lucid lexer.
 *
 * ─── Structure of this file ───────────────────────────────────────────────
 *   1. LexerState — the cursor and the diagnostic sink
 *   2. Cursor primitives (advance, peek, match)
 *   3. Token construction
 *   4. The keyword table
 *   5. Comment scanners (block, doc, line)
 *   6. Literal scanners (identifier, number, string, raw string, char)
 *   7. Operator/punctuation scanner
 *   8. The dispatch (lexOne) and the public entry points
 *
 * ─── The cursor ───────────────────────────────────────────────────────────
 * The lexer maintains (position, line, column). Only `advance()` moves
 * them, and it moves all three together so they can never disagree. Every
 * token constructor captures the cursor's location *before* the token's
 * first character is consumed.
 */

#include "Lexer.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

using namespace lucid::diag;

namespace lucid::lexer {

// =============================================================================
// 1. LexerState
// =============================================================================

/// @brief The cursor, the pool, and the diagnostic sink.
struct LexerState {
    std::string_view              source;
    StringPool&                   pool;
    lucid::diag::DiagnosticEngine& diagnostics;
    std::vector<Token>            tokens;

    size_t   position = 0;   // byte offset into source
    uint32_t line     = 1;   // 1-indexed
    uint32_t column   = 1;   // 1-indexed

    LexerState(std::string_view src,
               StringPool& p,
               lucid::diag::DiagnosticEngine& diag)
        : source(src), pool(p), diagnostics(diag) {}
};

// =============================================================================
// 2. Cursor primitives
// =============================================================================

namespace {

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

SourceLocation currentLocation(const LexerState& s) noexcept {
    return SourceLocation{s.line, s.column};
}

} // namespace

// =============================================================================
// 3. Token construction
// =============================================================================

namespace {

/// @brief Build a token. The payload is already interned.
Token makeToken(TokenType type,
                InternedString value,
                SourceLocation location) {
    return Token{type, value, location};
}

/// @brief Build a token from a raw lexeme, interning it through the pool.
Token makeTokenFromLexeme(LexerState& s,
                          TokenType type,
                          std::string_view lexeme,
                          SourceLocation location) {
    return Token{type, s.pool.intern(lexeme), location};
}

// ─── Diagnostics ──────────────────────────────────────────────────────────
//
// Every diagnostic takes an explicit location. The two helpers below exist
// so the call sites read cleanly:
//
//   reportAt — the diagnostic is about the current cursor position.
//   reportErrorAt — the diagnostic is about a location captured earlier
//                   (the opening quote of an unterminated string, for
//                   instance). These are the ones that fixed the old
//                   "reported at EOF" bug.

void reportAt(LexerState& s, DiagCode code, std::string message) {
    s.diagnostics.errorAt(code, currentLocation(s), std::move(message));
}

void reportErrorAt(LexerState& s,
                   DiagCode code,
                   SourceLocation loc,
                   std::string message) {
    s.diagnostics.errorAt(code, loc, std::move(message));
}

} // namespace

// =============================================================================
// 4. The keyword table
// =============================================================================
//
// This is the entire set of words the lexer recognizes as anything other
// than IDENTIFIER. Every entry corresponds to a KW_* value in Tokens.hpp.
//
// A linear scan over string_views. Fifty entries, one comparison per entry
// on a miss; the branch predictor handles it well because most identifiers
// share prefixes with at most a few keywords. If this ever shows up in a
// profile, the fix is a perfect hash — not now.

namespace {

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

    // Sized aliases. Both spellings produce the same PrimitiveKind; the
    // distinct token types let Sema detect which spelling the source used.
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

    return TokenType::IDENTIFIER;
}

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

} // namespace

// =============================================================================
// 5. Comments
// =============================================================================
//
// Three forms:
//
//   -- ...           line comment; dropped, no token.
//   /- ... -/        block comment; nestable; dropped, no token.
//   /-- ... --/      doc comment; NOT nestable; survives as DOC_COMMENT.
//
// The two delimiter forms are different on purpose:
//   - The block-comment opener is /- (two chars); its closer is -/ (two).
//   - The doc-comment opener is /-- (three chars); its closer is --/ (three).
//   - A block comment nests: a nested /- ... -/ inside it must be consumed
//     before the outer -/ can close it. A doc comment does not nest: its
//     body is "any char", so --/ is the first --/ that appears and it ends
//     the comment.
//
// This is the bug the previous implementation had: it used one scanner for
// both, looking for -/ as the closer and stripping only two characters,
// which left a stray '-' in the doc-comment body and terminated on the
// wrong sequence.

namespace {

/// @brief Consume a line comment. The leading `--` has been consumed.
void skipLineComment(LexerState& s) noexcept {
    while (!isAtEnd(s) && currentChar(s) != '\n') advance(s);
}

/// @brief Consume a nestable block comment. The leading `/-` has been consumed.
/// @param terminated  Set to true if a matching `-/` was found.
void readBlockComment(LexerState& s, bool& terminated) {
    terminated = false;
    int depth = 1;

    while (!isAtEnd(s)) {
        if (currentChar(s) == '/' && peekChar(s, 1) == '-') {
            depth++;
            advance(s); advance(s);
            continue;
        }
        if (currentChar(s) == '-' && peekChar(s, 1) == '/') {
            depth--;
            advance(s); advance(s);
            if (depth == 0) {
                terminated = true;
                return;
            }
            continue;
        }
        advance(s);
    }
}

/// @brief Consume a doc comment. The leading `/--` has been consumed.
///
/// Returns the comment's body, with the `--/` closer stripped. Not
/// nestable: the first `--/` after the opener ends the comment, and any
/// `/-` or `-/` inside the body is literal text.
///
/// @param terminated  Set to true if a matching `--/` was found. When
///                    false, the body is everything from the opener to
///                    end-of-input, and the caller reports an error at
///                    the opener's location.
std::string readDocComment(LexerState& s, bool& terminated) {
    terminated = false;
    std::string body;

    while (!isAtEnd(s)) {
        if (currentChar(s) == '-' &&
            peekChar(s, 1) == '-' &&
            peekChar(s, 2) == '/') {
            advance(s); advance(s); advance(s);   // consume `--/`
            terminated = true;
            return body;
        }
        body += currentChar(s);
        advance(s);
    }

    return body;
}

} // namespace

// =============================================================================
// 6. Literal scanners
// =============================================================================

namespace {

// ─── Identifiers and keywords ─────────────────────────────────────────────

/// @brief Lex an identifier or a keyword.
///
/// The lexeme is interned once, whether the token is a keyword or an
/// identifier. Keyword spellings are canonical strings ("TABLE", "let"),
/// so the pool ends up with one ID per keyword regardless of how many
/// times each appears.
void lexIdentifier(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const size_t         startPos = s.position;

    while (!isAtEnd(s) && isIdentifierChar(currentChar(s))) {
        advance(s);
    }

    const std::string_view word =
        s.source.substr(startPos, s.position - startPos);
    const TokenType type = keywordToType(word);

    s.tokens.push_back(makeTokenFromLexeme(s, type, word, startLoc));
}

// ─── Numbers ──────────────────────────────────────────────────────────────
//
// The lexer produces raw lexemes; it does not parse the number. `0xFF` is
// a HEX_LITERAL whose value is the interned string "0xFF". Sema interprets
// the lexeme.
//
// The `.` vs `..` / `..<` ambiguity is resolved here: a `.` is part of a
// float only if it is immediately followed by a digit. `1..10` lexes as
// INT_LITERAL(1), RANGE, INT_LITERAL(10); `1.0` lexes as one FLOAT_LITERAL.

void lexNumber(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const size_t         startPos = s.position;

    // Radix prefixes: 0x, 0b, 0o (case-insensitive).
    if (currentChar(s) == '0') {
        const char next = peekChar(s, 1);

        auto lexRadix = [&](char lower, char upper,
                            bool (*isDigitFn)(char),
                            TokenType type,
                            const char* name) {
            advance(s); advance(s);   // consume `0x` / `0b` / `0o`
            if (!isDigitFn(currentChar(s))) {
                reportErrorAt(s, DiagCode::Lex_InvalidRadixLiteral, startLoc,
                              std::string(name) +
                              " literal has no digits after '0" +
                              std::string(1, lower) + "'");
                s.tokens.push_back(makeTokenFromLexeme(
                    s, TokenType::UNKNOWN,
                    s.source.substr(startPos, s.position - startPos),
                    startLoc));
                return true;
            }
            while (isDigitFn(currentChar(s))) advance(s);
            s.tokens.push_back(makeTokenFromLexeme(
                s, type,
                s.source.substr(startPos, s.position - startPos),
                startLoc));
            return true;
        };

        if (next == 'x' || next == 'X')
            { lexRadix('x', 'X', isHexDigit, TokenType::HEX_LITERAL, "hexadecimal"); return; }
        if (next == 'b' || next == 'B')
            { lexRadix('b', 'B', isBinDigit, TokenType::BINARY_LITERAL, "binary"); return; }
        if (next == 'o' || next == 'O')
            { lexRadix('o', 'O', isOctDigit, TokenType::OCTAL_LITERAL, "octal"); return; }
    }

    // Decimal integer part.
    while (isDigit(currentChar(s))) advance(s);

    bool isFloat = false;

    // Fractional part: `.` followed by a digit. A bare `.` is not part of
    // the number — `1.method()` lexes as INT_LITERAL(1), DOT, IDENTIFIER.
    if (currentChar(s) == '.' && isDigit(peekChar(s, 1))) {
        isFloat = true;
        advance(s);   // `.`
        while (isDigit(currentChar(s))) advance(s);
    }

    // Exponent part.
    if (currentChar(s) == 'e' || currentChar(s) == 'E') {
        isFloat = true;
        advance(s);
        if (currentChar(s) == '+' || currentChar(s) == '-') advance(s);
        if (!isDigit(currentChar(s))) {
            reportErrorAt(s, DiagCode::Lex_InvalidNumberLiteral, startLoc,
                          "exponent has no digits");
            s.tokens.push_back(makeTokenFromLexeme(
                s, TokenType::UNKNOWN,
                s.source.substr(startPos, s.position - startPos),
                startLoc));
            return;
        }
        while (isDigit(currentChar(s))) advance(s);
    }

    s.tokens.push_back(makeTokenFromLexeme(
        s,
        isFloat ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL,
        s.source.substr(startPos, s.position - startPos),
        startLoc));
}

// ─── Escape processing ────────────────────────────────────────────────────
//
// One function, shared by the string lexer and the char lexer. Resolves
// the escape sequence to its actual character. The two callers diverge in
// only one way: a string may contain a NUL (`'\0'`), which it appends to
// its content; a char cannot contain a NUL as its *only* character and
// still be a valid char, but the grammar does not forbid it — a NUL char
// literal is simply a char whose value is 0. Both callers use the same
// resolved byte.

/// @brief Consume the `\` and the following escape character. Append the
///        resolved byte to `out`. Returns false if the escape is invalid
///        or the input ended mid-escape; in that case a diagnostic has
///        been reported at `escapeLoc`.
bool readEscape(LexerState& s, std::string& out, SourceLocation escapeLoc) {
    advance(s);   // consume `\`

    if (isAtEnd(s)) {
        reportErrorAt(s, DiagCode::Lex_InvalidEscapeSequence, escapeLoc,
                      "unterminated escape sequence");
        return false;
    }

    const char next = currentChar(s);
    switch (next) {
        case 'n':  out += '\n'; advance(s); return true;
        case 't':  out += '\t'; advance(s); return true;
        case 'r':  out += '\r'; advance(s); return true;
        case '\\': out += '\\'; advance(s); return true;
        case '"':  out += '"';  advance(s); return true;
        case '\'': out += '\''; advance(s); return true;
        case '0':  out += '\0'; advance(s); return true;
        default:
            reportErrorAt(s, DiagCode::Lex_InvalidEscapeSequence, escapeLoc,
                          std::string("unknown escape '\\") + next + "'");
            advance(s);   // consume the offending character
            return false;
    }
}

// ─── Strings ──────────────────────────────────────────────────────────────
//
// Two forms:
//
//   "..."        normal string; escapes processed; no literal newline.
//   """..."""    raw string; no escapes; newlines allowed; the only
//                forbidden sequence is """ itself.
//
// The grammar has no string interpolation. A "..." string is one token.

/// @brief Lex a normal string. The cursor is on the opening `"`.
void lexString(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s);   // opening `"`

    std::string content;

    while (!isAtEnd(s)) {
        const char c = currentChar(s);

        if (c == '"') {
            advance(s);   // closing `"`
            s.tokens.push_back(makeTokenFromLexeme(
                s, TokenType::STRING_LITERAL, content, startLoc));
            return;
        }

        if (c == '\n') {
            reportErrorAt(s, DiagCode::Lex_NewlineInString, startLoc,
                          "a normal string literal cannot contain a newline; "
                          "use \"\"\" for a multi-line raw string");
            s.tokens.push_back(makeTokenFromLexeme(
                s, TokenType::UNKNOWN, content, startLoc));
            return;   // do NOT consume the newline; let the parser see it
        }

        if (c == '\\') {
            if (!readEscape(s, content, currentLocation(s))) {
                s.tokens.push_back(makeTokenFromLexeme(
                    s, TokenType::UNKNOWN, content, startLoc));
                return;
            }
            continue;
        }

        content += c;
        advance(s);
    }

    reportErrorAt(s, DiagCode::Lex_UnterminatedString, startLoc,
                  "unterminated string literal");
    s.tokens.push_back(makeTokenFromLexeme(
        s, TokenType::UNKNOWN, content, startLoc));
}

/// @brief Lex a raw string. The cursor is on the first `"` of `"""`.
void lexRawString(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s); advance(s); advance(s);   // opening `"""`

    const size_t contentStart = s.position;

    while (!isAtEnd(s)) {
        if (currentChar(s) == '"' &&
            peekChar(s, 1) == '"' &&
            peekChar(s, 2) == '"') {
            const std::string_view content =
                s.source.substr(contentStart, s.position - contentStart);
            advance(s); advance(s); advance(s);   // closing `"""`
            s.tokens.push_back(makeTokenFromLexeme(
                s, TokenType::RAW_STRING_LITERAL, content, startLoc));
            return;
        }
        advance(s);
    }

    reportErrorAt(s, DiagCode::Lex_UnterminatedRawString, startLoc,
                  "unterminated raw string (expected \"\"\")");
    s.tokens.push_back(makeTokenFromLexeme(
        s, TokenType::UNKNOWN,
        s.source.substr(contentStart, s.position - contentStart),
        startLoc));
}

// ─── Character literals ───────────────────────────────────────────────────
//
// A char literal is exactly one character or one escape sequence between
// single quotes. Escapes resolve to their actual byte, matching the
// string lexer. `'\n'` is a CHAR_LITERAL whose value is the interned
// one-byte string "\n" (the actual newline), not "\\n".

void lexChar(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);

    advance(s);   // opening `'`

    if (isAtEnd(s)) {
        reportErrorAt(s, DiagCode::Lex_UnterminatedCharLiteral, startLoc,
                      "unterminated character literal");
        s.tokens.push_back(makeToken(TokenType::UNKNOWN, s.pool.intern(""), startLoc));
        return;
    }

    std::string value;

    if (currentChar(s) == '\\') {
        if (!readEscape(s, value, currentLocation(s))) {
            s.tokens.push_back(makeTokenFromLexeme(
                s, TokenType::UNKNOWN, value, startLoc));
            return;
        }
    } else {
        value += currentChar(s);
        advance(s);
    }

    if (isAtEnd(s) || currentChar(s) != '\'') {
        reportErrorAt(s, DiagCode::Lex_UnterminatedCharLiteral, startLoc,
                      "unterminated character literal (expected closing ')");
        s.tokens.push_back(makeTokenFromLexeme(
            s, TokenType::UNKNOWN, value, startLoc));
        return;
    }

    advance(s);   // closing `'`
    s.tokens.push_back(makeTokenFromLexeme(
        s, TokenType::CHAR_LITERAL, value, startLoc));
}

} // namespace

// =============================================================================
// 7. Operators and punctuation
// =============================================================================
//
// Every operator token's value is the operator's spelling, interned. This
// is a deliberate uniformity choice: a token's value field is always
// valid, so peekValue() never has to special-case "this token has no
// spelling". The pool cost is one ID per distinct operator, not one per
// occurrence.
//
// Order of checks: three-character operators first, then two-character,
// then compound-assignment (`op=`), then single-character.

namespace {

void lexOperatorOrPunctuation(LexerState& s) {
    const SourceLocation startLoc = currentLocation(s);
    const char c    = currentChar(s);
    const char next = peekChar(s, 1);
    const char third = peekChar(s, 2);

    // Consume `n` bytes and emit a token whose value is `spelling`.
    auto emit = [&](TokenType type, std::string_view spelling, size_t n) {
        for (size_t i = 0; i < n; ++i) advance(s);
        s.tokens.push_back(makeTokenFromLexeme(s, type, spelling, startLoc));
    };

    // ─── Three-character operators ──────────────────────────────────────
    if (c == '<' && next == '<' && third == '=') { emit(TokenType::SHL_ASSIGN,      "<<=", 3); return; }
    if (c == '>' && next == '>' && third == '=') { emit(TokenType::SHR_ASSIGN,      ">>=", 3); return; }
    if (c == '.' && next == '.' && third == '.') { emit(TokenType::VARIADIC,        "...", 3); return; }
    if (c == '.' && next == '.' && third == '<') { emit(TokenType::RANGE_EXCLUSIVE, "..<", 3); return; }

    // ─── Two-character operators ────────────────────────────────────────
    if (c == '*' && next == '*') { emit(TokenType::POW,                "**", 2); return; }
    if (c == '<' && next == '<') { emit(TokenType::SHL,                "<<", 2); return; }
    if (c == '>' && next == '>') { emit(TokenType::SHR,                ">>", 2); return; }
    if (c == '.' && next == '.') { emit(TokenType::RANGE,              "..", 2); return; }
    if (c == '-' && next == '>') { emit(TokenType::ARROW,              "->", 2); return; }
    if (c == '=' && next == '=') { emit(TokenType::EQUAL_EQUAL,        "==", 2); return; }
    if (c == '!' && next == '=') { emit(TokenType::NOT_EQUAL,          "!=", 2); return; }
    if (c == '<' && next == '=') { emit(TokenType::LESS_EQUAL,         "<=", 2); return; }
    if (c == '>' && next == '=') { emit(TokenType::GREATER_EQUAL,      ">=", 2); return; }
    if (c == '?' && next == '?') { emit(TokenType::QUESTION_QUESTION,  "??", 2); return; }

    // ─── Compound assignment: single-char op followed by `=` ────────────
    //
    // Checked before single-character dispatch so `+=` is not lexed as
    // `+` then `=`. It is checked after the two-character operators so
    // `==` is not lexed as `=` then `=`.
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
        case '+': emit(TokenType::PLUS,     "+", 1); return;
        case '-': emit(TokenType::MINUS,    "-", 1); return;
        case '*': emit(TokenType::MUL,      "*", 1); return;
        case '/': emit(TokenType::DIV,      "/", 1); return;
        case '%': emit(TokenType::MOD,      "%", 1); return;
        case '<': emit(TokenType::LESS,     "<", 1); return;
        case '>': emit(TokenType::GREATER,  ">", 1); return;
        case '=': emit(TokenType::ASSIGN,   "=", 1); return;
        case '&': emit(TokenType::BIT_AND,  "&", 1); return;
        case '|': emit(TokenType::BIT_OR,   "|", 1); return;
        case '^': emit(TokenType::BIT_XOR,  "^", 1); return;
        case '~': emit(TokenType::BIT_NOT,  "~", 1); return;
        case ':': emit(TokenType::COLON,    ":", 1); return;
        case ',': emit(TokenType::COMMA,    ",", 1); return;
        case ';': emit(TokenType::SEMICOLON,";", 1); return;
        case '(': emit(TokenType::LPAREN,   "(", 1); return;
        case ')': emit(TokenType::RPAREN,   ")", 1); return;
        case '{': emit(TokenType::LBRACE,   "{", 1); return;
        case '}': emit(TokenType::RBRACE,   "}", 1); return;
        case '[': emit(TokenType::LBRACKET, "[", 1); return;
        case ']': emit(TokenType::RBRACKET, "]", 1); return;
        case '.': emit(TokenType::DOT,      ".", 1); return;
        case '@': emit(TokenType::AT_SIGN,  "@", 1); return;
        default:  break;
    }

    // ─── Unknown character ──────────────────────────────────────────────
    reportErrorAt(s, DiagCode::Lex_UnknownCharacter, startLoc,
                  std::string("unexpected character '") + c + "'");
    advance(s);
    s.tokens.push_back(makeTokenFromLexeme(
        s, TokenType::UNKNOWN, std::string_view(&c, 1), startLoc));
}

} // namespace

// =============================================================================
// 8. The dispatch and the public entry points
// =============================================================================

namespace {

/// @brief Lex one token. The main loop calls this until EOF.
void lexOne(LexerState& s) {
    skipWhitespace(s);

    if (isAtEnd(s)) {
        // Intern "" once; the pool maps it to ID 0, which is what a
        // default-constructed InternedString holds, so this is free.
        s.tokens.push_back(makeToken(TokenType::EOF_TOKEN, InternedString{}, currentLocation(s)));
        return;
    }

    const char c    = currentChar(s);
    const char next = peekChar(s, 1);

    // ─── Comments ───────────────────────────────────────────────────────
    //
    // Order matters: `/--` before `/-`. `--` is a distinct first character
    // from either, so its position in the sequence doesn't interact with
    // the other two.
    if (c == '/' && next == '-' && peekChar(s, 2) == '-') {
        const SourceLocation startLoc = currentLocation(s);
        advance(s); advance(s); advance(s);   // consume `/--`
        bool terminated = false;
        std::string body = readDocComment(s, terminated);
        if (!terminated) {
            reportErrorAt(s, DiagCode::Lex_UnterminatedBlockComment, startLoc,
                          "unterminated documentation comment (expected --/)");
            return;
        }
        s.tokens.push_back(makeTokenFromLexeme(
            s, TokenType::DOC_COMMENT, body, startLoc));
        return;
    }
    if (c == '/' && next == '-') {
        const SourceLocation startLoc = currentLocation(s);
        advance(s); advance(s);   // consume `/-`
        bool terminated = false;
        readBlockComment(s, terminated);
        if (!terminated) {
            reportErrorAt(s, DiagCode::Lex_UnterminatedBlockComment, startLoc,
                          "unterminated block comment (expected -/)");
        }
        return;   // block comments are dropped
    }
    if (c == '-' && next == '-') {
        advance(s); advance(s);   // consume `--`
        skipLineComment(s);
        return;   // line comments are dropped
    }

    // ─── Identifiers and keywords ───────────────────────────────────────
    if (isIdentifierStart(c)) {
        lexIdentifier(s);
        return;
    }

    // ─── Numbers ────────────────────────────────────────────────────────
    // A `.` followed by a digit starts a float; a `.` not followed by a
    // digit is DOT, handled by the operator lexer below.
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

std::vector<Token> tokenize(std::string_view source,
                            StringPool& pool,
                            lucid::diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, pool, diagnostics);

    while (true) {
        lexOne(s);
        if (!s.tokens.empty() &&
            s.tokens.back().type == TokenType::EOF_TOKEN) {
            break;
        }
    }

    return s.tokens;
}

std::vector<Token> tokenize_n(std::string_view source,
                              size_t max_tokens,
                              StringPool& pool,
                              lucid::diag::DiagnosticEngine& diagnostics) {
    LexerState s(source, pool, diagnostics);

    for (size_t i = 0; i < max_tokens; ++i) {
        lexOne(s);
        if (!s.tokens.empty() &&
            s.tokens.back().type == TokenType::EOF_TOKEN) {
            break;
        }
    }

    return s.tokens;
}

} // namespace lucid::lexer