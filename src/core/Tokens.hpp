/// @file Tokens.hpp
/// 
/// @responsibility The token vocabulary: the TokenType enum, the Token value
///                 type, and the classification predicates the parser uses
///                 to dispatch.
/// 
/// ─── Design: the token vocabulary is the boot set ─────────────────────────
/// The lexer recognizes only the boot set — the frames, the content markers,
/// the statement and concurrency keywords, the literals, the punctuation,
/// and the target sigils. Every name the language appears to have beyond
/// that list (int, float, Vec2, Map, toStr, Stringable, ...) is lexed as
/// IDENTIFIER and resolved by Sema against the core scripts. This header
/// enumerates the boot set and nothing else.
/// 
/// ─── Design: Token is a value type ────────────────────────────────────────
/// Every token carries a TokenType, a std::string payload, and a packed
/// SourceLocation. The payload is the raw lexeme for identifiers and
/// literals, the spelling for operators, the text for doc-comments, and
/// empty for tokens that have no text (like EOF_TOKEN). The lexer does not
/// interpret the payload; Sema does. A HEX_LITERAL with payload "0xFF" is
/// a string until Sema turns it into a number.
/// 
/// ─── Design: SourceLocation is packed into 32 bits ────────────────────────
/// A token's location is a single SourceLocation (4 bytes), not separate
/// line/column fields (8 bytes). This matches the AST, where every node
/// also carries a SourceLocation. It also makes every diagnostic a single
/// field read, and every parser backtrack a single-index operation.
/// 
/// ─── Design: no TokenStream here ──────────────────────────────────────────
/// TokenStream — the forward-only view the parser consumes — is a separate
/// header in the parser subsystem. This file defines the tokens themselves
/// and the predicates over their types. Nothing else.

#pragma once

#include "core/SourceLocation.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// TokenType
// ─────────────────────────────────────────────────────────────────────────────
//
// The enum is ordered so tokens of the same kind are contiguous. The parser
// uses the `isXxx` predicates below rather than raw numeric comparisons, but
// the grouping is what makes a `switch` over TokenType readable: all
// keywords, all literals, all operators, all delimiters appear together.
//
// Naming convention:
//
//   KW_*      — a boot-set keyword. The lexer recognizes it by spelling.
//   *_LITERAL — a literal form. The lexer produces the raw lexeme.
//   *_COMMENT — a comment form that survives to the parser (only DOC_COMMENT)
//   (none)    — an operator or delimiter, named by its shape.
//   UNKNOWN   — a lexing error; the parser reports and recovers.
//   EOF_TOKEN — end of input. Always the final token.

enum class TokenType : uint16_t {

    // ─── End of input ───────────────────────────────────────────────────
    EOF_TOKEN = 0,

    // ─── Error recovery ─────────────────────────────────────────────────
    UNKNOWN,        // bad character, malformed literal; the parser reports

    // ─── Identifiers ────────────────────────────────────────────────────
    IDENTIFIER,     // any name that is not a boot-set keyword

    // ─── Frame keywords ─────────────────────────────────────────────────
    //
    // The keywords that introduce a declaration. Uppercase frames bind to
    // the host or declare behavior; lowercase frames name things.

    KW_TYPE,        // TYPE
    KW_FN,          // FN
    KW_DEF,         // DEF
    KW_REQUIRE,     // REQUIRE

    KW_CONST,       // const
    KW_LET,         // let
    KW_IMPORT,      // import
    KW_TRAIT,       // trait
    KW_SATISFY,     // satisfy

    // ─── Content markers ────────────────────────────────────────────────
    //
    // Markers that appear inside a frame's target position. `struct` and
    // `enum` may also start a top-level declaration as sugar.

    KW_STRUCT,      // struct
    KW_ENUM,        // enum
    KW_FN_MARKER,   // fn  — the function-type stage marker (not the FN frame)
    KW_CLS_MARKER,  // cls — the function-type stage marker
    KW_AS,          // as
    KW_SELF,        // Self

    // ─── Statement keywords ─────────────────────────────────────────────
    KW_IF,          // if
    KW_ELSE,        // else
    KW_FOR,         // for
    KW_WHILE,       // while
    KW_DO,          // do
    KW_SWITCH,      // switch
    KW_CASE,        // case
    KW_DEFAULT,     // default
    KW_BREAK,       // break
    KW_CONTINUE,    // continue
    KW_RETURN,      // return

    // ─── Concurrency keywords ───────────────────────────────────────────
    KW_ASYNC,       // async
    KW_SPAWN,       // spawn
    KW_START,       // start
    KW_AWAIT,       // await
    KW_ALL,         // all
    KW_ANY,         // any

    // ─── Literal keywords ───────────────────────────────────────────────
    KW_NIL,         // nil
    KW_ERR,         // err
    KW_TRUE,        // true
    KW_FALSE,       // false

    // ─── Literals with a payload ────────────────────────────────────────
    INT_LITERAL,        // decimal integer
    FLOAT_LITERAL,      // float
    HEX_LITERAL,        // 0x...
    BINARY_LITERAL,     // 0b...
    CHAR_LITERAL,       // 'c' or '\n'
    STRING_HEAD,        // opening segment of a "..." string
    STRING_MIDDLE,      // a segment between two interpolations
    STRING_END,         // closing segment of a "..." string
    RAW_STRING_LITERAL, // """..."""

    // ─── Comments that survive to the parser ────────────────────────────
    //
    // Ordinary `--` and `/- ... -/` comments are dropped by the lexer. The
    // doc-comment form `/-- ... --/` is attached to the declaration that
    // follows it, and the harvester scans backward from the declaration's
    // start position to recover it.

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
    COLON,          // :
    DOUBLE_COLON,   // ::
    DOT,            // .
    ARROW,          // ->
    RANGE,          // ..
    RANGE_EXCLUSIVE,// ..<
    VARIADIC,       // ...

    AT_SIGN,        // @
    HASH,           // #

    // ─── Operators ──────────────────────────────────────────────────────

    // Assignment
    ASSIGN,         // =
    PLUS_ASSIGN,    // +=
    MINUS_ASSIGN,   // -=
    MUL_ASSIGN,     // *=
    DIV_ASSIGN,     // /=
    MOD_ASSIGN,     // %=
    POW_ASSIGN,     // **=
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

    // Suffix markers
    QUESTION,       // ?
    BANG,           // !
    QUESTION_BANG,  // ?!

    // Special operators
    PIPELINE,          // |>
    QUESTION_QUESTION, // ??
};

// ─────────────────────────────────────────────────────────────────────────────
// Token
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A single lexical token.
///
/// The payload's meaning depends on the type:
///   - IDENTIFIER, keyword types: the identifier or keyword spelling.
///   - literal types:              the raw lexeme from the source
///                                 (for STRING_HEAD/MIDDLE/END, the segment
///                                 content with escapes already processed).
///   - operator types:             the operator's spelling.
///   - DOC_COMMENT:                the comment text, with `/--` and `--/`
///                                 stripped.
///   - punctuation:                the punctuation character(s).
///   - EOF_TOKEN, UNKNOWN:         empty or the offending character.
///
/// The payload is `std::string`, not `std::string_view`, because a token
/// can outlive the source buffer in the LSP path (the buffer is re-read on
/// every keystroke, and a token captured for a completion list must not
/// dangle). One small allocation per token is acceptable for a lexer that
/// runs once per compile.
///
/// The location is a single `SourceLocation` (4 bytes packed) rather than
/// separate `line` and `column` fields. See the memory note at the top of
/// this file.
struct Token {
    TokenType      type     = TokenType::UNKNOWN;
    std::string    value;
    SourceLocation location;

    Token() = default;

    /// The canonical constructor: type, value, location.
    Token(TokenType t, std::string v, SourceLocation loc)
        : type(t), value(std::move(v)), location(loc) {}

    /// Convenience: line and column as raw integers. The lexer uses this
    /// form because it tracks line and column as integers, not packed.
    Token(TokenType t, std::string v, uint32_t line, uint32_t column)
        : type(t), value(std::move(v)), location(line, column) {}

    bool is(TokenType t)      const noexcept { return type == t; }
    bool isNot(TokenType t)   const noexcept { return type != t; }
    bool isEof()              const noexcept { return type == TokenType::EOF_TOKEN; }
    bool isUnknown()          const noexcept { return type == TokenType::UNKNOWN; }

    /// True if this token has a non-empty payload. Used by diagnostics to
    /// decide whether to quote the value or just name the token type.
    bool hasValue() const noexcept { return !value.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Classification predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// The parser dispatches on these rather than on raw enum comparisons. The
// predicates are the contract; the enum's ordering is an implementation
// detail.

inline bool isKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_TYPE && t <= TokenType::KW_FALSE;
}

inline bool isFrameKeyword(TokenType t) noexcept {
    return (t >= TokenType::KW_TYPE    && t <= TokenType::KW_DEF)
        || (t >= TokenType::KW_CONST   && t <= TokenType::KW_SATISFY)
        || t == TokenType::KW_REQUIRE;
}

inline bool isContentMarker(TokenType t) noexcept {
    return t >= TokenType::KW_STRUCT && t <= TokenType::KW_SELF;
}

inline bool isStatementKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_IF && t <= TokenType::KW_RETURN;
}

inline bool isConcurrencyKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_ASYNC && t <= TokenType::KW_ANY;
}

inline bool isLiteralKeyword(TokenType t) noexcept {
    return t >= TokenType::KW_NIL && t <= TokenType::KW_FALSE;
}

inline bool isLiteral(TokenType t) noexcept {
    return (t >= TokenType::INT_LITERAL && t <= TokenType::RAW_STRING_LITERAL)
        || isLiteralKeyword(t);
}

inline bool isStringSegment(TokenType t) noexcept {
    return t == TokenType::STRING_HEAD
        || t == TokenType::STRING_MIDDLE
        || t == TokenType::STRING_END;
}

inline bool isDelimiter(TokenType t) noexcept {
    return t >= TokenType::LPAREN && t <= TokenType::HASH;
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

/// True for tokens that can begin an expression. The parser's Pratt loop
/// uses this to decide whether to parse a prefix.
inline bool canStartExpression(TokenType t) noexcept {
    return isLiteral(t)
        || t == TokenType::IDENTIFIER
        || t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::MINUS
        || t == TokenType::BANG
        || t == TokenType::BIT_NOT
        || t == TokenType::KW_IF
        || t == TokenType::KW_NIL
        || t == TokenType::KW_ERR;
}

// ─────────────────────────────────────────────────────────────────────────────
// Names
// ─────────────────────────────────────────────────────────────────────────────

/// The canonical spelling of a token type, for diagnostics and JSON dumps.
/// Returns a string literal — no allocation. Defined in Tokens.cpp.
const char* tokenTypeName(TokenType t) noexcept;

/// The predicate name the parser uses in "expected X, found Y" messages.
/// Differs from `tokenTypeName` for a few tokens: IDENTIFIER becomes
/// "identifier", EOF_TOKEN becomes "end of input", punctuation keeps its
/// spelling. Returns a string literal. Defined in Tokens.cpp.
const char* tokenTypeDescription(TokenType t) noexcept;