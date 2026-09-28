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
 */

#pragma once

#include "core/SourceLocation.hpp"
#include "core/memory/InternedString.hpp"

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

    KW_FIXED,       // FIXED
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
    QUESTION,       // ?
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
///   - IDENTIFIER:      the name, interned.
///   - keyword types:   the keyword's spelling, interned. Uniform with
///                      every other token: `value` is always valid.
///   - literal types:   the literal's content, interned. For a string or
///                      char, escapes are already resolved; for a number,
///                      the raw lexeme is stored and Sema interprets it.
///   - operator types:  the operator's spelling, interned.
///   - DOC_COMMENT:     the comment body with `/--` and `--/` stripped.
///   - EOF_TOKEN:       an invalid InternedString (id 0).
///   - UNKNOWN:         whatever fragment the lexer could recover, interned.
///
/// `value` is always a valid handle. A caller that wants the text uses
/// `pool.lookupView(tok.value)`; a caller that only needs the token type
/// ignores it. There is no case where the field is uninitialized or holds
/// a stale string.
struct Token {
    TokenType      type     = TokenType::UNKNOWN;
    InternedString value;
    SourceLocation location;

    Token() = default;

    Token(TokenType t, InternedString v, SourceLocation loc)
        : type(t), value(v), location(loc) {}

    bool is(TokenType t)    const noexcept { return type == t; }
    bool isNot(TokenType t) const noexcept { return type != t; }
    bool isEof()            const noexcept { return type == TokenType::EOF_TOKEN; }
    bool isUnknown()        const noexcept { return type == TokenType::UNKNOWN; }
    bool hasValue()         const noexcept { return value.isValid(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Classification predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// The parser dispatches on these. The enum's ordering is an implementation
// detail; the predicates are the contract.

inline bool isKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_FIXED && t <= TokenType::KW_NIL;
}

/// @brief True for a keyword that begins a suspend statement (§9.2.2).
///
/// `start` (§9.2.4) is not a suspend keyword; it is an expression
/// prefix. It is not a member of this set.
inline bool isSuspendKeyword(TokenType t) noexcept {
    switch (t) {
        case TokenType::KW_WAIT:
        case TokenType::KW_WAIT_FRAMES:
        case TokenType::KW_WAIT_UNTIL:
        case TokenType::KW_WAIT_FOR_EVENT:
        case TokenType::KW_WAIT_FOR_REQUEST:
            return true;
        default:
            return false;
    }
}

/// @brief True for a token that begins a primitive type.
inline bool isPrimitiveTypeKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_BOOL && t <= TokenType::KW_DOUBLE;
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

// ─────────────────────────────────────────────────────────────────────────────
// Statement-boundary predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// These three predicates are the foundation of the optional-`;` rule
// (§2.7, §12.6). A statement or declaration ends at the first token that
// cannot continue it, so the parser never needs a `;` to find the next
// construct — but only if the tokens that begin a statement and the
// tokens that continue an expression are disjoint sets. These predicates
// name those sets, and the disjointness of `startsStatement` and
// `continuesExpression` is the invariant the whole scheme rests on.
//
// They live in Tokens.hpp, not GrammarPositions.hpp, because they answer
// "what does this token do" — a vocabulary question — rather than "what
// construct can begin at this position" — a grammar-position question.
// ErrorRecovery.hpp and any future formatter use them directly and have
// no business pulling in grammar-position logic.

/// @brief True for a token that can begin a statement.
///
/// The set §12.6 uses to decide whether an expression statement has ended
/// and a new statement has begun. A statement begins with an identifier,
/// a declaration keyword (`let`/`const`), a control-flow keyword, a jump
/// keyword, a suspend keyword, `start`, or `{` for a bare block.
///
/// This set must be disjoint from `continuesExpression`. A unit test
/// asserts that.
inline bool startsStatement(TokenType t) noexcept {
    switch (t) {
        case TokenType::IDENTIFIER:
        case TokenType::KW_LET:
        case TokenType::KW_CONST:
        case TokenType::KW_IF:
        case TokenType::KW_SWITCH:
        case TokenType::KW_FOR:
        case TokenType::KW_WHILE:
        case TokenType::KW_RETURN:
        case TokenType::KW_BREAK:
        case TokenType::KW_CONTINUE:
        case TokenType::KW_WAIT:
        case TokenType::KW_WAIT_FRAMES:
        case TokenType::KW_WAIT_UNTIL:
        case TokenType::KW_WAIT_FOR_EVENT:
        case TokenType::KW_WAIT_FOR_REQUEST:
        case TokenType::KW_START:
        case TokenType::LBRACE:
            return true;
        default:
            return false;
    }
}

/// @brief True for a token that can continue an expression.
///
/// Every postfix and infix operator, plus the range and arrow operators
/// that appear mid-expression. If the parser is at a statement boundary
/// and the current token is in this set, the previous statement is not
/// finished — the token belongs to the expression on the line above.
///
/// This set must be disjoint from `startsStatement`. A unit test asserts
/// that.
inline bool continuesExpression(TokenType t) noexcept {
    switch (t) {
        // Postfix and grouping.
        case TokenType::LPAREN:
        case TokenType::LBRACKET:
        case TokenType::DOT:

        // Null-coalescing and range.
        case TokenType::QUESTION_QUESTION:
        case TokenType::RANGE:
        case TokenType::RANGE_EXCLUSIVE:

        // Lambda arrow.
        case TokenType::ARROW:

        // Logical operators (keywords, but infix).
        case TokenType::KW_AND:
        case TokenType::KW_OR:
            return true;

        default:
            // Every binary and assignment operator, plus the unary bit
            // operators that can also appear infix. isOperator covers the
            // ASSIGN..QUESTION_QUESTION range.
            return isOperator(t);
    }
}

/// @brief True for a token that can end a statement.
///
/// The "previous token" test for the statement-recovery scan (§12.6,
/// rule 2): an identifier stops the scan only when the token before it
/// could have ended a statement. The set is the tokens an expression can
/// end with — an identifier, a literal, a closer, `?` — plus the bare
/// jump keywords, which end a statement with no operand.
///
/// An operator or a comma is deliberately *not* in this set: an
/// identifier after an operator is mid-expression, not a new statement.
inline bool canEndStatement(TokenType t) noexcept {
    switch (t) {
        case TokenType::IDENTIFIER:
        case TokenType::RPAREN:
        case TokenType::RBRACKET:
        case TokenType::RBRACE:
        case TokenType::QUESTION:

        // Bare jumps end a statement with no operand.
        case TokenType::KW_RETURN:
        case TokenType::KW_BREAK:
        case TokenType::KW_CONTINUE:
            return true;

        default:
            return isLiteral(t);
    }
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