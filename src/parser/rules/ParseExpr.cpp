/**
 * @file ParseExpr.cpp
 * @brief The expression parser's core: Pratt loop and dispatch.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseExpr            the entry point
 *   - parseRequiredExpr    "parse or placeholder" helper
 *   - parsePrattExpr       the Pratt loop (precedence climbing)
 *   - parsePrefixExpr      prefix forms: unary, literal, identifier, paren
 *   - parsePrimaryExpr     the primary forms that are not unary
 *   - parsePostfixExpr     postfix forms: call, index, slice, field, `::`,
 *                          pipeline
 *
 * The specific parsers for the primary forms (literal, array, struct,
 * if-expression, identifier, function literal, pipeline) are declared in
 * Parser.hpp and implemented in ParseExprLiterals.cpp and
 * ParseExprFuncLit.cpp. This file calls them.
 *
 * ─── Design: the Pratt loop ───────────────────────────────────────────────
 * The Pratt loop is the standard top-down operator-precedence parser. It
 * works by:
 *
 *   1. Parsing a prefix form (a literal, an identifier, a unary operator
 *      applied to a prefix form, a parenthesized expression, ...).
 *
 *   2. Looping: look at the next token. If it is a postfix operator
 *      (call, index, field access), parse the postfix and update the
 *      left-hand side. If it is an infix operator whose precedence is at
 *      least the loop's minimum, consume the operator and parse the
 *      right-hand side. Otherwise break out of the loop.
 *
 * The "minimum precedence" is what makes precedence climbing work: each
 * recursive call to `parsePrattExpr` sets a floor, and operators whose
 * precedence is below the floor are left for the enclosing call.
 *
 * ─── Design: assignment and `??` are handled before the precedence cutoff
 * ────────────────────────────────────────────────────────────────────────
 * Assignment (`=`, `+=`, ...) and null-coalescing (`??`) have two
 * properties that differ from the standard binary operators:
 *
 *   - They are right-associative: `a = b = c` parses as `a = (b = c)`.
 *   - Their precedence is looser than any binary operator, but they are
 *     not part of `infixPrec`'s standard table.
 *
 * The Pratt loop handles both specially: it checks for assignment and
 * `??` *before* the precedence cutoff, so the operator is always
 * consumed regardless of `minPrec`. The right-associativity comes from
 * the recursive call inside the infix handler using the operator's own
 * precedence level, which lets the inner call consume another instance
 * of the same operator.
 *
 * ─── Design: `and`/`or` are identifiers, not keywords ─────────────────────
 * The grammar lists `and` and `or` as operators with precedence, but
 * they are not boot-set keywords — they are identifiers. The Pratt loop
 * has to detect them by checking an identifier token's *value* rather
 * than its type.
 *
 * This file's `parsePrattExpr` checks for `and`/`or` in the identifier
 * case: when the current token is an IDENTIFIER and its value is "and"
 * or "or", the loop treats it as a binary operator with the appropriate
 * precedence. If a future design promotes them to keywords, this check
 * becomes a normal token-type dispatch.
 *
 * ─── Design: `!` on a call inside a pipeline ──────────────────────────────
 * The `!` argument-pack marker (`f(args)!`) is only valid inside a
 * pipeline step. The parser accepts it at any call and marks the call's
 * `hasArgPack` field; Sema rejects a `hasArgPack` call outside a
 * pipeline step. The parser does not distinguish pipeline context from
 * call context; that distinction is a semantic one.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// Local predicates
// =============================================================================

namespace {

/// @brief True if the token can start a postfix operator.
///
/// Postfix operators extend the current left-hand side without introducing
/// a new prefix form. They bind tighter than any infix operator:
/// `f(x).field[0]` parses as `(((f(x)).field)[0])`.
///
/// The postfix forms are:
///
///   - `(`       a call
///   - `[`       an index or a slice
///   - `.`       a field access or an enum variant access
///   - `::`      a module access or a static struct member access
///   - `|>`      a pipeline step
///
/// `|>` is listed here because the pipeline extends the current
/// expression to the right as a sequence of steps, which is a postfix
/// operation on the seed. It is handled by `parsePipelineExpr` in
/// ParseExprFuncLit.cpp.
bool isPostfixStart(TokenType t) {
    return t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::DOT
        || t == TokenType::DOUBLE_COLON
        || t == TokenType::PIPELINE;
}

/// @brief True if the token is an assignment operator.
///
/// Assignment operators are handled specially by the Pratt loop: they
/// are checked before the precedence cutoff, because their precedence is
/// not a value in `infixPrec`.
bool isAssignmentOp(TokenType t) {
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

/// @brief True if the current token is the identifier `and` or `or`.
///
/// `and` and `or` are not keywords; they are ordinary identifiers whose
/// meaning the Pratt loop recognizes by value. This predicate is used by
/// `parsePrattExpr` to decide whether an IDENTIFIER token should be
/// treated as a binary operator.
bool isAndOrIdentifier(TokenStream& stream) {
    if (!stream.check(TokenType::IDENTIFIER)) return false;
    const std::string& v = stream.peek().value;
    return v == "and" || v == "or";
}

} // namespace

// =============================================================================
// parseExpr — the entry point
// =============================================================================

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx) {
    if (stream.isAtEnd()) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an expression, but the input ended");
        return nullptr;
    }

    // The Pratt loop's minimum precedence is -1, which lets the loop
    // consume every infix operator it encounters (its own precedence
    // floor is only used to terminate recursion, not to filter the
    // outermost call's operators).
    return parsePrattExpr(stream, ctx, -1);
}

// =============================================================================
// parseRequiredExpr — parse-or-placeholder
// =============================================================================

ExprAST* parseRequiredExpr(TokenStream& stream,
                           ParserContext& ctx,
                           const char* expectedWhat) {
    ExprAST* expr = parseExpr(stream, ctx);
    if (expr) return expr;

    // parseExpr already reported a diagnostic. Produce an
    // UnknownExprAST so the caller can continue without a null check at
    // every use site.
    auto* placeholder = ctx.arena().make<UnknownExprAST>();
    placeholder->loc = stream.currentLoc();
    placeholder->hasSyntaxError = true;
    return placeholder;
}

// =============================================================================
// parsePrattExpr — the loop
// =============================================================================

ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec) {
    // ─── Prefix ───────────────────────────────────────────────────────────
    //
    // The prefix form is where an expression starts. Everything that can
    // begin an expression is handled here (or by parsePrimaryExpr, which
    // this function calls). A prefix that fails returns nullptr and the
    // loop returns nullptr to its caller.
    ExprAST* lhs = parsePrefixExpr(stream, ctx);
    if (!lhs) return nullptr;

    // ─── Loop over postfix and infix operators ────────────────────────────
    while (!stream.isAtEnd()) {
        const TokenType current = stream.peekType();

        // ─── Postfix ──────────────────────────────────────────────────────
        //
        // Postfix operators bind tighter than any infix. They extend the
        // current left-hand side: a call, an index, a field access, a
        // pipeline step. They are always consumed regardless of
        // `minPrec`, because their binding power is higher than any
        // infix operator.
        if (isPostfixStart(current)) {
            lhs = parsePostfixExpr(stream, ctx, lhs);
            if (!lhs) return nullptr;
            continue;
        }

        // ─── Assignment ───────────────────────────────────────────────────
        //
        // Assignment is right-associative and its precedence is looser
        // than any binary operator. It is checked before the precedence
        // cutoff so it is always consumed. The infix handler for
        // assignment parses the RHS at the assignment's own precedence,
        // which produces right-associativity.
        if (isAssignmentOp(current)) {
            const TokenType opTok = stream.peekType();
            stream.consume();
            lhs = parseInfixAssign(stream, ctx, lhs, opTok);
            if (!lhs) return nullptr;
            continue;
        }

        // ─── Null coalescing ──────────────────────────────────────────────
        //
        // `??` is right-associative. Like assignment, it is checked
        // before the precedence cutoff.
        if (current == TokenType::QUESTION_QUESTION) {
            stream.consume();
            lhs = parseInfixNullCoalesce(stream, ctx, lhs);
            if (!lhs) return nullptr;
            continue;
        }

        // ─── `and` / `or` as identifiers ──────────────────────────────────
        //
        // These are not keywords in the current grammar; the parser
        // recognizes them by value. Their precedences are:
        //
        //   `and`   2
        //   `or`    1
        //
        // The check happens before the standard infix dispatch, because
        // an identifier that is not `and` or `or` is not an operator at
        // all — the loop breaks and the caller sees the identifier as
        // the next token.
        if (isAndOrIdentifier(stream)) {
            const int prec = (stream.peek().value == "and") ? 2 : 1;
            if (prec < minPrec) break;

            const std::string opValue = stream.peek().value;
            stream.consume();

            // Build the BinaryOp for the operator.
            const BinaryOp op = (opValue == "and") ? BinaryOp::And
                                                   : BinaryOp::Or;

            // Parse the RHS at the operator's own precedence plus one,
            // producing left-associativity (the same rule as for other
            // left-associative binary operators).
            ExprAST* rhs = parsePrattExpr(stream, ctx, prec + 1);
            if (!rhs) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                                   stream.currentLoc(),
                                   "expected right-hand side of '",
                                   opValue, "'");
                return nullptr;
            }

            auto* binary = ctx.arena().make<BinaryExprAST>(op);
            binary->loc = lhs->loc;
            binary->left = lhs;
            binary->right = rhs;
            lhs = binary;
            continue;
        }

        // ─── Standard binary operators ────────────────────────────────────
        //
        // Arithmetic, comparison, bitwise, and range operators. The
        // precedence comes from `infixPrec`. If the precedence is below
        // the loop's floor, the operator belongs to an enclosing
        // recursive call; break out and let the caller handle it.
        const int prec = infixPrec(current);
        if (prec < minPrec) break;

        if (prec >= 0) {
            const TokenType opTok = stream.peekType();
            stream.consume();
            lhs = parseInfixBinary(stream, ctx, lhs, opTok, prec);
            if (!lhs) return nullptr;
            continue;
        }

        // Not an infix operator at this precedence. Break out.
        break;
    }

    return lhs;
}

// =============================================================================
// parsePrefixExpr — unary operators and the primary dispatcher
// =============================================================================

ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Unary operators ──────────────────────────────────────────────────
    //
    // `-`, `not`, `~`. The `not` operator is a keyword? No — under the
    // clean-model design, `not` is not a boot-set keyword. It is an
    // identifier that names an operator, resolved through the DEF table.
    //
    // So the unary operator tokens here are only `-` and `~`. The
    // identifier `not` is handled by the primary parser as an
    // identifier; Sema recognizes it as a unary operator name during
    // overload resolution.
    //
    // This means `not x` at the parser level is an identifier expression
    // followed by another expression, which is a syntax error at the
    // standard parse. To support `not x`, the parser has to recognize
    // `not` as a prefix operator here.
    //
    // We do recognize it: an IDENTIFIER whose value is `not` and which
    // is in a prefix position is treated as a unary operator. The
    // alternative — making `not` a keyword — is a grammar change that
    // would promote `not`, `and`, `or` to keywords together. For now,
    // this file handles all three by value.
    if (current == TokenType::MINUS || current == TokenType::BIT_NOT) {
        stream.consume();   // `-` or `~`

        const UnaryOp op = (current == TokenType::MINUS) ? UnaryOp::Neg
                                                         : UnaryOp::BitNot;

        // Parse the operand at the unary precedence plus one. Unary
        // operators are right-associative: `- - x` parses as `-(-x)`.
        // The precedence for a unary operator is the highest (7 in the
        // grammar's table); using 7 for the recursion ensures the
        // operand parse consumes another unary if one follows.
        constexpr int kUnaryPrec = 7;
        ExprAST* operand = parsePrattExpr(stream, ctx, kUnaryPrec);
        if (!operand) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                               stream.currentLoc(),
                               "expected an operand for unary operator");
            return nullptr;
        }

        auto* unary = ctx.arena().make<UnaryExprAST>(op);
        unary->loc = loc;
        unary->operand = operand;
        return unary;
    }

    // `not x` — the identifier `not` in prefix position.
    if (current == TokenType::IDENTIFIER &&
        stream.peek().value == "not") {
        stream.consume();   // `not`

        constexpr int kUnaryPrec = 7;
        ExprAST* operand = parsePrattExpr(stream, ctx, kUnaryPrec);
        if (!operand) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                               stream.currentLoc(),
                               "expected an operand for 'not'");
            return nullptr;
        }

        auto* unary = ctx.arena().make<UnaryExprAST>(UnaryOp::Not);
        unary->loc = loc;
        unary->operand = operand;
        return unary;
    }

    // ─── Not a unary operator: fall through to the primary parser ─────────
    return parsePrimaryExpr(stream, ctx);
}

// =============================================================================
// parsePrimaryExpr — the primary forms
// =============================================================================

ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Literals ─────────────────────────────────────────────────────────
    if (isLiteral(current)) {
        return parseLiteralExpr(stream, ctx);
    }

    // ─── Array literal: `[` ───────────────────────────────────────────────
    if (current == TokenType::LBRACKET) {
        return parseArrayLiteralExpr(stream, ctx);
    }

    // ─── If-expression: `if cond ?? then else else` ───────────────────────
    if (current == TokenType::KW_IF) {
        return parseIfExpr(stream, ctx);
    }

    // ─── Anonymous function: `fn (...) -> ... { ... }` ────────────────────
    //
    // The literal begins with `fn`. The parser checks this before
    // anything else, because a function literal's `fn` is unambiguous.
    if (current == TokenType::KW_FN_MARKER) {
        return parseAnonFuncExpr(stream, ctx);
    }

    // ─── Parenthesized expression or a function literal with a bare group
    //
    // A bare `(` at expression position could be:
    //   - a parenthesized expression `(a + b)`
    //   - a function literal whose leading marker was omitted
    //     `(a int) -> int { ... }`
    //
    // looksLikeAnonFunc distinguishes them by scanning past the group
    // and checking whether a `{ ... }` body follows.
    if (current == TokenType::LPAREN) {
        if (looksLikeAnonFunc(stream, ctx)) {
            return parseAnonFuncExpr(stream, ctx);
        }

        // Parenthesized expression.
        stream.consume();   // `(`
        if (stream.check(TokenType::RPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_EmptyGroup,
                               stream.currentLoc(),
                               "empty parenthesized expression");
            stream.consume();   // `)`
            return nullptr;
        }

        ExprAST* inner = parseExpr(stream, ctx);
        if (!inner) {
            return nullptr;
        }

        if (!stream.match(TokenType::RPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ')' to close parenthesized "
                               "expression, got '", stream.peekValue(), "'");
            return nullptr;
        }
        return inner;
    }

    // ─── Identifier ───────────────────────────────────────────────────────
    //
    // A bare identifier. This covers:
    //   - local variables and parameters
    //   - function names
    //   - enum type names used before `.Variant`
    //   - generic specialization references (`identity<int>`)
    //   - module names used before `::member`
    //   - struct type names used before a literal (`Point { ... }`)
    //
    // parseIdentifierExpr handles the identifier and its optional generic
    // arguments. It does not consume a following `.`, `::`, `(`, or
    // `{`; those are postfix operators or literal-introducers handled
    // by the caller or by the postfix dispatcher.
    if (current == TokenType::IDENTIFIER) {
        // Peek past the identifier and any generic arguments to see if a
        // struct literal follows.
        const size_t savedPos = stream.getPos();

        Token nameTok = stream.peek();
        InternedString name = ctx.pool().intern(nameTok.value);
        stream.consume();   // identifier

        ArenaSpan<TypeAST*> genericArgs;
        if (stream.check(TokenType::LESS)) {
            genericArgs = parseGenericArgs(stream, ctx);
        }

        const bool isStructLiteral = stream.check(TokenType::LBRACE);
        stream.setPos(savedPos);   // restore for the actual parse

        if (isStructLiteral) {
            // Consume the identifier and generic args for real, then call
            // parseStructLiteralExpr.
            stream.consume();   // identifier
            ArenaSpan<TypeAST*> args;
            if (stream.check(TokenType::LESS)) {
                args = parseGenericArgs(stream, ctx);
            }
            return parseStructLiteralExpr(stream, ctx, name, args);
        }
        return parseIdentifierExpr(stream, ctx);
    }

    // ─── Not a primary form ───────────────────────────────────────────────
    ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                       loc,
                       "expected an expression, got '",
                       stream.peekValue(), "'");
    return nullptr;
}

// =============================================================================
// parsePostfixExpr — the postfix forms
// =============================================================================

ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs) {
    if (!lhs) return nullptr;

    const TokenType current = stream.peekType();

    // ─── Call: `f(args)` or `f(args)!` ────────────────────────────────────
    if (current == TokenType::LPAREN) {
        return parseCallExpr(stream, ctx, lhs);
    }

    // ─── Index or slice: `a[i]`, `a[lo..hi]` ──────────────────────────────
    //
    // The two forms share a starting `[`. The parser peeks past the `[`
    // to determine whether the contents are a single index or a range
    // (a slice). The peek is bounded and non-recursive: it scans tokens
    // at bracket depth 0 inside the `[ ... ]` looking for `..` or `..<`.
    // If it finds one at the top level of the bracket pair, the form is
    // a slice.
    if (current == TokenType::LBRACKET) {
        const bool isSlice = looksLikeSliceStart(stream);
        if (isSlice) {
            return parseSliceExpr(stream, ctx, lhs);
        }
        return parseIndexExpr(stream, ctx, lhs);
    }

    // ─── Field access: `a.b` ──────────────────────────────────────────────
    if (current == TokenType::DOT) {
        return parseFieldAccessExpr(stream, ctx, lhs);
    }

    // ─── Module access or static member access: `a::b` ────────────────────
    //
    // `::` extends an expression with a member of a module or a static
    // member of a struct. The parser does not distinguish the two; it
    // produces a ModuleAccessExprAST, and Sema resolves whether the
    // left-hand side is a module name or a struct name.
    //
    // The left-hand side of `::` must be an identifier (a module name or
    // a struct name). If the source writes `expr::member` with a
    // non-identifier `expr`, that's a syntax error; the current
    // dispatch does not check because parsePrimaryExpr already produced
    // an IdentifierExprAST for the module/struct name in the common
    // case, and a non-identifier left-hand side is rare enough that
    // Sema will catch it.
    if (current == TokenType::DOUBLE_COLON) {
        return parseModuleAccessExpr(stream, ctx);
    }

    // ─── Pipeline: `seed |> step |> step` ─────────────────────────────────
    if (current == TokenType::PIPELINE) {
        return parsePipelineExpr(stream, ctx, lhs);
    }

    // ─── Not a postfix operator ───────────────────────────────────────────
    // The Pratt loop checks `isPostfixStart` before calling this function,
    // so this branch is unreachable. Return `lhs` unchanged as a
    // defensive fallback.
    return lhs;
}

// =============================================================================
// Local helper — slice detection
// =============================================================================

namespace {

/// @brief Peek past a `[` to decide whether the form is a slice or an index.
///
/// A slice's bracket pair contains a top-level `..` or `..<`. An index's
/// does not. The check walks tokens inside the bracket pair at depth 0
/// (bracket depth only; paren depth is not relevant because the
/// expression inside the brackets is not being parsed here — just
/// scanned for a top-level range operator).
///
/// The scan is bounded by the closing `]`. If the bracket pair is
/// malformed (missing the closer), the scan stops at EOF and returns
/// false. The caller (parseIndexExpr or parseSliceExpr) will report the
/// malformed bracket.
///
/// Precondition: the current token is `[`. The stream position is
/// restored before returning.
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

    // Reached EOF without seeing a closing `]` or a range operator.
    stream.setPos(savedPos);
    return false;
}

} // namespace

} // namespace lucid::parser