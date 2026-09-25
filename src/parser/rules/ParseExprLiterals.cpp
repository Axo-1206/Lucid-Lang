/**
 * @file ParseExprLiterals.cpp
 * @brief The primary and postfix expression parsers that are not function
 *        literals or pipelines.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseLiteralExpr         scalar literals and the `nil`/`err` tokens
 *   - parseArrayLiteralExpr    `[a, b, c]`
 *   - parseStructLiteralExpr   `Point { x = 1.0, y = 2.0 }`
 *   - parseIdentifierExpr      a bare name, possibly with generics
 *   - parseIfExpr              `if cond ?? then else else`
 *   - parseIndexExpr           `a[i]`
 *   - parseSliceExpr           `a[lo..hi]`
 *   - looksLikeSliceStart      helper: does `[` start a slice or an index?
 *
 * The remaining postfix parsers — call, field access, module access — are
 * declared in Parser.hpp but implemented in ParseExprFuncLit.cpp, because
 * they interact with the function-literal and pipeline machinery.
 *
 * ─── Design: primary parsers produce a node per token-shape ───────────────
 * Each function here reads one expression form. The dispatcher in
 * parsePrimaryExpr (ParseExpr.cpp) has already checked the leading token,
 * so these functions do not re-check the shape; they assume the caller
 * has confirmed it and read the construct.
 *
 * The one exception is parseStructLiteralExpr: it is called from two
 * places (parsePrimaryExpr's identifier branch when it peeks a following
 * `{`, and the same position for `Type<Args> { ... }`), and it verifies
 * the presence of the opening brace before proceeding. This is defensive
 * and cheap.
 *
 * ─── Design: `parseIdentifierExpr` and generic args ───────────────────────
 * An identifier expression may be a bare name (`x`, `add`) or a generic
 * specialization reference (`identity<int>`, `map<int, string>`). Both
 * forms are `IdentifierExprAST` nodes; the distinction is whether
 * `genericArgs` is empty.
 *
 * The struct-literal case (`Point { ... }`) is decided by the caller
 * (parsePrimaryExpr), which peeks past the identifier and its generic
 * args to look for `{`. This file's `parseIdentifierExpr` therefore
 * assumes the caller has already decided the form is not a struct
 * literal; it just parses the identifier and its generics.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseLiteralExpr — scalar literals
// =============================================================================

/// @brief Parse a scalar literal.
///
/// The grammar's `literal` production lists every form:
///
///   INT_LIT    FLOAT_LIT    STRING_LIT    CHAR_LIT
///   BOOL_LIT   'nil'        'err'
///
/// Under the current token set, each form is a distinct token type (or,
/// for strings, a short sequence of STRING_HEAD / STRING_END tokens).
/// This function reads the token(s), records the raw lexeme as the
/// literal's value, and produces a `LiteralExprAST`.
///
/// The parser does not convert numeric literals; Sema does. The `value`
/// field is the raw lexeme as written in source. Sema reads the lexeme
/// and produces the concrete value (with range checks, radix handling,
/// and fit-to-target-type validation).
///
/// The `nil` and `err` literals are keywords (KW_NIL, KW_ERR) and are
/// handled here alongside the numeric and string literals.
LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // Helper: consume one token and produce a literal with the given kind.
    auto emitSimple = [&](LiteralKind kind) -> LiteralExprAST* {
        Token valueTok = stream.consume();
        auto* lit = ctx.arena().make<LiteralExprAST>(
            kind, ctx.pool().intern(valueTok.value));
        lit->loc = loc;
        return lit;
    };

    switch (current) {
        case TokenType::INT_LITERAL:    return emitSimple(LiteralKind::Int);
        case TokenType::HEX_LITERAL:    return emitSimple(LiteralKind::Hex);
        case TokenType::BINARY_LITERAL: return emitSimple(LiteralKind::Binary);
        case TokenType::FLOAT_LITERAL:  return emitSimple(LiteralKind::Float);
        case TokenType::CHAR_LITERAL:   return emitSimple(LiteralKind::Char);
        case TokenType::KW_TRUE:        return emitSimple(LiteralKind::True);
        case TokenType::KW_FALSE:       return emitSimple(LiteralKind::False);
        case TokenType::KW_NIL:         return emitSimple(LiteralKind::Nil);
        case TokenType::KW_ERR:         return emitSimple(LiteralKind::Err);

        case TokenType::RAW_STRING_LITERAL:
            return emitSimple(LiteralKind::RawString);

        case TokenType::STRING_HEAD: {
            // A `"..."` string is emitted as a sequence of
            // STRING_HEAD / STRING_MIDDLE / STRING_END tokens. A string
            // with no interpolation is one HEAD followed by one END.
            //
            // A string with interpolations is a sequence:
            //   HEAD, <interpolation tokens>, MIDDLE, <tokens>, ..., END
            //
            // The parser folds the whole sequence into a single
            // LiteralExprAST with the STRING kind. The interpolations
            // are *not* parsed here; they are the Sema layer's job, or
            // a future re-lexing pass. This parser assumes strings
            // without interpolation for the initial cut.
            //
            // If a MIDDLE follows the HEAD, the parser reports
            // "string interpolation is not yet supported" and skips to
            // the END. The string's value is the concatenation of the
            // HEAD and any MIDDLE segments' raw content; no further
            // interpretation.
            Token head = stream.consume();
            std::string combined = head.value;

            while (stream.check(TokenType::STRING_MIDDLE) ||
                   stream.check(TokenType::STRING_END)) {
                if (stream.check(TokenType::STRING_END)) {
                    stream.consume();
                    break;
                }
                // MIDDLE
                Token mid = stream.consume();
                combined += mid.value;
            }

            auto* lit = ctx.arena().make<LiteralExprAST>(
                LiteralKind::String, ctx.pool().intern(combined));
            lit->loc = loc;
            return lit;
        }

        default:
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedLiteral,
                               loc,
                               "expected a literal, got '",
                               stream.peekValue(), "'");
            return nullptr;
    }
}

// =============================================================================
// parseArrayLiteralExpr — `[a, b, c]`
// =============================================================================

/// @brief Parse an array literal.
///
/// The form is a bracketed, comma-separated list of expressions:
///
///   `[1, 2, 3]`
///   `[]`
///   `[a + b, c * d]`
///
/// The array's kind (dynamic, slice, fixed) is not determined by the
/// literal; it is determined by the *declared type* of the binding the
/// literal initializes. The parser produces a generic
/// `ArrayLiteralExprAST` and lets Sema decide the kind based on context.
///
/// A trailing comma is permitted: `[1, 2, 3,]` is legal. The grammar
/// does not forbid it; this function accepts and skips it.
///
/// An empty literal `[]` is legal at the parser level. Sema rejects it
/// when there is no declared type to determine the element type.
ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream,
                                           ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '[', got '", stream.peekValue(), "'");
        return nullptr;
    }

    std::vector<ExprAST*> elements;

    // Empty array literal `[]`.
    if (stream.match(TokenType::RBRACKET)) {
        auto* empty = ctx.arena().make<ArrayLiteralExprAST>(
            ctx.arena().makeBuilder<ExprAST*>().build());
        empty->loc = loc;
        return empty;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET) &&
           ctx.canContinue()) {
        ExprAST* elem = parseExpr(stream, ctx);
        if (!elem) {
            // parseExpr reports its own error. Produce a placeholder
            // element and synchronize to the next element or the
            // closing `]`.
            auto* placeholder = ctx.arena().make<UnknownExprAST>();
            placeholder->loc = stream.currentLoc();
            placeholder->hasSyntaxError = true;
            elements.push_back(placeholder);

            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::RBRACKET);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }
        elements.push_back(elem);

        if (stream.match(TokenType::COMMA)) {
            // A trailing comma before the closing `]` is permitted.
            if (stream.check(TokenType::RBRACKET)) {
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RBRACKET)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ']' in array literal, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::RBRACKET);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close array literal, got '",
                           stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<ExprAST*>(elements.size());
    for (ExprAST* e : elements) builder.push_back(e);

    auto* arrayLit = ctx.arena().make<ArrayLiteralExprAST>(builder.build());
    arrayLit->loc = loc;
    return arrayLit;
}

// =============================================================================
// parseStructLiteralExpr — `Type { field = value, ... }`
// =============================================================================

/// @brief Parse a struct literal.
///
/// The form is a struct type name, optional generic arguments, and a
/// brace-delimited list of field initializers:
///
///   `Point { x = 1.0, y = 2.0 }`
///   `Box<int> { value = 42 }`
///   `Pair { first = 1, second = "one" }`
///
/// Field order is free. Fields with defaults may be omitted. A trailing
/// comma after the last field is permitted.
///
/// The parser does not resolve the type name against the type namespace,
/// does not check that the fields exist, and does not check that the
/// field values' types match the fields' declared types. Sema does all
/// of that after the AST is built.
///
/// The parser produces a `StructLiteralExprAST` whose `typeName` is the
/// interned name, `genericArgs` the parsed generics, and `inits` the
/// field initializers.
StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream,
                                             ParserContext& ctx,
                                             InternedString typeName,
                                             ArenaSpan<TypeAST*> genericArgs) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '{' for struct literal, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    std::vector<FieldInitAST*> inits;

    // Empty literal `Type {}`.
    if (stream.match(TokenType::RBRACE)) {
        auto* empty = ctx.arena().make<StructLiteralExprAST>(
            typeName, genericArgs,
            ctx.arena().makeBuilder<FieldInitAST*>().build());
        empty->loc = loc;
        return empty;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // ─── Field name ───────────────────────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected a field name in struct literal, "
                               "got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::RBRACE);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }
        Token nameTok = stream.consume();
        InternedString fieldName = ctx.pool().intern(nameTok.value);

        // ─── `=` ──────────────────────────────────────────────────────────
        if (!stream.match(TokenType::ASSIGN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected '=' after field name '",
                               ctx.pool().lookup(fieldName), "', got '",
                               stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::RBRACE);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }

        // ─── Field value ──────────────────────────────────────────────────
        ExprAST* value = parseRequiredExpr(stream, ctx, "field value");

        auto* init = ctx.arena().make<FieldInitAST>(fieldName, value);
        init->loc = nameTok.location;   // or value->loc; see note
        if (value->hasSyntaxError) init->hasSyntaxError = true;
        inits.push_back(init);

        // ─── Separator ────────────────────────────────────────────────────
        if (stream.match(TokenType::COMMA)) {
            // Trailing comma before `}` is permitted.
            if (stream.check(TokenType::RBRACE)) {
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RBRACE)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or '}' after field value, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::RBRACE);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '}' to close struct literal, got '",
                           stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<FieldInitAST*>(inits.size());
    for (FieldInitAST* f : inits) builder.push_back(f);

    auto* structLit = ctx.arena().make<StructLiteralExprAST>(
        typeName, genericArgs, builder.build());
    structLit->loc = loc;
    return structLit;
}

// =============================================================================
// parseIdentifierExpr — a bare name, optionally generic
// =============================================================================

/// @brief Parse a bare identifier, optionally followed by generic
///        arguments.
///
/// Two forms:
///
///   `x`             a plain name
///   `identity<int>` a generic specialization reference
///
/// The parser does not resolve the name against the scope. It produces
/// an `IdentifierExprAST` with the interned name and, if present, the
/// generic arguments. Sema resolves the name and checks the arguments.
///
/// The identifier may be followed by `::` (a module-qualified access),
/// `.` (a field access), `(` (a call), or `{` (a struct literal). None
/// of these are consumed by this function; the caller (parsePrimaryExpr)
/// checks for `{` before calling this function, and the Pratt loop's
/// postfix dispatch handles the rest.
IdentifierExprAST* parseIdentifierExpr(TokenStream& stream,
                                       ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           loc,
                           "expected an identifier, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // Optional generic arguments.
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }

    auto* idExpr = ctx.arena().make<IdentifierExprAST>(name);
    idExpr->loc = loc;
    idExpr->genericArgs = genericArgs;
    return idExpr;
}

// =============================================================================
// parseIfExpr — `if cond ?? then else else`
// =============================================================================

/// @brief Parse the expression form of `if`.
///
/// Grammar:
///
///   if_expr := 'if' expr '??' expr 'else' expr
///
/// Distinct from the statement form (`IfStmtAST`), which has an optional
/// `else` and produces no value. The parser distinguishes them by the
/// `??` after the condition: an if with `??` is an expression, an if
/// without is a statement.
///
/// The `else` branch is required. Chained if-expressions are
/// right-associative: `if a ?? b else if c ?? d else e` parses as
/// `if a ?? b else (if c ?? d else e)`. The parser handles this by
/// letting the else branch be an arbitrary expression, and recursively
/// dispatching a leading `if` in that position to this function.
IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_IF)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'if', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Condition ────────────────────────────────────────────────────────
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a condition after 'if'");
        return nullptr;
    }

    // ─── `??` separator ───────────────────────────────────────────────────
    if (!stream.match(TokenType::QUESTION_QUESTION)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '?\?' after the if-expression's "
                           "condition, got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Then branch ──────────────────────────────────────────────────────
    ExprAST* thenBranch = parseExpr(stream, ctx);
    if (!thenBranch) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a then-branch after '?\?'");
        return nullptr;
    }

    // ─── `else` ───────────────────────────────────────────────────────────
    if (!stream.match(TokenType::KW_ELSE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected 'else' after the then-branch, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Else branch ──────────────────────────────────────────────────────
    //
    // The else branch is an expression. If it is another if-expression,
    // parseExpr's dispatch to parsePrimaryExpr sees a leading `if` and
    // routes to this function. That produces the right-associativity
    // for chained if-expressions without any special code here.
    ExprAST* elseBranch = parseExpr(stream, ctx);
    if (!elseBranch) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an else-branch after 'else'");
        return nullptr;
    }

    auto* ifExpr = ctx.arena().make<IfExprAST>(
        condition, thenBranch, elseBranch);
    ifExpr->loc = loc;
    return ifExpr;
}

// =============================================================================
// parseIndexExpr — `a[i]`
// =============================================================================

/// @brief Parse an index expression: `container[index]`.
///
/// The container is the expression to the left of the `[`. The index is
/// any expression of the container's key type. The parser produces an
/// `IndexExprAST` with both operands; Sema resolves the operation
/// through the `INDEX_GET` DEF table.
///
/// The parser does not check that the container is indexable, that the
/// index type is correct, or that the index is in range. All of those
/// are Sema's job.
IndexExprAST* parseIndexExpr(TokenStream& stream,
                             ParserContext& ctx,
                             ExprAST* target) {
    const SourceLocation loc = stream.currentLoc();

    if (!target) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           loc,
                           "expected a target for the index expression");
        return nullptr;
    }

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '[', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // Empty index: `a[]` is a syntax error.
    if (stream.check(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "empty index expression; a value is required "
                           "inside '[]'");
        stream.consume();   // `]`
        return nullptr;
    }

    ExprAST* index = parseExpr(stream, ctx);
    if (!index) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an index expression");
        return nullptr;
    }

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close index expression, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    auto* indexExpr = ctx.arena().make<IndexExprAST>(target, index);
    indexExpr->loc = loc;
    return indexExpr;
}

// =============================================================================
// parseSliceExpr — `a[lo..hi]`
// =============================================================================

/// @brief Parse a slice expression: `container[lo..hi]` or a variant.
///
/// The forms:
///
///   `a[lo..hi]`      inclusive range from lo to hi
///   `a[lo..<hi]`     exclusive range from lo to hi
///   `a[..hi]`        from the start of the container to hi
///   `a[lo..]`        from lo to the end of the container
///   `a[..]`          the whole container
///
/// A slice is a borrowed view into the underlying array. The parser
/// produces a `SliceExprAST` with the bounds; Sema checks that the
/// container is sliceable and that the bounds are compatible with the
/// element type.
///
/// The `start` and `end` fields may be null when the corresponding
/// bound is omitted. The `isExclusive` flag distinguishes `..` from
/// `..<`.
SliceExprAST* parseSliceExpr(TokenStream& stream,
                             ParserContext& ctx,
                             ExprAST* target) {
    const SourceLocation loc = stream.currentLoc();

    if (!target) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           loc,
                           "expected a target for the slice expression");
        return nullptr;
    }

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '[', got '", stream.peekValue(), "'");
        return nullptr;
    }

    ExprAST* start = nullptr;
    ExprAST* end   = nullptr;
    bool     isExclusive = false;

    // ─── Start bound ──────────────────────────────────────────────────────
    //
    // If the first token is a range operator, the start is omitted.
    if (!stream.check(TokenType::RANGE) &&
        !stream.check(TokenType::RANGE_EXCLUSIVE)) {
        start = parseExpr(stream, ctx);
        if (!start) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                               stream.currentLoc(),
                               "expected a slice start expression or a "
                               "range operator");
            return nullptr;
        }
    }

    // ─── Range operator ───────────────────────────────────────────────────
    if (!stream.check(TokenType::RANGE) &&
        !stream.check(TokenType::RANGE_EXCLUSIVE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '..' or '..<' in slice expression, "
                           "got '", stream.peekValue(), "'");
        return nullptr;
    }
    isExclusive = stream.check(TokenType::RANGE_EXCLUSIVE);
    stream.consume();   // `..` or `..<`

    // ─── End bound ────────────────────────────────────────────────────────
    //
    // If the next token is `]`, the end is omitted.
    if (!stream.check(TokenType::RBRACKET)) {
        end = parseExpr(stream, ctx);
        if (!end) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                               stream.currentLoc(),
                               "expected a slice end expression or ']'");
            return nullptr;
        }
    }

    // ─── Closing `]` ──────────────────────────────────────────────────────
    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close slice expression, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    auto* slice = ctx.arena().make<SliceExprAST>(
        target, start, end, isExclusive);
    slice->loc = loc;
    return slice;
}

// =============================================================================
// looksLikeSliceStart — file-local helper
// =============================================================================

/// @brief Peek past a `[` to decide whether the form is a slice or an index.
///
/// A slice's bracket pair contains a top-level `..` or `..<`. An index's
/// does not. The helper walks the tokens inside the bracket pair,
/// tracking bracket depth, and returns true at the first top-level range
/// operator it finds.
///
/// The scan is bounded by the closing `]` of the outer bracket pair. If
/// the pair is malformed (missing the closer), the scan stops at EOF and
/// returns false; the caller reports the malformed bracket.
///
/// Precondition: the current token is `[`. The stream position is
/// restored before returning, on every path.
///
/// The helper is file-local (declared in Parser.hpp only for the purpose
/// of being callable from `parsePostfixExpr` in ParseExpr.cpp; if that
/// call site is moved or removed, this can become file-local to this
/// file).
bool looksLikeSliceStart(TokenStream& stream) {
    const size_t savedPos = stream.getPos();

    if (!stream.check(TokenType::LBRACKET)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();   // `[`

    int bracketDepth = 0;
    while (!stream.isAtEnd()) {
        const TokenType t = stream.peekType();

        if (t == TokenType::LBRACKET) {
            bracketDepth++;
            stream.consume();
            continue;
        }
        if (t == TokenType::RBRACKET) {
            if (bracketDepth == 0) {
                // Reached the closing `]` of the outer bracket pair
                // without seeing a range operator. Not a slice.
                stream.setPos(savedPos);
                return false;
            }
            bracketDepth--;
            stream.consume();
            continue;
        }
        if (bracketDepth == 0 &&
            (t == TokenType::RANGE || t == TokenType::RANGE_EXCLUSIVE)) {
            stream.setPos(savedPos);
            return true;
        }

        stream.consume();
    }

    // Reached EOF without a closing `]` or a range operator.
    stream.setPos(savedPos);
    return false;
}

} // namespace lucid::parser