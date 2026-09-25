/**
 * @file ParseStmt.cpp
 * @brief The statement parsers, except the concurrency statements.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseStmt          dispatch on the current statement keyword
 *   - parseBlock         `{ stmt* }`
 *   - parseIfStmt        `if cond { ... } [else ...]`
 *   - parseSwitchStmt    `switch subject { case ...: { ... } ... }`
 *   - parseSwitchCase    one `case` clause inside a switch
 *   - parseForStmt       `for x T in iterable { ... }`
 *   - parseWhileStmt     `while cond { ... }`
 *   - parseDoWhileStmt   `do { ... } while cond;`
 *   - parseReturnStmt    `return [expr];`
 *   - parseBreakStmt     `break;`
 *   - parseContinueStmt  `continue;`
 *   - parseExprStmt      an expression used as a statement
 *   - parseDeclStmt      a local declaration inside a block
 *
 * The concurrency statements — `spawn`, `start`, `await` — are declared
 * in Parser.hpp and implemented in ParseStmtConcurrency.cpp. See that
 * file for the fiber-related forms.
 *
 * ─── Design: dispatch is by leading keyword ───────────────────────────────
 * `parseStmt` looks at the current token and dispatches:
 *
 *   - a declaration keyword (`let`, `const`, `FN`, `TYPE`, `struct`,
 *     `enum`, `trait`, `satisfy`, `DEF`, `@`) → `parseDeclStmt`
 *   - a control-flow keyword (`if`, `while`, `do`, `for`, `switch`) →
 *     the matching parser
 *   - a jump keyword (`return`, `break`, `continue`) → the matching
 *     parser
 *   - a concurrency keyword (`spawn`, `start`, `await`) → the matching
 *     parser in ParseStmtConcurrency.cpp
 *   - anything else that can begin an expression → `parseExprStmt`
 *
 * `parseStmt` does not consume a stray `;`; the block parser skips those
 * before calling `parseStmt`.
 *
 * ─── Design: statement terminators ────────────────────────────────────────
 * Every statement in the grammar either ends with a `}` (a block form) or
 * with a `;`. The block forms — `if`, `while`, `for`, `switch` — end
 * with the block's `}` and do not have a trailing `;`. The expression
 * forms — `return`, `break`, `continue`, `spawn`, `start`, `await`,
 * expression statements — end with `;`.
 *
 * Each parser in this file consumes its own terminator. The block forms
 * consume the block's `}`; the expression forms consume the `;`.
 *
 * `do`/`while` is the outlier: it ends with a `;` after the condition,
 * because the last thing written is the condition, not a block.
 *
 * ─── Design: if-statement vs. if-expression ───────────────────────────────
 * `if` has both a statement form and an expression form. The two are
 * distinguished by the presence of `??` after the condition:
 *
 *   `if x { ... }`        a statement (no `??`)
 *   `if x ?? a else b`    an expression (with `??`)
 *
 * The statement parser reads the condition and then expects a block. If
 * the source wrote `if x ?? a else b`, the `??` appears where the block
 * should be; the statement parser reports "expected '{'" and the
 * expression form is never reached from this path. The expression form
 * is only reachable from expression position (via `parsePrimaryExpr`'s
 * `KW_IF` dispatch).
 *
 * This is the right behavior: in statement position, `if` is a
 * statement; in expression position, `if` is an expression. The
 * parser's position tells it which to expect, and a mismatched form is
 * a syntax error at the point where the two forms diverge.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/DeclAST.hpp"

using namespace lucid::diag;

#include <vector>

namespace lucid::parser {

// =============================================================================
// Local predicates
// =============================================================================

namespace {

/// @brief True if the token can begin a local declaration.
///
/// Local declarations are the same forms as top-level declarations,
/// except `import`, which is top-level only.
bool isLocalDeclStart(TokenType t) {
    return t == TokenType::KW_LET
        || t == TokenType::KW_CONST
        || t == TokenType::KW_FN
        || t == TokenType::KW_TYPE
        || t == TokenType::KW_STRUCT
        || t == TokenType::KW_ENUM
        || t == TokenType::KW_TRAIT
        || t == TokenType::KW_SATISFY
        || t == TokenType::KW_DEF
        || t == TokenType::AT_SIGN;   // attributes before a declaration
}

/// @brief True if the token is a control-flow or jump keyword.
bool isControlFlowStart(TokenType t) {
    return t == TokenType::KW_IF
        || t == TokenType::KW_WHILE
        || t == TokenType::KW_DO
        || t == TokenType::KW_FOR
        || t == TokenType::KW_SWITCH
        || t == TokenType::KW_RETURN
        || t == TokenType::KW_BREAK
        || t == TokenType::KW_CONTINUE;
}

/// @brief True if the token is a concurrency-statement keyword.
bool isConcurrencyStmtStart(TokenType t) {
    return t == TokenType::KW_SPAWN
        || t == TokenType::KW_START
        || t == TokenType::KW_AWAIT;
}

/// @brief True if the token can begin an expression.
///
/// Used by `parseStmt`'s fallthrough to decide whether to call
/// `parseExprStmt`. Any token that can start a prefix expression counts.
bool canStartStmtExpression(TokenType t) {
    return isLiteral(t)
        || t == TokenType::IDENTIFIER
        || t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::MINUS
        || t == TokenType::BIT_NOT
        || t == TokenType::KW_IF
        || t == TokenType::KW_FN_MARKER;
}

} // namespace

// =============================================================================
// parseStmt — the dispatcher
// =============================================================================

StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx) {
    if (stream.isAtEnd() || !ctx.canContinue()) {
        return nullptr;
    }

    // Skip a stray `;` silently. Empty statements are legal and common
    // (a declaration followed by `;` at statement position, for
    // instance). The block parser also skips them; this is belt and
    // suspenders for direct callers.
    if (stream.match(TokenType::SEMICOLON)) {
        return nullptr;
    }

    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Local declaration ────────────────────────────────────────────────
    if (isLocalDeclStart(current)) {
        return parseDeclStmt(stream, ctx);
    }

    // ─── Control flow ─────────────────────────────────────────────────────
    if (current == TokenType::KW_IF) {
        return parseIfStmt(stream, ctx);
    }
    if (current == TokenType::KW_WHILE) {
        return parseWhileStmt(stream, ctx);
    }
    if (current == TokenType::KW_DO) {
        DoWhileStmtAST* stmt = parseDoWhileStmt(stream, ctx);
        // `do`/`while` ends with a `;` after the condition.
        if (stmt) {
            if (!stream.match(TokenType::SEMICOLON)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                                   stream.currentLoc(),
                                   "expected ';' after do-while statement, "
                                   "got '", stream.peekValue(), "'");
                stmt->hasSyntaxError = true;
            }
        }
        return stmt;
    }
    if (current == TokenType::KW_FOR) {
        return parseForStmt(stream, ctx);
    }
    if (current == TokenType::KW_SWITCH) {
        return parseSwitchStmt(stream, ctx);
    }

    // ─── Jumps ────────────────────────────────────────────────────────────
    if (current == TokenType::KW_RETURN) {
        ReturnStmtAST* stmt = parseReturnStmt(stream, ctx);
        if (stmt && !stream.match(TokenType::SEMICOLON)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ';' after return statement, got '",
                               stream.peekValue(), "'");
            stmt->hasSyntaxError = true;
        }
        return stmt;
    }
    if (current == TokenType::KW_BREAK) {
        BreakStmtAST* stmt = parseBreakStmt(stream, ctx);
        if (stmt && !stream.match(TokenType::SEMICOLON)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ';' after 'break', got '",
                               stream.peekValue(), "'");
            stmt->hasSyntaxError = true;
        }
        return stmt;
    }
    if (current == TokenType::KW_CONTINUE) {
        ContinueStmtAST* stmt = parseContinueStmt(stream, ctx);
        if (stmt && !stream.match(TokenType::SEMICOLON)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ';' after 'continue', got '",
                               stream.peekValue(), "'");
            stmt->hasSyntaxError = true;
        }
        return stmt;
    }

    // ─── Concurrency ──────────────────────────────────────────────────────
    if (isConcurrencyStmtStart(current)) {
        StmtAST* stmt = nullptr;
        if (current == TokenType::KW_SPAWN) {
            stmt = parseSpawnStmt(stream, ctx);
        } else if (current == TokenType::KW_START) {
            stmt = parseStartStmt(stream, ctx);
        } else {  // KW_AWAIT
            stmt = parseAwaitStmt(stream, ctx);
        }
        if (stmt && !stream.match(TokenType::SEMICOLON)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ';' after concurrency statement, "
                               "got '", stream.peekValue(), "'");
            stmt->hasSyntaxError = true;
        }
        return stmt;
    }

    // ─── Expression statement ─────────────────────────────────────────────
    if (canStartStmtExpression(current)) {
        ExprStmtAST* stmt = parseExprStmt(stream, ctx);
        if (stmt && !stream.match(TokenType::SEMICOLON)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ';' after expression statement, "
                               "got '", stream.peekValue(), "'");
            stmt->hasSyntaxError = true;
        }
        return stmt;
    }

    // ─── Not a statement ──────────────────────────────────────────────────
    ctx.diag().errorAt(DiagCode::Syntax_UnexpectedToken,
                       loc,
                       "expected a statement, got '",
                       stream.peekValue(), "'");
    // Consume the offending token so the caller can make progress.
    stream.consume();
    return nullptr;
}

// =============================================================================
// parseBlock — `{ stmt* }`
// =============================================================================

BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           loc,
                           "expected '{' to open a block, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // Push a FuncBody context. The context stack is used by error
    // recovery to pick the right follow-set. For a block body, the
    // follow-set is "anything that can follow a statement inside a
    // block," which is what the parser uses by default. Pushing
    // FuncBody here is informational rather than functional in the
    // current design; if the parser ever gains context-dependent
    // recovery rules, the frame is already there.
    ctx.pushContext(SyntacticContext::FuncBody, loc);

    std::vector<StmtAST*> stmts;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray semicolons.
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        const size_t posBefore = stream.getPos();

        StmtAST* stmt = parseStmt(stream, ctx);
        if (stmt) {
            stmts.push_back(stmt);
            continue;
        }

        // parseStmt returned null. If it consumed nothing, we're stuck;
        // synchronize to the next plausible statement start. If it
        // consumed something, the next iteration handles the rest.
        if (stream.getPos() == posBefore) {
            // Ensure progress: skip the current token and synchronize.
            synchronizeTo(stream, ctx,
                          TokenType::SEMICOLON,
                          TokenType::RBRACE);
            if (stream.match(TokenType::SEMICOLON)) {
                continue;
            }
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }
    }

    // Closing `}`.
    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close block, got '",
                           stream.peekValue(), "'");
    }

    ctx.popContext();

    auto builder = ctx.arena().makeBuilder<StmtAST*>(stmts.size());
    for (StmtAST* s : stmts) builder.push_back(s);

    auto* block = ctx.arena().make<BlockStmtAST>();
    block->loc = loc;
    block->stmts = builder.build();
    return block;
}

// =============================================================================
// parseIfStmt — `if cond { ... } [else ...]`
// =============================================================================

IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_IF)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'if', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // Condition.
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a condition after 'if'");
        return nullptr;
    }

    // Then-branch. Must be a block.
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the then-branch, got '",
                           stream.peekValue(), "'");
        // Recover: produce a placeholder then-branch.
        auto* placeholder = ctx.arena().make<UnknownStmtAST>();
        placeholder->loc = stream.currentLoc();
        placeholder->hasSyntaxError = true;

        auto* ifStmt = ctx.arena().make<IfStmtAST>();
        ifStmt->loc = loc;
        ifStmt->condition = condition;
        ifStmt->thenBranch = placeholder;
        ifStmt->hasSyntaxError = true;
        return ifStmt;
    }

    StmtAST* thenBranch = parseBlock(stream, ctx);

    // Optional else-branch.
    StmtAST* elseBranch = nullptr;
    if (stream.match(TokenType::KW_ELSE)) {
        if (stream.check(TokenType::KW_IF)) {
            // `else if` — a chained if-statement.
            elseBranch = parseIfStmt(stream, ctx);
        } else if (stream.check(TokenType::LBRACE)) {
            // `else { ... }` — an else block.
            elseBranch = parseBlock(stream, ctx);
        } else {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                               stream.currentLoc(),
                               "expected 'if' or '{' after 'else', got '",
                               stream.peekValue(), "'");
            elseBranch = ctx.arena().make<UnknownStmtAST>();
            elseBranch->loc = stream.currentLoc();
            elseBranch->hasSyntaxError = true;
        }
    }

    auto* ifStmt = ctx.arena().make<IfStmtAST>();
    ifStmt->loc = loc;
    ifStmt->condition = condition;
    ifStmt->thenBranch = thenBranch;
    ifStmt->elseBranch = elseBranch;

    if ((thenBranch && thenBranch->hasSyntaxError) ||
        (elseBranch && elseBranch->hasSyntaxError)) {
        ifStmt->hasSyntaxError = true;
    }
    return ifStmt;
}

// =============================================================================
// parseSwitchStmt — `switch subject { case ...: { ... } ... }`
// =============================================================================

SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_SWITCH)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'switch', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // Subject.
    ExprAST* subject = parseExpr(stream, ctx);
    if (!subject) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a subject for 'switch'");
        return nullptr;
    }

    // Opening `{`.
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' after the switch subject, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // Push a SwitchBody context for error recovery.
    ctx.pushContext(SyntacticContext::SwitchBody, loc);

    std::vector<SwitchCaseAST*> cases;
    BlockStmtAST* defaultBody = nullptr;
    std::optional<SourceLocation> defaultLoc;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray `;` and `,`.
        if (stream.match(TokenType::SEMICOLON) ||
            stream.match(TokenType::COMMA)) {
            continue;
        }

        if (stream.check(TokenType::KW_CASE)) {
            SwitchCaseAST* c = parseSwitchCase(stream, ctx);
            if (c) cases.push_back(c);
            continue;
        }

        if (stream.check(TokenType::KW_DEFAULT)) {
            if (defaultBody != nullptr) {
                ctx.diag().errorAt(DiagCode::Syntax_MultipleDefaults,
                                   stream.currentLoc(),
                                   "duplicate 'default' clause in switch");
                // Skip the duplicate's body.
                stream.consume();   // `default`
                if (stream.match(TokenType::COLON)) {
                    if (stream.check(TokenType::LBRACE)) {
                        parseBlock(stream, ctx);   // discard
                    } else {
                        synchronizeTo(stream, ctx,
                                      TokenType::KW_CASE,
                                      TokenType::KW_DEFAULT,
                                      TokenType::RBRACE);
                    }
                }
                continue;
            }
            defaultLoc = stream.currentLoc();
            stream.consume();   // `default`

            if (!stream.match(TokenType::COLON)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                                   stream.currentLoc(),
                                   "expected ':' after 'default', got '",
                                   stream.peekValue(), "'");
                continue;
            }

            if (stream.check(TokenType::LBRACE)) {
                defaultBody = parseBlock(stream, ctx);
            } else {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                                   stream.currentLoc(),
                                   "expected '{' for the default body, "
                                   "got '", stream.peekValue(), "'");
                synchronizeTo(stream, ctx,
                              TokenType::KW_CASE,
                              TokenType::KW_DEFAULT,
                              TokenType::RBRACE);
            }
            continue;
        }

        ctx.diag().errorAt(DiagCode::Syntax_UnexpectedToken,
                           stream.currentLoc(),
                           "expected 'case' or 'default' inside switch, "
                           "got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::KW_CASE,
                      TokenType::KW_DEFAULT,
                      TokenType::RBRACE);
    }

    // Closing `}`.
    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close switch, got '",
                           stream.peekValue(), "'");
    }

    ctx.popContext();

    auto builder = ctx.arena().makeBuilder<SwitchCaseAST*>(cases.size());
    for (SwitchCaseAST* c : cases) builder.push_back(c);

    auto* switchStmt = ctx.arena().make<SwitchStmtAST>();
    switchStmt->loc = loc;
    switchStmt->subject = subject;
    switchStmt->cases = builder.build();
    switchStmt->defaultBody = defaultBody;
    switchStmt->defaultLoc = defaultLoc;
    return switchStmt;
}

// =============================================================================
// parseSwitchCase — one `case` clause
// =============================================================================

SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_CASE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'case', got '", stream.peekValue(), "'");
        return nullptr;
    }

    std::vector<CaseValueAST*> values;

    // Read one or more case values, separated by commas, until `:`.
    while (!stream.isAtEnd() && !stream.check(TokenType::COLON)) {
        CaseValueAST* cv = parseCaseValue(stream, ctx);
        if (cv) {
            values.push_back(cv);
        } else {
            // parseCaseValue reports its own error.
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::COLON);
        }

        if (stream.match(TokenType::COMMA)) {
            continue;
        }
        if (stream.check(TokenType::COLON)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ':' in case values, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::COLON);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    // `:`.
    if (!stream.match(TokenType::COLON)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after case values, got '",
                           stream.peekValue(), "'");
    }

    // Body block.
    BlockStmtAST* body = nullptr;
    if (stream.check(TokenType::LBRACE)) {
        body = parseBlock(stream, ctx);
    } else {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for the case body, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::KW_CASE,
                      TokenType::KW_DEFAULT,
                      TokenType::RBRACE);
    }

    auto valueBuilder = ctx.arena().makeBuilder<CaseValueAST*>(values.size());
    for (CaseValueAST* v : values) valueBuilder.push_back(v);

    auto* c = ctx.arena().make<SwitchCaseAST>();
    c->loc = loc;
    c->values = valueBuilder.build();
    c->body = body;
    return c;
}

// =============================================================================
// parseCaseValue — one value inside a `case` clause
// =============================================================================

/// @brief Parse one case value: a literal, an enum variant, a range, or
///        an enum variant with a payload binding.
///
/// The grammar's `case_value` is deliberately narrow. It is one of:
///
///   - a literal (`case 200`, `case 'a'`)
///   - an enum variant (`case Direction.North`)
///   - an enum variant with a payload binding (`case JsonValue.Num(n)`)
///   - a literal range (`case 1..10`)
///
/// The parser distinguishes the forms by their shape:
///
///   - An IDENTIFIER followed by `.` followed by an IDENTIFIER (and
///     optionally `(IDENTIFIER)`) is an enum variant. The first
///     identifier is the enum type name; the second is the variant.
///   - A literal followed by `..` or `..<` and another literal is a
///     range.
///   - Any other literal is a single case value.
///
/// The parser does not resolve the enum type name or the variant;
/// Sema does. The parser does not check that range bounds are literals
/// (the grammar requires it); it produces whatever it parses and lets
/// Sema validate.
CaseValueAST* parseCaseValue(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Enum variant or range or literal ─────────────────────────────────
    //
    // An IDENTIFIER followed by `.` is an enum variant access. The
    // parser reads the type name, the dot, the variant name, and an
    // optional `(binding)`.
    if (current == TokenType::IDENTIFIER) {
        // Peek: is the next token a `.`? If not, this identifier is a
        // constant reference in a case, which the grammar does not
        // currently allow, but which a future extension might. Report
        // a syntax error and treat the identifier as a literal.
        const size_t savedPos = stream.getPos();
        stream.consume();   // identifier
        const bool isVariantAccess = stream.check(TokenType::DOT);
        stream.setPos(savedPos);

        if (isVariantAccess) {
            Token enumTok = stream.consume();   // enum type name
            InternedString enumName = ctx.pool().intern(enumTok.value);

            if (!stream.match(TokenType::DOT)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                                   stream.currentLoc(),
                                   "expected '.' after enum type name in "
                                   "case value, got '", stream.peekValue(), "'");
                return nullptr;
            }

            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                   stream.currentLoc(),
                                   "expected a variant name after '.', got '",
                                   stream.peekValue(), "'");
                return nullptr;
            }
            Token variantTok = stream.consume();
            InternedString variantName = ctx.pool().intern(variantTok.value);

            // Optional payload binding: `(binding)`.
            InternedString binding;
            bool hasBinding = false;

            if (stream.match(TokenType::LPAREN)) {
                if (!stream.check(TokenType::IDENTIFIER)) {
                    ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                       stream.currentLoc(),
                                       "expected a binding name inside "
                                       "'(...)', got '",
                                       stream.peekValue(), "'");
                } else {
                    Token bindTok = stream.consume();
                    binding = ctx.pool().intern(bindTok.value);
                    hasBinding = true;
                }

                if (!stream.match(TokenType::RPAREN)) {
                    ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                                       stream.currentLoc(),
                                       "expected ')' to close payload "
                                       "binding, got '",
                                       stream.peekValue(), "'");
                }
            }

            // Produce the enum-variant access as a FieldAccessExprAST.
            // The grammar's case_value for an enum variant is
            // "IDENTIFIER '.' IDENTIFIER", which parses as a field
            // access. The parser produces that node; Sema resolves the
            // enum type name and the variant.
            auto* enumIdent = ctx.arena().make<IdentifierExprAST>(enumName);
            enumIdent->loc = loc;
            auto* fieldAccess = ctx.arena().make<FieldAccessExprAST>(variantName);
            fieldAccess->loc = loc;
            fieldAccess->object = enumIdent;

            auto* cv = ctx.arena().make<CaseValueAST>(hasBinding);
            cv->loc = loc;
            cv->value = fieldAccess;
            cv->binding = binding;
            return cv;
        }
        // Not an enum variant. Fall through to the literal case; a
        // bare identifier in case position is a syntax error, but
        // parseLiteralExpr handles identifiers as literals of kind
        // "String" and we can let Sema reject it.
    }

    // ─── Literal or range ─────────────────────────────────────────────────
    //
    // Parse the first literal (or whatever expression form the
    // grammar's narrow case_value allows). If a range operator
    // follows, produce a RangeExprAST; otherwise produce the literal.
    ExprAST* first = nullptr;

    if (isLiteral(current)) {
        // For a bare literal, produce a LiteralExprAST.
        first = parseLiteralExpr(stream, ctx);
    } else {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedCaseValue,
                           loc,
                           "expected a case value, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    if (!first) {
        return nullptr;
    }

    // Check for a range operator.
    if (stream.check(TokenType::RANGE) ||
        stream.check(TokenType::RANGE_EXCLUSIVE)) {
        const bool isExclusive = stream.check(TokenType::RANGE_EXCLUSIVE);
        stream.consume();

        if (!isLiteral(stream.peekType())) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedLiteral,
                               stream.currentLoc(),
                               "expected a literal end bound for a case "
                               "range, got '", stream.peekValue(), "'");
            return nullptr;
        }
        ExprAST* second = parseLiteralExpr(stream, ctx);
        if (!second) return nullptr;

        auto* range = ctx.arena().make<RangeExprAST>(isExclusive);
        range->loc = loc;
        range->lo = first;
        range->hi = second;

        auto* cv = ctx.arena().make<CaseValueAST>(false);
        cv->loc = loc;
        cv->value = range;
        return cv;
    }

    // A single literal case value.
    auto* cv = ctx.arena().make<CaseValueAST>(false);
    cv->loc = loc;
    cv->value = first;
    return cv;
}

// =============================================================================
// parseForStmt — `for x T in iterable { ... }`
// =============================================================================

ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_FOR)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'for', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── First binding ────────────────────────────────────────────────────
    //
    // The first binding is `x T` (name + type) or `_` (discard).
    ParamAST* indexVar = nullptr;
    bool isDiscardIndex = false;

    if (isUnderscoreIdentifier(stream)) {
        stream.consume();   // `_`
        isDiscardIndex = true;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        InternedString name = ctx.pool().intern(nameTok.value);

        TypeAST* type = parseType(stream, ctx);
        if (!type) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a type for the loop binding '",
                               ctx.pool().lookup(name), "', got '",
                               stream.peekValue(), "'");
            type = ctx.arena().make<UnknownTypeAST>();
            type->loc = stream.currentLoc();
            type->hasSyntaxError = true;
        }

        indexVar = ctx.arena().make<ParamAST>(
            name, type, /*isVariadic=*/false, /*isConstParam=*/false);
        indexVar->loc = nameTok.location;
    } else {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedForBinding,
                           stream.currentLoc(),
                           "expected a loop binding name or '_', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Second binding (collection iteration only) ───────────────────────
    //
    // If a comma follows, this is a collection iteration and a second
    // binding follows.
    ParamAST* valueVar = nullptr;
    bool isDiscardValue = false;

    if (stream.match(TokenType::COMMA)) {
        if (isUnderscoreIdentifier(stream)) {
            stream.consume();   // `_`
            isDiscardValue = true;
        } else if (stream.check(TokenType::IDENTIFIER)) {
            Token nameTok = stream.consume();
            InternedString name = ctx.pool().intern(nameTok.value);

            TypeAST* type = parseType(stream, ctx);
            if (!type) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                                   stream.currentLoc(),
                                   "expected a type for the loop binding '",
                                   ctx.pool().lookup(name), "', got '",
                                   stream.peekValue(), "'");
                type = ctx.arena().make<UnknownTypeAST>();
                type->loc = stream.currentLoc();
                type->hasSyntaxError = true;
            }

            valueVar = ctx.arena().make<ParamAST>(
                name, type, /*isVariadic=*/false, /*isConstParam=*/false);
            valueVar->loc = nameTok.location;
        } else {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedForBinding,
                               stream.currentLoc(),
                               "expected a second loop binding name or '_', "
                               "got '", stream.peekValue(), "'");
            return nullptr;
        }
    }

    // ─── `in` ─────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::KW_IN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected 'in' after the loop binding(s), got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Iterable ─────────────────────────────────────────────────────────
    ExprAST* iterable = parseExpr(stream, ctx);
    if (!iterable) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an iterable expression after 'in'");
        return nullptr;
    }

    // ─── Optional step (range loops only) ─────────────────────────────────
    //
    // A range loop's iterable may be followed by `.. step`. If the
    // iterable is not a range and a `..` follows, that's an error;
    // the parser accepts it anyway and lets Sema report.
    ExprAST* step = nullptr;
    if (stream.match(TokenType::RANGE)) {
        step = parseExpr(stream, ctx);
        if (!step) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                               stream.currentLoc(),
                               "expected a step expression after '..'");
        }
    }

    // ─── Body ─────────────────────────────────────────────────────────────
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        body = ctx.arena().make<UnknownStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;
    }

    auto* forStmt = ctx.arena().make<ForStmtAST>();
    forStmt->loc = loc;
    forStmt->indexVar = indexVar;
    forStmt->valueVar = valueVar;
    forStmt->iterable = iterable;
    forStmt->step = step;
    forStmt->body = body;

    // Mark syntax errors: a discarded binding is not an error, but a
    // binding with a syntax error is.
    if ((indexVar && indexVar->hasSyntaxError) ||
        (valueVar && valueVar->hasSyntaxError) ||
        (iterable && iterable->hasSyntaxError) ||
        (step && step->hasSyntaxError) ||
        (body && body->hasSyntaxError)) {
        forStmt->hasSyntaxError = true;
    }
    // Discarded bindings are noted by setting the corresponding pointer
    // to nullptr; the ForStmtAST's docs cover that representation.
    (void)isDiscardIndex;
    (void)isDiscardValue;
    return forStmt;
}

// =============================================================================
// parseWhileStmt — `while cond { ... }`
// =============================================================================

WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_WHILE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'while', got '", stream.peekValue(), "'");
        return nullptr;
    }

    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a condition after 'while'");
        return nullptr;
    }

    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        body = ctx.arena().make<UnknownStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;
    }

    auto* whileStmt = ctx.arena().make<WhileStmtAST>();
    whileStmt->loc = loc;
    whileStmt->condition = condition;
    whileStmt->body = body;
    if (body->hasSyntaxError) whileStmt->hasSyntaxError = true;
    return whileStmt;
}

// =============================================================================
// parseDoWhileStmt — `do { ... } while cond`
// =============================================================================

DoWhileStmtAST* parseDoWhileStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_DO)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'do', got '", stream.peekValue(), "'");
        return nullptr;
    }

    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        body = ctx.arena().make<UnknownStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;
    }

    if (!stream.match(TokenType::KW_WHILE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected 'while' after the do-block, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a condition after 'while'");
        condition = ctx.arena().make<UnknownExprAST>();
        condition->loc = stream.currentLoc();
        condition->hasSyntaxError = true;
    }

    auto* doWhile = ctx.arena().make<DoWhileStmtAST>();
    doWhile->loc = loc;
    doWhile->body = body;
    doWhile->condition = condition;
    if (body->hasSyntaxError || condition->hasSyntaxError) {
        doWhile->hasSyntaxError = true;
    }
    return doWhile;
}

// =============================================================================
// parseReturnStmt — `return [expr]`
// =============================================================================

ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_RETURN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'return', got '", stream.peekValue(), "'");
        return nullptr;
    }

    auto* ret = ctx.arena().make<ReturnStmtAST>();
    ret->loc = loc;

    // A bare `return;` is legal; the value is null. A `return` followed
    // by anything that can start an expression is a value-returning
    // return.
    if (stream.check(TokenType::SEMICOLON) ||
        stream.check(TokenType::RBRACE) ||
        stream.isAtEnd()) {
        return ret;   // no value
    }

    ExprAST* value = parseExpr(stream, ctx);
    if (!value) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a return value expression after "
                           "'return', got '", stream.peekValue(), "'");
        ret->hasSyntaxError = true;
        return ret;
    }
    ret->value = value;
    if (value->hasSyntaxError) ret->hasSyntaxError = true;
    return ret;
}

// =============================================================================
// parseBreakStmt — `break`
// =============================================================================

BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_BREAK)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'break', got '", stream.peekValue(), "'");
        return nullptr;
    }

    auto* b = ctx.arena().make<BreakStmtAST>();
    b->loc = loc;
    return b;
}

// =============================================================================
// parseContinueStmt — `continue`
// =============================================================================

ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_CONTINUE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'continue', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    auto* c = ctx.arena().make<ContinueStmtAST>();
    c->loc = loc;
    return c;
}

// =============================================================================
// parseExprStmt — an expression used as a statement
// =============================================================================

ExprStmtAST* parseExprStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    ExprAST* expr = parseExpr(stream, ctx);
    if (!expr) {
        // parseExpr already reported. Produce a placeholder so the
        // caller can continue.
        auto* placeholder = ctx.arena().make<UnknownExprAST>();
        placeholder->loc = loc;
        placeholder->hasSyntaxError = true;

        auto* stmt = ctx.arena().make<ExprStmtAST>(placeholder);
        stmt->loc = loc;
        stmt->hasSyntaxError = true;
        return stmt;
    }

    auto* stmt = ctx.arena().make<ExprStmtAST>(expr);
    stmt->loc = loc;
    if (expr->hasSyntaxError) stmt->hasSyntaxError = true;
    return stmt;
}

// =============================================================================
// parseDeclStmt — a local declaration inside a block
// =============================================================================

/// @brief Parse a local declaration inside a block.
///
/// Local declarations are the same forms as top-level declarations,
/// except `import` (top-level only). The parser delegates to
/// `parseDecl` in ParseDecl.cpp and wraps the result in a `DeclStmtAST`.
///
/// The `parseDecl` function does not know whether it is being called at
/// top level or inside a block. The parser's dispatch to `parseDecl`
/// does not depend on context; the wrapping in `DeclStmtAST` (this
/// function) or the direct append (the top-level parser) is what
/// distinguishes the two.
DeclStmtAST* parseDeclStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    DeclAST* decl = parseDecl(stream, ctx);
    if (!decl) {
        return nullptr;
    }

    auto* stmt = ctx.arena().make<DeclStmtAST>(decl);
    stmt->loc = loc;
    if (decl->hasSyntaxError) stmt->hasSyntaxError = true;
    return stmt;
}

} // namespace lucid::parser