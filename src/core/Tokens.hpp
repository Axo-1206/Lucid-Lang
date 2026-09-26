/**
 * @file Tokens.hpp
 *
 * @responsibility The token vocabulary for the consolidated Lucid grammar:
 *                 the TokenType enum, the Token value type, and the
 *                 classification predicates the parser uses to dispatch.
 *
 * ─── Design: the token set is the language's fixed vocabulary ─────────────
 * The parser recognizes the keywords, punctuation, and literal forms below
 * and nothing else. Every other name — a table name, a function name, a
 * variable — is an IDENTIFIER and is resolved by Sema against the module's
 * declarations.
 *
 * The primitive type names (`int`, `float`, `bool`, ...) are keywords, not
 * identifiers. The grammar (§2.2) lists them alongside the declaration
 * keywords, and they are recognized directly by the lexer. A primitive is
 * stored inline at its natural size; it carries no host handle, no
 * registry lookup, and no runtime indirection. Making the primitive names
 * keywords rather than declarable identifiers is what keeps that property
 * true: nothing can shadow `int`, and nothing can redefine it.
 *
 * ─── Design: `and`, `or`, `not` are keywords ──────────────────────────────
 * They are operators, and operators are fixed lexical tokens (§6.10).
 * Making them keywords is what lets the parser recognize an operator
 * without consulting name resolution — the same reason `+` and `==` are
 * punctuation rather than identifiers.
 *
 * ─── Design: attributes are juxtaposed, not bracketed ─────────────────────
 * The grammar writes `@export @on(EventKind.KeyDown)` — `@` followed by an
 * identifier, repeated. There are no `[...]` brackets around attribute
 * lists. The lexer emits `AT_SIGN` and the identifier as separate tokens;
 * the parser reads the pair.
 *
 * ─── Design: every function value is a bare code pointer ──────────────────
 * The grammar has no `fn`/`cls` distinction and no closures. Every
 * function value is a compile-time-known code address (§5.0). There is no
 * `KW_FN_MARKER` distinct from the `FN` declaration keyword; the same
 * `FN` token introduces a declaration and (via §5.0's function-type
 * syntax) names the code-address type.
 */

#pragma once

#include "core/SourceLocation.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// TokenType
// ─────────────────────────────────────────────────────────────────────────────
//
// Naming convention:
//
//   KW_*      — a keyword. The lexer recognizes it by spelling.
//   *_LITERAL — a literal form. The lexer produces the raw lexeme.
//   DOC_COMMENT — the one comment form that survives to the parser.
//   (none)    — an operator or delimiter, named by its shape.
//   UNKNOWN   — a lexing error; the parser reports and recovers.
//   EOF_TOKEN — end of input. Always the final token.
//
// The enum is ordered so tokens of the same category are contiguous. The
// parser uses the `isXxx` predicates rather than raw numeric comparisons,
// but the ordering is what makes a `switch` over TokenType readable.

enum class TokenType : uint16_t {

    // ─── End of input ───────────────────────────────────────────────────
    EOF_TOKEN = 0,

    // ─── Error recovery ─────────────────────────────────────────────────
    UNKNOWN,        // bad character, malformed literal; the parser reports

    // ─── Identifiers ────────────────────────────────────────────────────
    IDENTIFIER,     // any name that is not a keyword

    // ─── Declaration keywords ───────────────────────────────────────────
    //
    // The three declaration forms plus `import`, plus the `host` target
    // modifier, plus `as` for import aliases.

    KW_TABLE,       // TABLE
    KW_FN,          // FN
    KW_LET,         // let
    KW_CONST,       // const
    KW_IMPORT,      // import
    KW_AS,          // as
    KW_HOST,        // host

    // ─── Primitive type keywords ────────────────────────────────────────
    //
    // The primitive type names are keywords, not identifiers. The sized
    // aliases (`int`, `long`, `uint`, `ulong`, `float`, `double`) are
    // distinct tokens from their canonical forms (`int32`, `int64`, ...)
    // so that Sema can decide whether a given spelling is the canonical
    // name or the alias. Both spellings produce the same underlying type.

    KW_BOOL,
    KW_CHAR,
    KW_STRING,
    KW_UNIT,

    KW_INT8,    KW_INT16,   KW_INT32,   KW_INT64,
    KW_UINT8,   KW_UINT16,  KW_UINT32,  KW_UINT64,
    KW_FLOAT32, KW_FLOAT64,

    // Sized aliases (same types as their canonical forms).
    KW_INT,     // = int32
    KW_LONG,    // = int64
    KW_UINT,    // = uint32
    KW_ULONG,   // = uint64
    KW_FLOAT,   // = float32
    KW_DOUBLE,  // = float64

    // ─── Statement keywords ─────────────────────────────────────────────
    KW_IF,          // if
    KW_ELSE,        // else
    KW_SWITCH,      // switch
    KW_CASE,        // case
    KW_DEFAULT,     // default
    KW_FOR,         // for
    KW_IN,          // in
    KW_WHILE,       // while
    KW_RETURN,      // return
    KW_BREAK,       // break
    KW_CONTINUE,    // continue

    // ─── Sequence keywords (§9.2) ───────────────────────────────────────
    //
    // The suspension primitive. These are recognized by the parser like
    // statement keywords; Sema enforces that they appear only inside a
    // `@sequence`-annotated function's body.

    KW_WAIT,            // wait
    KW_WAIT_FRAMES,     // waitFrames
    KW_WAIT_UNTIL,      // waitUntil
    KW_WAIT_FOR_EVENT,  // waitForEvent
    KW_WAIT_FOR_REQUEST,// waitForRequest
    KW_START,           // start

    // ─── Operator keywords ──────────────────────────────────────────────
    //
    // `and`, `or`, `not` are operators (§6.10). They are keywords so the
    // parser can recognize them without consulting name resolution.

    KW_AND,         // and
    KW_OR,          // or
    KW_NOT,         // not

    // ─── Literal keywords ───────────────────────────────────────────────
    KW_TRUE,        // true
    KW_FALSE,       // false
    KW_NIL,         // nil

    // ─── Literals with a payload ────────────────────────────────────────
    INT_LITERAL,        // decimal integer
    FLOAT_LITERAL,      // float
    HEX_LITERAL,        // 0x...
    BINARY_LITERAL,     // 0b...
    OCTAL_LITERAL,      // 0o...
    CHAR_LITERAL,       // 'c' or '\n'
    STRING_LITERAL,     // "..."
    RAW_STRING_LITERAL, // """..."""

    // ─── The one comment form that reaches the parser ───────────────────
    //
    // `--` line comments and `/- ... -/` block comments are dropped by the
    // lexer. The doc-comment form `/-- ... --/` attaches to the next
    // declaration, so it survives as a token.

    DOC_COMMENT,    // /-- ... --/

    // ─── Delimiters ─────────────────────────────────────────────────────
    LPAREN,         // (
    RPAREN,         // )
    LBRACE,         // {
    RBRACE,         // }
    LBRACKET,       // [
    RBRACKET,       // ]

    COMMA,          // ,
    SEMICOLON,      // ;
    DOT,            // .
    COLON,          // :
    ARROW,          // ->
    VARIADIC,       // ...
    RANGE,          // ..
    RANGE_EXCLUSIVE,// ..<
    AT_SIGN,        // @

    // ─── Operators ──────────────────────────────────────────────────────

    // Assignment
    ASSIGN,         // =
    PLUS_ASSIGN,    // +=
    MINUS_ASSIGN,   // -=
    MUL_ASSIGN,     // *=
    DIV_ASSIGN,     // /=
    MOD_ASSIGN,     // %=
    BIT_AND_ASSIGN, // &=
    BIT_OR_ASSIGN,  // |=
    BIT_XOR_ASSIGN, // ^=
    SHL_ASSIGN,     // <<=
    SHR_ASSIGN,     // >>=

    // Arithmetic
    PLUS,           // +
    MINUS,          // -
    MUL,            // *
    DIV,            // /
    MOD,            // %
    POW,            // **

    // Comparison
    EQUAL_EQUAL,    // ==
    NOT_EQUAL,      // !=
    LESS,           // <
    LESS_EQUAL,     // <=
    GREATER,        // >
    GREATER_EQUAL,  // >=

    // Bitwise
    BIT_AND,        // &
    BIT_OR,         // |
    BIT_XOR,        // ^
    BIT_NOT,        // ~
    SHL,            // <<
    SHR,            // >>

    // Null coalescing
    QUESTION_QUESTION, // ??
};

// ─────────────────────────────────────────────────────────────────────────────
// Token
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A single lexical token.
///
/// The payload's meaning depends on the type:
///   - IDENTIFIER and keyword types: the spelling.
///   - literal types: the raw lexeme from the source.
///   - operator types: the operator's spelling.
///   - DOC_COMMENT: the comment text, with the `/--` and `--/` markers
///     stripped.
///   - punctuation: the punctuation character(s).
///   - EOF_TOKEN, UNKNOWN: empty or the offending character.
struct Token {
    TokenType      type     = TokenType::UNKNOWN;
    std::string    value;
    SourceLocation location;

    Token() = default;

    Token(TokenType t, std::string v, SourceLocation loc)
        : type(t), value(std::move(v)), location(loc) {}

    /// Convenience: line and column as raw integers. The lexer uses this
    /// form because it tracks line and column as integers.
    Token(TokenType t, std::string v, uint32_t line, uint32_t column)
        : type(t), value(std::move(v)), location(line, column) {}

    bool is(TokenType t)    const noexcept { return type == t; }
    bool isNot(TokenType t) const noexcept { return type != t; }
    bool isEof()            const noexcept { return type == TokenType::EOF_TOKEN; }
    bool isUnknown()        const noexcept { return type == TokenType::UNKNOWN; }

    bool hasValue() const noexcept { return !value.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Classification predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// The parser dispatches on these. The enum's ordering is an implementation
// detail; the predicates are the contract.

inline bool isKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_TABLE && t <= TokenType::KW_NIL;
}

/// @brief True for a token that can begin a declaration.
///
/// A declaration begins with one of the three declaration keywords.
/// `import` and `host` are not included: `import` begins an import
/// directive, which the parser handles separately, and `host` is a target
/// modifier that appears after `=`.
inline bool isDeclarationKeyword(TokenType t) noexcept {
    return t == TokenType::KW_TABLE
        || t == TokenType::KW_FN
        || t == TokenType::KW_LET
        || t == TokenType::KW_CONST;
}

/// @brief True for a token that begins a primitive type.
inline bool isPrimitiveTypeKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_BOOL && t <= TokenType::KW_DOUBLE;
}

inline bool isStatementKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_IF && t <= TokenType::KW_CONTINUE;
}

/// @brief True for a token that begins a sequence suspend point.
inline bool isSuspendKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_WAIT && t <= TokenType::KW_WAIT_FOR_REQUEST;
}

inline bool isOperatorKeyword(TokenType t) noexcept {
    return t == TokenType::KW_AND
        || t == TokenType::KW_OR
        || t == TokenType::KW_NOT;
}

inline bool isLiteralKeyword(TokenType t) noexcept {
    return t == TokenType::KW_TRUE
        || t == TokenType::KW_FALSE
        || t == TokenType::KW_NIL;
}

inline bool isLiteral(TokenType t) noexcept {
    return (t >= TokenType::INT_LITERAL && t <= TokenType::RAW_STRING_LITERAL)
        || isLiteralKeyword(t);
}

inline bool isDelimiter(TokenType t) noexcept {
    return t >= TokenType::LPAREN && t <= TokenType::AT_SIGN;
}

inline bool isOpeningDelimiter(TokenType t) noexcept {
    return t == TokenType::LPAREN
        || t == TokenType::LBRACE
        || t == TokenType::LBRACKET;
}

inline bool isClosingDelimiter(TokenType t) noexcept {
    return t == TokenType::RPAREN
        || t == TokenType::RBRACE
        || t == TokenType::RBRACKET;
}

inline bool isOperator(TokenType t) noexcept {
    return t >= TokenType::ASSIGN && t <= TokenType::QUESTION_QUESTION;
}

inline bool isAssignmentOperator(TokenType t) noexcept {
    return t >= TokenType::ASSIGN && t <= TokenType::SHR_ASSIGN;
}

inline bool isComparisonOperator(TokenType t) noexcept {
    return t >= TokenType::EQUAL_EQUAL && t <= TokenType::GREATER_EQUAL;
}

inline bool isBitwiseOperator(TokenType t) noexcept {
    return t >= TokenType::BIT_AND && t <= TokenType::SHR;
}

/// @brief True for a token that can begin a prefix expression.
///
/// Used by the Pratt loop's prefix dispatcher to decide whether to
/// consume a token at all.
inline bool canStartExpression(TokenType t) noexcept {
    return isLiteral(t)
        || t == TokenType::IDENTIFIER
        || t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::MINUS
        || t == TokenType::BIT_NOT
        || t == TokenType::KW_NOT
        || t == TokenType::KW_START;    // start expr: `start f(args)`
}

// ─────────────────────────────────────────────────────────────────────────────
// Names
// ─────────────────────────────────────────────────────────────────────────────

/// The canonical spelling of a token type, for diagnostics and JSON
/// dumps. Returns a string literal. Defined in Tokens.cpp.
const char* tokenTypeName(TokenType t) noexcept;

/// The predicate name the parser uses in "expected X, found Y"
/// messages. Defined in Tokens.cpp.
const char* tokenTypeDescription(TokenType t) noexcept;