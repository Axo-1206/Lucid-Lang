/**
 * @file ParseStmt.cpp
 * @brief The statement parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseStmt              dispatch on the current statement token
 *   - parseBlock             `{ stmt* }`
 *   - parseVarDeclStmt       a `let`/`const` inside a block
 *   - parseAssignOrExprStmt  an assignment or an expression statement
 *   - parseReturnStmt        `return [expr] ;`
 *   - parseBreakStmt         `break [label] ;`
 *   - parseContinueStmt      `continue [label] ;`
 *   - parseIfStmt            `if cond { ... } [else ...]`
 *   - parseSwitchStmt        `switch expr { case ...; default: ... }`
 *   - parseSwitchCase        one `case` clause
 *   - parseWhileStmt         `[label:] while cond { ... }`
 *   - parseForStmt           `[label:] for binding [, binding] in iter { ... }`
 *   - parseForBinding        one `for` binding
 *   - parseIterable          a `for` iterable (range or expression)
 *   - the five suspend statements
 *
 * ─── Design: assignment is a statement, not an expression ─────────────────
 * §12: "An `assign_stmt` is a statement only — there is no assignment form
 * in the `expr` grammar." The parser reflects this. `parseExpr` never
 * consumes an assignment operator; the operators `=`, `+=`, `-=`, etc.
 * are not in the Pratt table. So an expression statement's expression
 * stops cleanly before the operator, and `parseAssignOrExprStmt` checks
 * `isAssignmentOperator(peekType())` to decide whether to build an
 * `AssignStmtAST` or an `ExprStmtAST`.
 *
 * No `looksLikeAssignment` helper. The disambiguation is a single check
 * after the LHS is parsed.
 *
 * ─── Design: labels ───────────────────────────────────────────────────────
 * A label is `IDENTIFIER ':'` before a `while` or `for`. The label is a
 * field on the loop statement, not a separate AST node. `break` and
 * `continue` carry an optional label field.
 *
 * ─── Design: recovery in the block loop ───────────────────────────────────
 * `parseBlock`'s loop calls `parseStmt`. If `parseStmt` returns `nullptr`
 * — meaning it could not produce even a marked node — the loop
 * synchronizes to the next block boundary: any statement start, any local
 * declaration start, `;`, or `}`. The stop set is a file-local predicate
 * (`isBlockBoundary`) so the dispatch set and the recovery set stay
 * aligned.
 *
 * ─── Design: top-level declarations inside a block ───────────────────────
 * §12.5 forbids `TABLE` and `FN` inside a block. When `parseStmt` sees one,
 * it reports "not allowed inside a block" at the declaration's own
 * location and then consumes the whole declaration — via `parseTableDecl`
 * or `parseFnDecl` — so the block's brace balance is preserved.
 *
 * Consuming only the keyword would leave the declaration's body in the
 * stream, and the block loop would parse the body as statements, producing
 * one cascading diagnostic per token. Discard-and-report keeps the count
 * at one.
 *
 * This rule is reported by the parser rather than deferred to Sema
 * because the parser is the only layer that sees the misplaced `TABLE`/`FN`
 * token before recovery discards it. Once the declaration is parsed, its
 * AST shape is indistinguishable from a correctly-placed declaration, so
 * Sema cannot tell the two cases apart.
 *
 * ─── Design: the switch's mandatory default ───────────────────────────────
 * §12.2 requires `default` in every `switch`. The parser enforces this at
 * parse time (unusual for this parser, but justified: the missing default
 * changes the shape of the tree, and the LSP wants a `SwitchStmtAST`
 * with a `defaultBody` even when the source omitted one). If `default` is
 * missing, the parser reports a diagnostic and produces a placeholder
 * empty block.
 */

#include "parser/Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"
#include "parser/support/GrammarPositions.hpp"

#include "core/Tokens.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <string>
#include <vector>

using namespace lucid::diag;

// =============================================================================
// File-local helpers
// =============================================================================

namespace {

/// @brief Map an assignment-operator token to its `AssignOp` value.
///
/// Precondition: `type` satisfies `isAssignmentOperator`. The
/// `AssignOp` enum in ExprAST.hpp has no `PowAssign` (the grammar's
/// assign_op set excludes `**=`), so every case is covered.
inline AssignOp tokenToAssignOp(TokenType type) noexcept {
    switch (type) {
        case TokenType::ASSIGN:         return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:    return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:   return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:     return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:     return AssignOp::DivAssign;
        case TokenType::MOD_ASSIGN:     return AssignOp::ModAssign;
        case TokenType::BIT_AND_ASSIGN: return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:  return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN: return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:     return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:     return AssignOp::ShrAssign;
        default:
            // Unreachable: the caller checks isAssignmentOperator first.
            // Return Assign so the function is total.
            return AssignOp::Assign;
    }
}

} // namespace

namespace lucid::parser {

// =============================================================================
// parseStmt — the dispatcher
// =============================================================================

StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx) {
    if (stream.isAtEnd() || !ctx.canContinue()) {
        return nullptr;
    }

    // Skip a stray `;` silently. Empty statements are legal and common
    // (a `;` after a declaration that already consumed its terminator,
    // for instance). The block loop also skips them; this is belt and
    // suspenders for direct callers.
    if (stream.match(TokenType::SEMICOLON)) {
        return nullptr;
    }

    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Local declaration ────────────────────────────────────────────────
    if (current == TokenType::KW_LET || current == TokenType::KW_CONST) {
        return parseVarDeclStmt(stream, ctx);
    }

    // ─── Bare block ───────────────────────────────────────────────────────
    if (current == TokenType::LBRACE) {
        return parseBlock(stream, ctx);
    }

    // ─── Control flow ─────────────────────────────────────────────────────
    switch (current) {
        case TokenType::KW_IF:     return parseIfStmt(stream, ctx);
        case TokenType::KW_WHILE:  return parseWhileStmt(stream, ctx);
        case TokenType::KW_FOR:    return parseForStmt(stream, ctx);
        case TokenType::KW_SWITCH: return parseSwitchStmt(stream, ctx);
        default: break;
    }

    // ─── Jumps ────────────────────────────────────────────────────────────
    switch (current) {
        case TokenType::KW_RETURN:   return parseReturnStmt(stream, ctx);
        case TokenType::KW_BREAK:    return parseBreakStmt(stream, ctx);
        case TokenType::KW_CONTINUE: return parseContinueStmt(stream, ctx);
        default: break;
    }

    // ─── Suspend statements ───────────────────────────────────────────────
    switch (current) {
        case TokenType::KW_WAIT:              return parseWaitStmt(stream, ctx);
        case TokenType::KW_WAIT_FRAMES:       return parseWaitFramesStmt(stream, ctx);
        case TokenType::KW_WAIT_UNTIL:        return parseWaitUntilStmt(stream, ctx);
        case TokenType::KW_WAIT_FOR_EVENT:    return parseWaitForEventStmt(stream, ctx);
        case TokenType::KW_WAIT_FOR_REQUEST:  return parseWaitForRequestStmt(stream, ctx);
        default: break;
    }

    // ─── Constructs that are not allowed inside a block ───────────────────
    //
    // A `TABLE` or `FN` inside a block is a top-level declaration in the
    // wrong place. The parser reports it here, at the declaration's own
    // location, and then *consumes the whole declaration* — body and all —
    // so the block's brace balance is preserved and the block loop
    // continues with the next statement.
    //
    // Consuming only the keyword (the previous behavior) left the body in
    // the stream. The block loop then parsed the body's tokens as
    // statements, producing one cascading diagnostic per token. Discard-
    // and-report keeps the diagnostic count at one: the misplaced
    // declaration, reported once, at the right location.
    //
    // This is a rare case where the parser reports a rule that §12.5
    // would otherwise leave to Sema. The parser has to, because it is the
    // only layer that sees the `TABLE`/`FN` token *before* recovery
    // discards it. Once the declaration is parsed into the AST, the
    // shape it produces is indistinguishable from a correctly-placed
    // declaration reached through an unwind — so Sema cannot tell the
    // two cases apart. The parser can, and does.
    if (current == TokenType::KW_FIXED || current == TokenType::KW_TABLE) {
        ctx.diag.errorAt(
            DiagCode::Syntax_UnexpectedToken, loc,
            current == TokenType::KW_FIXED
                ? "a FIXED TABLE declaration is not allowed inside a block; "
                  "move it to the top level"
                : "a TABLE declaration is not allowed inside a block; "
                  "move it to the top level");

        // Consume the whole declaration. parseTableDecl reports its own
        // errors if the declaration is malformed; those are independent
        // of the misplacement, so letting them fire is correct. The
        // result is discarded — the block has no place for it.
        (void)parseTableDecl(stream, ctx);
        return nullptr;
    }
    if (current == TokenType::KW_FN) {
        ctx.diag.errorAt(
            DiagCode::Syntax_UnexpectedToken, loc,
            "an FN declaration is not allowed inside a block; "
            "move it to the top level");

        // Consume the whole function. Same reasoning as the table case.
        (void)parseFnDecl(stream, ctx);
        return nullptr;
    }
    if (current == TokenType::AT_SIGN) {
        ctx.diag.errorAt(
            DiagCode::Syntax_UnexpectedToken, loc,
            "an attribute is not allowed on a statement; "
            "attributes precede only TABLE, FN, and column declarations");

        // An `@` at statement level is always misplaced. Consuming just
        // the `@` is correct here: the attribute's own tokens (its name
        // and argument list) are not a construct the block loop can
        // mistake for a statement, and the declaration the attribute
        // belonged to — if there was one — begins with `TABLE`/`FN` and
        // is handled by the branches above. Consume the `@` and let the
        // block loop report the next token if it is not a declaration
        // keyword.
        stream.consume();
        return nullptr;
    }

    // ─── Expression or assignment ─────────────────────────────────────────
    if (canStartExpression(current)) {
        return parseAssignOrExprStmt(stream, ctx);
    }

    // ─── Not a statement ──────────────────────────────────────────────────
    ctx.diag.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                       "expected a statement, got '",
                       stream.peekValueView(ctx.pool), "'");
    stream.consume();
    return nullptr;
}

// =============================================================================
// parseBlock — `{ stmt* }`
// =============================================================================

BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock, loc,
                           "expected '{' to open a block, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    std::vector<StmtAST*> stmts;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray semicolons. They are legal and harmless.
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        const size_t posBefore = stream.getPos();

        StmtAST* stmt = parseStmt(stream, ctx);
        if (stmt != nullptr) {
            stmts.push_back(stmt);
            continue;
        }

        // parseStmt returned nullptr. If it also failed to advance the
        // stream, we must synchronize to avoid an infinite loop. If it
        // did advance, the next iteration will handle whatever comes
        // next.
        if (stream.getPos() == posBefore) {
            synchronizeUntil(stream, [](TokenType t) {
                return isBlockBoundary(t);
            });

            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }
    }

    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close the block, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto* block = ctx.arena.make<BlockStmtAST>();
    block->loc = loc;
    if (!stmts.empty()) {
        auto builder = ctx.arena.makeBuilder<StmtAST*>(stmts.size());
        for (StmtAST* s : stmts) builder.push_back(s);
        block->stmts = builder.build();
    }
    return block;
}

// =============================================================================
// parseVarDeclStmt — a `let`/`const` inside a block
// =============================================================================

VarDeclStmtAST* parseVarDeclStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // parseVarDecl parses the declaration and stops at the first token
    // that cannot continue it (§12.6). It does not consume a `;`; a
    // stray `;` after the declaration is an empty statement the block
    // loop skips.
    VarDeclAST* decl = parseVarDecl(stream, ctx);
    if (decl == nullptr) {
        return nullptr;
    }

    auto* stmt = ctx.arena.make<VarDeclStmtAST>(decl);
    stmt->loc = loc;
    if (decl->hasSyntaxError) stmt->hasSyntaxError = true;
    return stmt;
}

// =============================================================================
// parseAssignOrExprStmt — `lvalue op expr;` or `expr;`
// =============================================================================

StmtAST* parseAssignOrExprStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    ExprAST* lhs = parseExpr(stream, ctx);
    if (lhs == nullptr) {
        // parseExpr reported. The block loop's recovery handles the rest.
        return nullptr;
    }

    // Assignment operator?
    if (isAssignmentOperator(stream.peekType())) {
        const TokenType opTok = stream.peekType();
        stream.consume();

        const AssignOp op = tokenToAssignOp(opTok);

        ExprAST* rhs = parseRequiredExpr(stream, ctx, "assignment value");

        auto* stmt = ctx.arena.make<AssignStmtAST>(op);
        stmt->loc = loc;
        stmt->lhs = lhs;
        stmt->rhs = rhs;
        if (lhs->hasSyntaxError || rhs->hasSyntaxError) {
            stmt->hasSyntaxError = true;
        }

        return stmt;
    }

    // Otherwise: an expression statement.
    auto* stmt = ctx.arena.make<ExprStmtAST>(lhs);
    stmt->loc = loc;
    if (lhs->hasSyntaxError) stmt->hasSyntaxError = true;

    return stmt;
}

// =============================================================================
// parseReturnStmt — `return [expr] ;`
// =============================================================================

ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_RETURN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'return', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* ret = ctx.arena.make<ReturnStmtAST>();
    ret->loc = loc;

    // A bare `return` is legal. A value-returning `return` is followed
    // by an expression. §12.6: the operand is present when the next
    // token can begin an expression.
    if (canStartExpression(stream.peekType())) {
        ExprAST* value = parseRequiredExpr(stream, ctx, "return value");
        ret->value = value;
        if (value != nullptr && value->hasSyntaxError) {
            ret->hasSyntaxError = true;
        }
    }

    return ret;
}

// =============================================================================
// parseBreakStmt — `break [label] ;`
// =============================================================================

BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_BREAK)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'break', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<BreakStmtAST>();
    stmt->loc = loc;

    if (stream.check(TokenType::IDENTIFIER)) {
        Token labelTok = stream.consume();
        stmt->label = labelTok.value;
    }

    return stmt;
}

// =============================================================================
// parseContinueStmt — `continue [label] ;`
// =============================================================================

ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_CONTINUE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'continue', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<ContinueStmtAST>();
    stmt->loc = loc;

    if (stream.check(TokenType::IDENTIFIER)) {
        Token labelTok = stream.consume();
        stmt->label = labelTok.value;
    }

    return stmt;
}

// =============================================================================
// parseIfStmt — `if cond { ... } [else ...]`
// =============================================================================

IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_IF)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'if', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Condition ────────────────────────────────────────────────────────
    ExprAST* condition = parseRequiredExpr(stream, ctx, "condition");

    // ─── Then-branch ──────────────────────────────────────────────────────
    BlockStmtAST* thenBranch = nullptr;
    if (stream.check(TokenType::LBRACE)) {
        thenBranch = parseBlock(stream, ctx);
    } else {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the then-branch, got '",
                           stream.peekValueView(ctx.pool), "'");

        // Partial-parse: an empty marked block.
        thenBranch = ctx.arena.make<BlockStmtAST>();
        thenBranch->loc = stream.currentLoc();
        thenBranch->hasSyntaxError = true;

        // Synchronize to a plausible `}` or `else` boundary.
        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::LBRACE
                || t == TokenType::KW_ELSE
                || isBlockBoundary(t);
        });
    }

    auto* ifStmt = ctx.arena.make<IfStmtAST>();
    ifStmt->loc = loc;
    ifStmt->condition = condition;
    ifStmt->thenBranch = thenBranch;

    // ─── Optional else-branch ─────────────────────────────────────────────
    if (stream.match(TokenType::KW_ELSE)) {
        if (stream.check(TokenType::KW_IF)) {
            // `else if` — a chained if-statement.
            StmtAST* nested = parseIfStmt(stream, ctx);
            ifStmt->elseBranch = nested;
        } else if (stream.check(TokenType::LBRACE)) {
            // `else { ... }` — an else block.
            BlockStmtAST* elseBlock = parseBlock(stream, ctx);
            ifStmt->elseBranch = elseBlock;
        } else {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                               stream.currentLoc(),
                               "expected 'if' or '{' after 'else', got '",
                               stream.peekValueView(ctx.pool), "'");

            auto* placeholder = ctx.arena.make<BlockStmtAST>();
            placeholder->loc = stream.currentLoc();
            placeholder->hasSyntaxError = true;
            ifStmt->elseBranch = placeholder;
            ifStmt->hasSyntaxError = true;
        }
    }

    if (thenBranch != nullptr && thenBranch->hasSyntaxError) {
        ifStmt->hasSyntaxError = true;
    }
    if (ifStmt->elseBranch != nullptr && ifStmt->elseBranch->hasSyntaxError) {
        ifStmt->hasSyntaxError = true;
    }
    return ifStmt;
}

// =============================================================================
// parseSwitchStmt — `switch expr { case ...; default: ... }`
// =============================================================================

SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_SWITCH)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'switch', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Subject ──────────────────────────────────────────────────────────
    ExprAST* subject = parseRequiredExpr(stream, ctx, "switch subject");

    // ─── `{` ──────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' after the switch subject, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* switchStmt = ctx.arena.make<SwitchStmtAST>();
    switchStmt->loc = loc;
    switchStmt->subject = subject;

    // ─── Cases and default ────────────────────────────────────────────────
    std::vector<SwitchCaseAST*> cases;
    BlockStmtAST* defaultBody = nullptr;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip free separators.
        if (stream.match(TokenType::SEMICOLON) ||
            stream.match(TokenType::COMMA)) {
            continue;
        }

        if (stream.check(TokenType::KW_CASE)) {
            SwitchCaseAST* c = parseSwitchCase(stream, ctx);
            if (c != nullptr) cases.push_back(c);
            continue;
        }

        if (stream.check(TokenType::KW_DEFAULT)) {
            const SourceLocation defaultLoc = stream.currentLoc();
            stream.consume();   // `default`

            if (defaultBody != nullptr) {
                ctx.diag.errorAt(DiagCode::Syntax_MultipleDefaults,
                                   defaultLoc,
                                   "duplicate 'default' clause in switch");
                // Consume the duplicate's body if it's a block.
                if (stream.match(TokenType::COLON)) {
                    if (stream.check(TokenType::LBRACE)) {
                        parseBlock(stream, ctx);   // discard
                    }
                }
                continue;
            }

            if (!stream.match(TokenType::COLON)) {
                ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                                   stream.currentLoc(),
                                   "expected ':' after 'default', got '",
                                   stream.peekValueView(ctx.pool), "'");
            }

            if (stream.check(TokenType::LBRACE)) {
                defaultBody = parseBlock(stream, ctx);
            } else {
                ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                                   stream.currentLoc(),
                                   "expected '{' for the default body, got '",
                                   stream.peekValueView(ctx.pool), "'");
                // Partial-parse: an empty marked block.
                defaultBody = ctx.arena.make<BlockStmtAST>();
                defaultBody->loc = defaultLoc;
                defaultBody->hasSyntaxError = true;
            }

            switchStmt->defaultLoc = defaultLoc;
            continue;
        }

        ctx.diag.errorAt(DiagCode::Syntax_UnexpectedToken,
                           stream.currentLoc(),
                           "expected 'case' or 'default' inside switch, got '",
                           stream.peekValueView(ctx.pool), "'");

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::KW_CASE
                || t == TokenType::KW_DEFAULT
                || t == TokenType::RBRACE;
        });
    }

    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close the switch, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    // ─── Mandatory default ────────────────────────────────────────────────
    if (defaultBody == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "every 'switch' must have a 'default' clause");
        defaultBody = ctx.arena.make<BlockStmtAST>();
        defaultBody->loc = stream.currentLoc();
        defaultBody->hasSyntaxError = true;
    }

    if (!cases.empty()) {
        auto builder = ctx.arena.makeBuilder<SwitchCaseAST*>(cases.size());
        for (SwitchCaseAST* c : cases) builder.push_back(c);
        switchStmt->cases = builder.build();
    }
    switchStmt->defaultBody = defaultBody;
    if (defaultBody->hasSyntaxError) switchStmt->hasSyntaxError = true;
    return switchStmt;
}

// =============================================================================
// parseSwitchCase — `case v1, v2, ... : { ... }`
// =============================================================================

SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_CASE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'case', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* caseNode = ctx.arena.make<SwitchCaseAST>();
    caseNode->loc = loc;

    // ─── Case values ──────────────────────────────────────────────────────
    std::vector<ExprAST*> values;

    while (!stream.isAtEnd() && !stream.check(TokenType::COLON) &&
           ctx.canContinue()) {
        ExprAST* value = parseRequiredExpr(stream, ctx, "case value");
        values.push_back(value);

        if (stream.match(TokenType::COMMA)) {
            // Trailing comma before `:` is not permitted.
            if (stream.check(TokenType::COLON)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in case value list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::COLON)) {
            break;
        }

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ':' after a case value, got '",
                           stream.peekValueView(ctx.pool), "'");

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::COMMA
                || t == TokenType::COLON;
        });
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::COLON)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after the case value(s), got '",
                           stream.peekValueView(ctx.pool), "'");
        caseNode->hasSyntaxError = true;
    }

    if (!values.empty()) {
        auto builder = ctx.arena.makeBuilder<ExprAST*>(values.size());
        for (ExprAST* v : values) builder.push_back(v);
        caseNode->values = builder.build();
    }

    // ─── Body ─────────────────────────────────────────────────────────────
    if (stream.check(TokenType::LBRACE)) {
        caseNode->body = parseBlock(stream, ctx);
        if (caseNode->body != nullptr && caseNode->body->hasSyntaxError) {
            caseNode->hasSyntaxError = true;
        }
    } else {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the case body, got '",
                           stream.peekValueView(ctx.pool), "'");

        caseNode->hasSyntaxError = true;

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::KW_CASE
                || t == TokenType::KW_DEFAULT
                || t == TokenType::RBRACE;
        });
    }

    return caseNode;
}

// =============================================================================
// parseWhileStmt — `[label:] while cond { ... }`
// =============================================================================

WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Optional label ───────────────────────────────────────────────────
    InternedString label;
    if (stream.check(TokenType::IDENTIFIER) &&
        stream.peekNextType() == TokenType::COLON &&
        stream.peekAt(2).type == TokenType::KW_WHILE) {
        Token labelTok = stream.consume();
        label = labelTok.value;
        stream.consume();   // `:`
    }

    if (!stream.match(TokenType::KW_WHILE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'while', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    ExprAST* condition = parseRequiredExpr(stream, ctx, "while condition");

    BlockStmtAST* body = nullptr;
    if (stream.check(TokenType::LBRACE)) {
        body = parseBlock(stream, ctx);
    } else {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the while body, got '",
                           stream.peekValueView(ctx.pool), "'");

        body = ctx.arena.make<BlockStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;

        synchronizeUntil(stream, [](TokenType t) {
            return isBlockBoundary(t);
        });
    }

    auto* whileStmt = ctx.arena.make<WhileStmtAST>();
    whileStmt->loc = loc;
    whileStmt->label = label;
    whileStmt->condition = condition;
    whileStmt->body = body;
    if (body != nullptr && body->hasSyntaxError) whileStmt->hasSyntaxError = true;
    return whileStmt;
}

// =============================================================================
// parseForStmt — `[label:] for binding [, binding] in iterable { ... }`
// =============================================================================

ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Optional label ───────────────────────────────────────────────────
    InternedString label;
    if (stream.check(TokenType::IDENTIFIER) &&
        stream.peekNextType() == TokenType::COLON &&
        stream.peekAt(2).type == TokenType::KW_FOR) {
        Token labelTok = stream.consume();
        label = labelTok.value;
        stream.consume();   // `:`
    }

    if (!stream.match(TokenType::KW_FOR)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'for', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* forStmt = ctx.arena.make<ForStmtAST>();
    forStmt->loc = loc;
    forStmt->label = label;

    // ─── First binding ────────────────────────────────────────────────────
    ParamAST* firstVar = parseForBinding(stream, ctx);
    if (firstVar == nullptr && !stream.check(TokenType::COMMA) &&
        !stream.check(TokenType::KW_IN)) {
        // parseForBinding reports its own error. If the next token is not
        // a comma or `in`, we cannot continue this for-loop.
        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::KW_IN
                || isBlockBoundary(t);
        });
    } else {
        forStmt->firstVar = firstVar;
    }

    // ─── Optional second binding ──────────────────────────────────────────
    if (stream.match(TokenType::COMMA)) {
        ParamAST* secondVar = parseForBinding(stream, ctx);
        forStmt->secondVar = secondVar;
    }

    // ─── `in` ─────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::KW_IN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected 'in' after the loop binding(s), got '",
                           stream.peekValueView(ctx.pool), "'");
        forStmt->hasSyntaxError = true;

        synchronizeUntil(stream, [](TokenType t) {
            return isBlockBoundary(t);
        });
    } else {
        // ─── Iterable ─────────────────────────────────────────────────────
        ExprAST* iterable = parseExpr(stream, ctx);
        forStmt->iterable = iterable;
        if (iterable != nullptr && iterable->hasSyntaxError) {
            forStmt->hasSyntaxError = true;
        }
    }

    // ─── Body ─────────────────────────────────────────────────────────────
    if (stream.check(TokenType::LBRACE)) {
        forStmt->body = parseBlock(stream, ctx);
    } else {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the for body, got '",
                           stream.peekValueView(ctx.pool), "'");

        auto* body = ctx.arena.make<BlockStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;
        forStmt->body = body;
        forStmt->hasSyntaxError = true;

        synchronizeUntil(stream, [](TokenType t) {
            return isBlockBoundary(t);
        });
    }

    return forStmt;
}

// =============================================================================
// parseForBinding — `name: type` or `_`
// =============================================================================

ParamAST* parseForBinding(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Discard `_` ──────────────────────────────────────────────────────
    //
    // `_` is an ordinary identifier whose spelling is "_" (§2.3: "`_`
    // alone is a valid identifier, used as a discard binding"). The
    // parser recognizes it by value, not by token type.
    if (stream.check(TokenType::IDENTIFIER) &&
        stream.peekValueView(ctx.pool) == "_") {
        stream.consume();   // `_`
        return nullptr;     // a discard has no ParamAST
    }

    // ─── Named binding ────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedForBinding,
                           stream.currentLoc(),
                           "expected a loop binding name or '_', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    if (!stream.match(TokenType::COLON)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after loop binding '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        auto* unkType = ctx.arena.make<UnknownTypeAST>();
        unkType->loc = stream.currentLoc();
        unkType->hasSyntaxError = true;

        auto* param = ctx.arena.make<ParamAST>(
            name, unkType, /*isVariadic=*/false, /*isConst=*/false);
        param->loc = loc;
        param->hasSyntaxError = true;
        return param;
    }

    TypeAST* type = parseType(stream, ctx);
    if (type == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type for loop binding '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        type = ctx.arena.make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    auto* param = ctx.arena.make<ParamAST>(
        name, type, /*isVariadic=*/false, /*isConst=*/false);
    param->loc = loc;
    if (type->hasSyntaxError) param->hasSyntaxError = true;
    return param;
}

// =============================================================================
// Suspend statements
// =============================================================================

namespace {

/// @brief A small helper that reads the argument inside a suspend
///        statement's parentheses.
///
/// The caller has consumed the keyword. The form is `( expr )`. Returns
/// an `UnknownExprAST` on failure, marked.
ExprAST* readSuspendArg(TokenStream& stream,
                        ParserContext& ctx,
                        const char* keyword) {
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' after '", keyword, "', got '",
                           stream.peekValueView(ctx.pool), "'");
        auto* unk = ctx.arena.make<UnknownExprAST>();
        unk->loc = stream.currentLoc();
        unk->hasSyntaxError = true;
        return unk;
    }

    ExprAST* arg = parseRequiredExpr(stream, ctx, "argument");

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close '", keyword,
                           "' arguments, got '",
                           stream.peekValueView(ctx.pool), "'");
        arg->hasSyntaxError = true;
    }

    return arg;
}

} // namespace

WaitStmtAST* parseWaitStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    if (!stream.match(TokenType::KW_WAIT)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'wait', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<WaitStmtAST>();
    stmt->loc = loc;
    stmt->seconds = readSuspendArg(stream, ctx, "wait");
    if (stmt->seconds->hasSyntaxError) stmt->hasSyntaxError = true;

    return stmt;
}

WaitFramesStmtAST* parseWaitFramesStmt(TokenStream& stream,
                                       ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    if (!stream.match(TokenType::KW_WAIT_FRAMES)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'waitFrames', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<WaitFramesStmtAST>();
    stmt->loc = loc;
    stmt->frames = readSuspendArg(stream, ctx, "waitFrames");
    if (stmt->frames->hasSyntaxError) stmt->hasSyntaxError = true;

    return stmt;
}

WaitUntilStmtAST* parseWaitUntilStmt(TokenStream& stream,
                                     ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    if (!stream.match(TokenType::KW_WAIT_UNTIL)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'waitUntil', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<WaitUntilStmtAST>();
    stmt->loc = loc;

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' after 'waitUntil', got '",
                           stream.peekValueView(ctx.pool), "'");
        stmt->hasSyntaxError = true;
        return stmt;
    }

    stmt->predicate = parseRequiredExpr(stream, ctx, "predicate");

    if (!stream.match(TokenType::COMMA)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' between waitUntil's predicate and "
                           "argument, got '",
                           stream.peekValueView(ctx.pool), "'");
        stmt->hasSyntaxError = true;
    } else {
        stmt->arg = parseRequiredExpr(stream, ctx, "waitUntil argument");
    }

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close 'waitUntil' arguments, got '",
                           stream.peekValueView(ctx.pool), "'");
        stmt->hasSyntaxError = true;
    }

    return stmt;
}

WaitForEventStmtAST* parseWaitForEventStmt(TokenStream& stream,
                                           ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    if (!stream.match(TokenType::KW_WAIT_FOR_EVENT)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'waitForEvent', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<WaitForEventStmtAST>();
    stmt->loc = loc;
    stmt->event = readSuspendArg(stream, ctx, "waitForEvent");
    if (stmt->event->hasSyntaxError) stmt->hasSyntaxError = true;

    return stmt;
}

WaitForRequestStmtAST* parseWaitForRequestStmt(TokenStream& stream,
                                               ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    if (!stream.match(TokenType::KW_WAIT_FOR_REQUEST)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'waitForRequest', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* stmt = ctx.arena.make<WaitForRequestStmtAST>();
    stmt->loc = loc;
    stmt->request = readSuspendArg(stream, ctx, "waitForRequest");
    if (stmt->request->hasSyntaxError) stmt->hasSyntaxError = true;

    return stmt;
}

} // namespace lucid::parser