/**
 * @file ParseStmtConcurrency.cpp
 * @brief The concurrency statement parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseSpawnStmt    `spawn f(args);`
 *   - parseStartStmt    `start [let|const] d T = f(args);`
 *   - parseAwaitStmt    `await d;`, `await a, b, c;`,
 *                       `await all(a, b, c);`, `await any(a, b, c);`
 *
 * ─── Design: three forms, one per async-call shape ────────────────────────
 * An `async` function `f` may be called in exactly three ways:
 *
 *   - `spawn f(args);`          fire-and-forget; the result is discarded.
 *   - `start d T = f(args);`    a held handle of type `Deferred<T>`.
 *   - `await d;`                consume a handle; narrows `d` to `T`.
 *
 * The parser produces a distinct `StmtAST` subclass for each:
 * `SpawnStmtAST`, `StartStmtAST`, `AwaitStmtAST`. The three classes
 * share no structure beyond being statements.
 *
 * ─── Design: the parser does not check async-ness ─────────────────────────
 * Whether `f` is actually marked `async` is a Sema concern, not a parser
 * concern. The parser produces a `SpawnStmtAST` for any `spawn f(args);`
 * and lets Sema reject a non-async callee with the appropriate
 * diagnostic.
 *
 * Similarly, whether `d` in `await d;` is a `Deferred<T>` binding is a
 * Sema concern. The parser produces an `AwaitStmtAST` whose `targets` are
 * `IdentifierExprAST`s; Sema resolves them and checks the linear-value
 * rules.
 *
 * ─── Design: `start`'s binding is a VarDeclAST ────────────────────────────
 * `start d T = f(args);` introduces a fresh binding `d` of type
 * `Deferred<T>`. The parser produces a `StartStmtAST` whose `binding` is a
 * `VarDeclAST` — the same node type used for `let`/`const` bindings.
 * Its declared type is `NamedTypeAST("Deferred")` with a single generic
 * argument, the inner type `T`.
 *
 * The `Deferred` name is not special to the parser; it is an ordinary
 * `NamedTypeAST` that Sema resolves to the core script's
 * `TYPE Deferred<T> = #host(LucidDeferred)` declaration. The parser just
 * builds the shape.
 *
 * ─── Design: `await all` and `await any` ──────────────────────────────────
 * `await all(a, b, c)` and `await any(a, b, c)` are sugar for
 * "await every target" and "await the first to complete." The parser
 * produces an `AwaitStmtAST` with the corresponding `AwaitKind` and the
 * same target list; the distinction is semantic, not syntactic.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::diag;

#include <vector>

namespace lucid::parser {

// =============================================================================
// parseSpawnStmt — `spawn f(args);`
// =============================================================================

/// @brief Parse a `spawn` statement.
///
/// The form:
///
///   `spawn call_expr;`
///
/// `spawn` runs its call on a new fiber and discards the result. There
/// is no binding form: `spawn d T = f(args);` is not legal. If the source
/// wrote a binding, the parser reports the error at the `=` and recovers
/// by treating the `=` as the start of an expression statement — actually
/// no, the parser reports the error and skips to the `;`.
///
/// The call is parsed by `parseCallExpr`, which is the same function
/// used for a normal call. The parser does not distinguish an `async`
/// call from a synchronous call at parse time; Sema checks whether the
/// callee is `async`.
SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_SPAWN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'spawn', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Callee ───────────────────────────────────────────────────────────
    //
    // The call is `f(args)`. The callee may be an identifier, a module
    // access (`math::sqrt`), a field access (`obj.callback`), or anything
    // else that produces a function value. The parser reads the callee
    // as a prefix expression, then dispatches the call form.
    //
    // In practice, `spawn f(args)` has a leading identifier for `f`.
    // The parser's general expression parse would consume too much: it
    // would see the `(` after `f` and produce a call, which is what we
    // want — but it would also consume a following `+`, `-`, etc. if the
    // source wrote `spawn f(args) + 1;`, which is not legal (the call is
    // the whole statement).
    //
    // We parse the callee as a prefix expression, then explicitly require
    // a call to follow. If the source wrote something else, we report the
    // error.

    ExprAST* callee = parsePrefixExpr(stream, ctx);
    if (!callee) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a call expression after 'spawn'");
        return nullptr;
    }

    // ─── `(` ──────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for the spawn call, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // `parseCallExpr` consumes the argument list and the optional `!`.
    // The `!` form is not meaningful for spawn; if the source wrote one,
    // the parser accepts it and Sema rejects.
    ExprAST* call = parseCallExpr(stream, ctx, callee);
    if (!call) {
        return nullptr;
    }

    // ─── No binding form ──────────────────────────────────────────────────
    //
    // A `spawn` statement does not bind its result. If the source wrote
    // `spawn d T = f(args);`, the `d` after `spawn` was parsed as the
    // callee (an identifier expression), and the following `T` is a
    // stray token that the parser reports when it expects `(`. That
    // diagnostic ("expected '('") is not ideal for this case; a better
    // message would be "spawn does not bind a value; use `start d T =
    // ...` to hold the result." A future refinement could peek for
    // `IDENTIFIER IDENTIFIER =` and produce that message. For now, the
    // parser produces the generic call-shape error.

    auto* spawn = ctx.arena().make<SpawnStmtAST>();
    spawn->loc = loc;
    spawn->call = call;
    if (call->hasSyntaxError) spawn->hasSyntaxError = true;
    return spawn;
}

// =============================================================================
// parseStartStmt — `start [let|const] d T = f(args);`
// =============================================================================

/// @brief Parse a `start` statement.
///
/// The forms:
///
///   `start d T = f(args);`        — mutable binding (default)
///   `start let d T = f(args);`    — mutable binding (explicit)
///   `start const d T = f(args);`  — immutable binding
///
/// `start` runs `f(args)` on a new fiber and binds a `Deferred<T>` handle
/// to `d`. The binding is always fresh; there is no form that assigns to
/// an existing variable.
///
/// The declared type of the binding is `Deferred<T>`, where `T` is the
/// inner type written in the source. The parser builds this as a
/// `NamedTypeAST("Deferred")` with a single generic argument.
///
/// The `T` is required. `start d = f(args);` (no type) is a syntax error;
/// the type cannot be inferred.
StartStmtAST* parseStartStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_START)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'start', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Optional `let`/`const` ───────────────────────────────────────────
    //
    // The keyword is optional; if omitted, the binding is `let`.
    DeclKeyword keyword = DeclKeyword::Let;
    if (stream.match(TokenType::KW_CONST)) {
        keyword = DeclKeyword::Const;
    } else if (stream.match(TokenType::KW_LET)) {
        keyword = DeclKeyword::Let;
    }

    // ─── Binding name ─────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a variable name after 'start', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Inner type `T` ───────────────────────────────────────────────────
    //
    // The written type is the *inner* type of the Deferred. The
    // binding's actual type is `Deferred<T>`, which the parser builds
    // below.
    TypeAST* innerType = parseType(stream, ctx);
    if (!innerType) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type for the started binding '",
                           ctx.pool().lookup(name), "', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── `=` ──────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' after the started binding, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Call expression ──────────────────────────────────────────────────
    //
    // The RHS is a call to an async function. The parser reads it as a
    // prefix expression followed by a call. Anything else is a syntax
    // error; `start d T = 5;` is not meaningful.
    ExprAST* callee = parsePrefixExpr(stream, ctx);
    if (!callee) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an async call after 'start d T ='");
        return nullptr;
    }

    if (!stream.check(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for the start call, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    ExprAST* call = parseCallExpr(stream, ctx, callee);
    if (!call) {
        return nullptr;
    }

    // ─── Build the `Deferred<T>` type ─────────────────────────────────────
    //
    // The binding's declared type is `Deferred<T>`. The parser builds it
    // as a NamedTypeAST with a generic argument. The name "Deferred" is
    // an ordinary identifier from the parser's point of view; Sema
    // resolves it to the core-script declaration.
    InternedString deferredName = ctx.pool().intern("Deferred");

    auto* deferredType = ctx.arena().make<NamedTypeAST>(deferredName);
    deferredType->loc = loc;
    auto argBuilder = ctx.arena().makeBuilder<TypeAST*>(1);
    argBuilder.push_back(innerType);
    deferredType->genericArgs = argBuilder.build();

    // ─── Build the VarDeclAST ─────────────────────────────────────────────
    //
    // The binding is a `VarDeclAST` — the same node used for `let`/`const`
    // at the top level and in local declarations. Its `init` is the call
    // expression; its declared type is `Deferred<T>`.
    //
    // The VarDeclAST is *not* added to the enclosing block's statement
    // list. It is stored inside the StartStmtAST, and Sema treats the
    // StartStmtAST as introducing the binding into the current scope.
    auto* binding = ctx.arena().make<VarDeclAST>(
        name, keyword, deferredType, call);
    binding->loc = loc;
    if (innerType->hasSyntaxError || call->hasSyntaxError) {
        binding->hasSyntaxError = true;
    }

    auto* start = ctx.arena().make<StartStmtAST>();
    start->loc = loc;
    start->binding = binding;
    start->call = call;
    if (binding->hasSyntaxError) start->hasSyntaxError = true;
    return start;
}

// =============================================================================
// parseAwaitStmt — `await d;`, `await a, b;`, `await all(...);`, `await any(...);`
// =============================================================================

/// @brief Parse an `await` statement.
///
/// Three syntactic forms:
///
///   `await d;`               — await one deferred; `d` narrows to `T`.
///   `await a, b, c;`         — await each in sequence; all narrow.
///   `await all(a, b, c);`    — await the group; all must resolve.
///   `await any(a, b, c);`    — await the group; first to resolve wins.
///
/// The `all` and `any` forms are the special ones: the parser recognizes
/// the keyword after `await` and produces an `AwaitStmtAST` with the
/// matching `AwaitKind`. Otherwise, the form is `Single`, and the target
/// list is comma-separated identifiers.
///
/// ─── Targets ──────────────────────────────────────────────────────────────
/// Each target is an identifier. `await a.b;` is not legal; the deferred
/// must be a binding, not a field access. The parser reads each target
/// as an `IdentifierExprAST` and lets Sema check that the binding is a
/// live `Deferred<T>`.
///
/// ─── `all` and `any` ──────────────────────────────────────────────────────
/// The keywords `all` and `any` are only valid immediately after `await`.
/// Elsewhere they are ordinary identifiers. The parser checks the
/// identifier's value to decide the form; a future grammar change could
/// promote them to dedicated tokens.
AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_AWAIT)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected 'await', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Special forms: `all` and `any` ───────────────────────────────────
    //
    // These are checked by identifier value, not by token type, because
    // `all` and `any` are not boot-set keywords. In other positions they
    // are ordinary identifiers.
    AwaitKind kind = AwaitKind::Single;

    if (stream.check(TokenType::IDENTIFIER)) {
        const std::string& v = stream.peek().value;
        if (v == "all") {
            kind = AwaitKind::All;
            stream.consume();
        } else if (v == "any") {
            kind = AwaitKind::Any;
            stream.consume();
        }
    }

    std::vector<ExprAST*> targets;

    if (kind == AwaitKind::All || kind == AwaitKind::Any) {
        // ─── Group form: `await all(a, b, c);` ────────────────────────────
        if (!stream.match(TokenType::LPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected '(' after 'await ",
                               (kind == AwaitKind::All) ? "all" : "any",
                               "', got '", stream.peekValue(), "'");
            return nullptr;
        }

        // Empty group: `await all();` — a syntax error.
        if (stream.match(TokenType::RPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected at least one deferred name inside "
                               "'await ",
                               (kind == AwaitKind::All) ? "all" : "any",
                               "(...)'");
            auto* empty = ctx.arena().make<AwaitStmtAST>();
            empty->loc = loc;
            empty->kind = kind;
            empty->hasSyntaxError = true;
            return empty;
        }

        // Parse comma-separated identifiers.
        while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
               ctx.canContinue()) {
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                   stream.currentLoc(),
                                   "expected a deferred name, got '",
                                   stream.peekValue(), "'");
                synchronizeTo(stream, ctx,
                              TokenType::COMMA,
                              TokenType::RPAREN);
                if (stream.match(TokenType::COMMA)) continue;
                break;
            }
            Token targetTok = stream.consume();
            auto* target = ctx.arena().make<IdentifierExprAST>(
                ctx.pool().intern(targetTok.value));
            target->loc = targetTok.location;
            targets.push_back(target);

            if (stream.match(TokenType::COMMA)) {
                continue;
            }
            if (stream.check(TokenType::RPAREN)) {
                break;
            }

            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ',' or ')' in 'await ",
                               (kind == AwaitKind::All) ? "all" : "any",
                               "(...)', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::RPAREN);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }

        if (!stream.match(TokenType::RPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ')' to close 'await ",
                               (kind == AwaitKind::All) ? "all" : "any",
                               "(...)', got '", stream.peekValue(), "'");
        }
    } else {
        // ─── Simple form: `await d;` or `await a, b, c;` ──────────────────
        while (!stream.isAtEnd() && !stream.check(TokenType::SEMICOLON) &&
               ctx.canContinue()) {
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                   stream.currentLoc(),
                                   "expected a deferred name after 'await', "
                                   "got '", stream.peekValue(), "'");
                synchronizeTo(stream, ctx,
                              TokenType::COMMA,
                              TokenType::SEMICOLON);
                if (stream.match(TokenType::COMMA)) continue;
                break;
            }
            Token targetTok = stream.consume();
            auto* target = ctx.arena().make<IdentifierExprAST>(
                ctx.pool().intern(targetTok.value));
            target->loc = targetTok.location;
            targets.push_back(target);

            if (stream.match(TokenType::COMMA)) {
                continue;
            }
            if (stream.check(TokenType::SEMICOLON)) {
                break;
            }

            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ',' or ';' after the deferred name, "
                               "got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::SEMICOLON);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }
    }

    // ─── Empty target list ────────────────────────────────────────────────
    if (targets.empty()) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected at least one deferred name after "
                           "'await'");
    }

    auto builder = ctx.arena().makeBuilder<ExprAST*>(targets.size());
    for (ExprAST* t : targets) builder.push_back(t);

    auto* await = ctx.arena().make<AwaitStmtAST>();
    await->loc = loc;
    await->kind = kind;
    await->targets = builder.build();
    if (targets.empty()) await->hasSyntaxError = true;
    return await;
}

} // namespace lucid::parser