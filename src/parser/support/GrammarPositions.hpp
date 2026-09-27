/**
 * @file GrammarPositions.hpp
 *
 * @brief The token sets that name grammatical positions in Lucid.
 *
 * ─── What this file provides ──────────────────────────────────────────────
 * A predicate for each grammar position whose possible leading tokens are
 * worth naming. Each predicate answers: "can this token begin this
 * grammatical construct?".
 *
 * These predicates are used by:
 *
 *   - the parser's dispatch, to decide what construct to parse next;
 *   - the parser's error recovery, to decide where a synchronization scan
 *     should stop;
 *   - future tooling (an LSP, a formatter), to answer "is a construct of
 *     this kind possible here?".
 *
 * ─── Design: why this is separate from Tokens.hpp ─────────────────────────
 * Tokens.hpp answers "what is this token?". GrammarPositions.hpp answers
 * "what can appear at this position in the grammar?". The two are
 * different questions.
 *
 *   - `isSuspendKeyword(KW_WAIT)` is true because §2.2 lists `wait` as a
 *     sequence keyword. That is a vocabulary fact, and it lives in
 *     Tokens.hpp.
 *   - `isStatementStart(KW_WAIT)` is true because §12 lists a suspend
 *     statement as one of the statement forms, and §9.2.2 says a suspend
 *     statement begins with a suspend keyword. That is a grammar-position
 *     fact, and it lives here.
 *
 * Keeping the two files separate means a reader of either one knows what
 * kind of question the file answers. It also means a consumer that only
 * needs the vocabulary (a syntax highlighter, say) does not pull in the
 * grammar-position predicates, and a consumer that only needs the
 * positions (a recovery scan) does not pull in the vocabulary.
 *
 * ─── Design: predicates compose ───────────────────────────────────────────
 * The predicates here are built from the vocabulary predicates in
 * Tokens.hpp. `canStartExpression` is the union of `isLiteral`, an
 * IDENTIFIER, and a few prefix operators. `isBlockStatementStart` is
 * `isStatementStart` plus `let`, `const`, `{`, and `canStartExpression`.
 * Composition is the point: a reader can see, in one line, exactly which
 * tokens a position accepts.
 *
 * ─── Design: named for positions, not for tokens ──────────────────────────
 * Each predicate's name says which grammar position it describes, not
 * which tokens it accepts. `isColumnStart` is easier to read at a call
 * site than `isIdentifierOrAtSign`. If the grammar's `column` production
 * changes, the name still describes the same thing; only the body changes.
 */

#pragma once

#include "core/Tokens.hpp"

namespace lucid::parser {

// =============================================================================
// Declaration positions
// =============================================================================

/// @brief True for a token that can begin a top-level declaration.
///
/// The top level of a file is `{ import_decl | top_level_decl }` (§3), and
/// every declaration may be preceded by juxtaposed attributes (§9). So a
/// top-level declaration position is entered by `import`, by one of the
/// three declaration keywords, or by `@` for an attribute list.
inline bool isDeclarationStart(TokenType t) noexcept {
    return t == TokenType::KW_IMPORT
        || t == TokenType::KW_FIXED
        || t == TokenType::KW_TABLE
        || t == TokenType::KW_FN
        || t == TokenType::KW_LET
        || t == TokenType::KW_CONST
        || t == TokenType::AT_SIGN;
}

/// @brief True for a keyword that names a declaration form.
///
/// This is the *keyword* question, not the *position* question. `import`
/// is a directive, not a name-binding declaration. `@` is not a keyword
/// at all. A caller that wants "can this token begin a declaration
/// position" uses `isDeclarationStart`; a caller that wants "is this
/// keyword one of the declaration forms" uses this.
inline bool isDeclarationKeyword(TokenType t) noexcept {
    return t == TokenType::KW_TABLE
        || t == TokenType::KW_FN
        || t == TokenType::KW_LET
        || t == TokenType::KW_CONST;
}

// =============================================================================
// Statement positions
// =============================================================================

/// @brief True for a keyword that can begin a statement.
///
/// The *statement-start* set: control-flow keywords, jump keywords, and
/// suspend keywords. `else`, `in`, `case`, and `default` are keywords
/// that appear *inside* a statement, not at its start.
///
/// A statement can also begin with `let`/`const` (a local declaration),
/// `{` (a bare block), or any expression-start token. `isBlockStatementStart`
/// below is the union of all of those; `isStatementStart` is just the
/// keyword subset.
inline bool isStatementStart(TokenType t) noexcept {
    switch (t) {
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
            return true;
        default:
            return false;
    }
}

/// @brief True for a keyword that appears inside a statement, not at its
///        start.
///
/// The complement of `isStatementStart` within the statement-keyword
/// range. Exposed because a diagnostic like "unexpected 'else' inside an
/// expression" wants to distinguish "this is a keyword that could have
/// been valid somewhere else" from "this is not a keyword at all".
inline bool isStatementInternalKeyword(TokenType t) noexcept {
    return t == TokenType::KW_ELSE
        || t == TokenType::KW_IN
        || t == TokenType::KW_CASE
        || t == TokenType::KW_DEFAULT;
}

/// @brief True for a keyword that names a statement form.
///
/// The union of statement-start keywords and statement-internal keywords.
/// A caller that wants "can begin a statement" uses `isStatementStart`; a
/// caller that wants "is any statement keyword" uses this.
inline bool isStatementKeyword(TokenType t) noexcept {
    return isStatementStart(t) || isStatementInternalKeyword(t);
}

// =============================================================================
// Expression positions
// =============================================================================

/// @brief True for a token that can begin a prefix expression.
///
/// The leading tokens of the grammar's `expr` production: a literal, an
/// identifier, a parenthesized expression, an array literal, a unary
/// prefix operator, or `start`.
///
/// Used by the Pratt loop's prefix dispatcher and by the block's statement
/// dispatch to decide whether a token begins an expression statement.
inline bool canStartExpression(TokenType t) noexcept {
    return isLiteral(t)
        || t == TokenType::IDENTIFIER
        || t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::MINUS
        || t == TokenType::BIT_NOT
        || t == TokenType::KW_NOT
        || t == TokenType::KW_START;
}

// =============================================================================
// Block positions
// =============================================================================

/// @brief True for a token that can begin a statement inside a block.
///
/// A block's statement position is entered by any statement keyword, by
/// `let` or `const` for a local variable, by `{` for a nested block, or
/// by any token that can begin an expression (an expression statement is
/// a statement form).
///
/// `TABLE`, `FN`, and `@` are excluded: §12.5 forbids them inside a
/// block. They are still accepted by `isBlockBoundary` below, because the
/// block's recovery loop wants to land on one and report a targeted error
/// rather than treat it as an expression start.
inline bool isBlockStatementStart(TokenType t) noexcept {
    return isStatementStart(t)
        || t == TokenType::KW_LET
        || t == TokenType::KW_CONST
        || t == TokenType::LBRACE
        || canStartExpression(t);
}

/// @brief True for a token that can bound the block loop's recovery scan.
///
/// A superset of `isBlockStatementStart` that also accepts the tokens
/// that are not valid inside a block but that the parser wants to stop on
/// so it can report a specific error. Also accepts the terminators that
/// end a statement or the block itself.
///
/// This is the stop set the block's statement loop uses after a statement
/// parse returns `nullptr`.
inline bool isBlockBoundary(TokenType t) noexcept {
    return isBlockStatementStart(t)
        || t == TokenType::KW_FIXED
        || t == TokenType::KW_TABLE
        || t == TokenType::KW_FN
        || t == TokenType::AT_SIGN
        || t == TokenType::SEMICOLON
        || t == TokenType::RBRACE;
}

// =============================================================================
// Table-body positions
// =============================================================================

/// @brief True for a token that can begin a column inside a table body.
///
/// A column is `[attrs] name : type` (§4.1). A column start is either an
/// identifier (the name) or `@` (the first attribute).
inline bool isColumnStart(TokenType t) noexcept {
    return t == TokenType::IDENTIFIER || t == TokenType::AT_SIGN;
}

/// @brief True for a token that can begin a row inside a table initializer.
///
/// A row is `{ expr, ... }` (§4.1). A row start is `{`.
inline bool isRowStart(TokenType t) noexcept {
    return t == TokenType::LBRACE;
}

} // namespace lucid::parser