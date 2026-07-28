/**
 * @file Concurrency.cpp
 * 
 * @responsibility Implements concurrency statement parsing for the Lucid
 *                 language: `async`/`await` (event-loop concurrency) and
 *                 `spawn`/`join` (OS-thread parallelism).
 * 
 * @related_files
 *   - src/parser/Parser.hpp – function declarations
 *   - src/parser/ParseStmt.cpp – statement dispatch (parseStmt). `await`
 *     and `join` are modeled after the sibling multi-assign/multi-var-decl
 *     statements (comma-list target, shared trailing semantics); `async`
 *     and `spawn` each bind exactly one target and do not use that shape.
 *   - src/parser/support/ParserContext.hpp – SyntacticContext, used here to
 *     confirm 'await' appears inside a function body
 */

#include "core/Tokens.hpp"
#include "parser/Parser.hpp"
#include "parser/support/TokenStream.hpp"
#include "parser/support/ParserContext.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugMacros.hpp"

namespace parser {

// =============================================================================
// parseAsyncStmt – Parses an async statement (event-loop concurrency)
// =============================================================================

AsyncStmtAST* parseAsyncStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAsyncStmt: parsing async statement");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Expect ASYNC keyword ──────────────────────────────────────────────
    if (!stream.match(TokenType::ASYNC)) {
        ctx.error(stream, DiagCode::E1001, "async", stream.peekValue());
        return ctx.arena.make<AsyncStmtAST>();
    }

    AsyncStmtAST* asyncStmt = ctx.arena.make<AsyncStmtAST>();
    asyncStmt->loc = loc;

    // ─── 2. Parse the single target: IDENTIFIER ─────────────────────────────
    // Grammar: async_stmt := 'async' IDENTIFIER '=' call_expr
    // The target is a plain, already-`let`-declared variable name — not a
    // full lvalue; field access and indexing are not part of this grammar.
    // Unlike await/join, async binds exactly one variable per statement —
    // no comma-separated list here.
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "variable name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }

    SourceLocation targetLoc = stream.currentLoc();
    Token nameTok = stream.advance();

    auto* target = ctx.arena.make<IdentifierExprAST>();
    target->loc = targetLoc;
    target->name = ctx.pool.intern(nameTok.value);
    asyncStmt->target = target;

    // ─── 3. Expect ASSIGN ────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1007, "=", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }

    // ─── 4. Parse the async call expression ─────────────────────────────────
    ExprPtr call = parseExpr(stream, ctx);
    if (!call) {
        ctx.error(stream, DiagCode::E1006, "async call expression", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return asyncStmt;
    }
    asyncStmt->call = call;

    LOG_PARSER("parseAsyncStmt: parsed async statement for target '", nameTok.value, "'");
    return asyncStmt;
}

// =============================================================================
// parseAwaitStmt – Parses an await statement (waits for async operations)
// =============================================================================

AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAwaitStmt: parsing await statement");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Expect AWAIT keyword ──────────────────────────────────────────────
    if (!stream.match(TokenType::AWAIT)) {
        ctx.error(stream, DiagCode::E1001, "await", stream.peekValue());
        return ctx.arena.make<AwaitStmtAST>();
    }

    AwaitStmtAST* awaitStmt = ctx.arena.make<AwaitStmtAST>();
    awaitStmt->loc = loc;

    // ─── 2. Context validation ───────────────────────────────────────────────
    // 'await' is only valid inside a function body (not at file top level).
    // This is a purely syntactic check — whether it actually pairs with a
    // preceding 'async' on the same variable(s) needs a symbol table and
    // is left to semantic analysis.
    if (!ctx.isInsideContext(SyntacticContext::FuncBody)) {
        ctx.error(stream, DiagCode::E1010, "awaiting",
                  "the keyword 'await' is only valid inside a function body");
    }

    auto targetBuilder = ctx.arena.makeBuilder<ExprPtr>();

    // ─── 3. Parse target list: IDENTIFIER { ',' IDENTIFIER } ────────────────
    // Grammar: await_stmt := 'await' IDENTIFIER { ',' IDENTIFIER }
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "variable name", stream.peekValue());
            synchronizeToContext(stream, ctx);
            break;
        }

        SourceLocation targetLoc = stream.currentLoc();
        Token nameTok = stream.advance();

        auto* target = ctx.arena.make<IdentifierExprAST>();
        target->loc = targetLoc;
        target->name = ctx.pool.intern(nameTok.value);

        targetBuilder.push_back(target);
    } while (stream.match(TokenType::COMMA));
    awaitStmt->targets = targetBuilder.build();

    LOG_PARSER("parseAwaitStmt: parsed await statement with ", awaitStmt->targets.size(), " target(s)");
    return awaitStmt;
}

// =============================================================================
// parseSpawnStmt – Parses a spawn statement (OS-thread parallelism)
// =============================================================================

SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseSpawnStmt: parsing spawn statement");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Expect SPAWN keyword ───────────────────────────────────────────────
    if (!stream.match(TokenType::SPAWN)) {
        ctx.error(stream, DiagCode::E1001, "spawn", stream.peekValue());
        return ctx.arena.make<SpawnStmtAST>();
    }

    SpawnStmtAST* spawnStmt = ctx.arena.make<SpawnStmtAST>();
    spawnStmt->loc = loc;

    // ─── 2. Parse the single binding: spawn_binding ─────────────────────────
    // Grammar: spawn_stmt    := 'spawn' spawn_binding '=' call_expr
    //          spawn_binding := IDENTIFIER | '_'
    // '_' discards the return value (fire-and-forget, no join required) and
    // is represented here as a null target, the same discard convention
    // parseForStmt uses for its index/value bindings. Unlike await/join,
    // spawn binds exactly one value per statement — no comma-separated list.
    if (stream.check(TokenType::UNDERSCORE)) {
        stream.advance(); // Consume '_'
        spawnStmt->target = nullptr;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        SourceLocation targetLoc = stream.currentLoc();
        Token nameTok = stream.advance();

        auto* target = ctx.arena.make<IdentifierExprAST>();
        target->loc = targetLoc;
        target->name = ctx.pool.intern(nameTok.value);

        spawnStmt->target = target;
    } else {
        ctx.error(stream, DiagCode::E1002, "variable name or '_'", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }

    // ─── 3. Expect ASSIGN ────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1007, "=", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }

    // ─── 4. Parse the spawn call expression ─────────────────────────────────
    ExprPtr call = parseExpr(stream, ctx);
    if (!call) {
        ctx.error(stream, DiagCode::E1006, "spawn call expression", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return spawnStmt;
    }
    spawnStmt->call = call;

    LOG_PARSER("parseSpawnStmt: parsed spawn statement (target ",
               spawnStmt->target ? "bound" : "discarded", ")");
    return spawnStmt;
}

// =============================================================================
// parseJoinStmt – Parses a join statement (waits for spawned threads)
// =============================================================================

JoinStmtAST* parseJoinStmt(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseJoinStmt: parsing join statement");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Expect JOIN keyword ────────────────────────────────────────────────
    if (!stream.match(TokenType::JOIN)) {
        ctx.error(stream, DiagCode::E1001, "join", stream.peekValue());
        return ctx.arena.make<JoinStmtAST>();
    }

    JoinStmtAST* joinStmt = ctx.arena.make<JoinStmtAST>();
    joinStmt->loc = loc;

    auto targetBuilder = ctx.arena.makeBuilder<ExprPtr>();

    // ─── 2. Parse target list: IDENTIFIER { ',' IDENTIFIER } ────────────────
    // Grammar: join_stmt := 'join' IDENTIFIER { ',' IDENTIFIER }
    do {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.error(stream, DiagCode::E1002, "variable name", stream.peekValue());
            synchronizeToContext(stream, ctx);
            break;
        }

        SourceLocation targetLoc = stream.currentLoc();
        Token nameTok = stream.advance();

        auto* target = ctx.arena.make<IdentifierExprAST>();
        target->loc = targetLoc;
        target->name = ctx.pool.intern(nameTok.value);

        targetBuilder.push_back(target);
    } while (stream.match(TokenType::COMMA));
    joinStmt->targets = targetBuilder.build();

    LOG_PARSER("parseJoinStmt: parsed join statement with ", joinStmt->targets.size(), " target(s)");
    return joinStmt;
}

} // namespace parser