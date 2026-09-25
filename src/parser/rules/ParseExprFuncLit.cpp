/**
 * @file ParseExprFuncLit.cpp
 * @brief Function literals, calls, access forms, and pipelines.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseAnonFuncExpr      `fn (...) -> ... { ... }` and bare-group
 *                            forgotten-marker forms
 *   - parseCallExpr          `f(args)` and `f(args)!`
 *   - parseFieldAccessExpr   `a.b`
 *   - parseModuleAccessExpr  `a::b`
 *   - parsePipelineExpr      `seed |> step |> step`
 *   - parsePipelineStep      one step inside a pipeline
 *
 * ─── Desugaring in this file ──────────────────────────────────────────────
 * `parseAnonFuncExpr` produces an `AnonFuncExprAST` — the only node that
 * holds a function body. For a curried literal, it produces a chain of
 * AnonFuncExprASTs, one per stage, with a ReturnStmt wrapping each inner
 * stage as the body of the next outer one.
 *
 * The chain-building mirrors the same pattern as `parseFuncDecl` in
 * ParseDecl.cpp. Both functions construct chains of AnonFuncExprASTs; the
 * only difference is where the signature comes from (the literal's own
 * written type vs. the declaration's declared type). A shared helper for
 * the chain construction is a future refinement; for now, two call sites
 * is the threshold below which inlining is clearer.
 *
 * ─── Design: the bare-group recovery in parseAnonFuncExpr ─────────────────
 * A function literal begins with `fn`. If the source omitted the marker
 * — `(a int) -> int { ... }` — the literal is malformed but its shape is
 * still recognizable: a parameter group, optional arrows, and a body.
 * `looksLikeAnonFunc` accepts this shape, the dispatcher routes to
 * `parseAnonFuncExpr`, and this function reports "expected 'fn' before
 * parameter group" at the exact position of the missing marker. The
 * resulting AST is a valid AnonFuncExprAST; the error is a warning-level
 * diagnostic that does not prevent further parsing.
 *
 * ─── Design: argument pack `!` is a pipeline-only form ────────────────────
 * `f(args)!` is the argument-pack form, valid only inside a pipeline step.
 * `parseCallExpr` accepts the `!` at any call and records it as
 * `hasArgPack`. Sema rejects a `!`-marked call that is not a pipeline
 * step. The parser does not distinguish pipeline context from call
 * context; the distinction is semantic.
 *
 * ─── Design: `.` and `::` are separate postfix operators ──────────────────
 * `.` is field access, resolved by Sema to a struct field or enum variant.
 * `::` is module access or static member access, resolved by Sema to a
 * module member or a static struct member. The two are syntactically
 * distinct and the parser dispatches on the operator token. Sema
 * disambiguates further (module vs. struct) by resolving the left-hand
 * side's name.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseAnonFuncExpr — function literals
// =============================================================================

/// @brief Parse a function literal.
///
/// The form:
///
///   func_literal := 'fn' '(' params ')' [ '->' return_type ] '{' body '}'
///
/// The literal's leading group is always preceded by `fn`. The return
/// type after `->` may itself be a function type (a curried literal):
///
///   fn (n int) -> fn (int) -> int { return fn (b int) -> int { ... }; }
///
/// The parser:
///
///   1. Reads the leading `fn` marker and the leading parameter group.
///   2. Reads an optional `->` and the return type. The return type is
///      parsed by `parseType`; if it is itself a function type, it
///      produces a nested `FuncTypeAST`.
///   3. Reads the body block.
///   4. Builds an `AnonFuncExprAST` whose `funcType` is the parsed
///      signature (a chain if curried) and whose `body` is the block.
///      For a curried literal, the chain is built right-to-left: the
///      innermost stage's body is the user's block; each outer stage's
///      body is a `ReturnStmt` wrapping the inner stage's
///      `AnonFuncExprAST`.
///
/// ─── Bare-group recovery ──────────────────────────────────────────────────
/// If the source omits the leading `fn` — `(a int) -> int { ... }` —
/// `parseAnonFuncExpr` reports "expected 'fn' before parameter group" at
/// the position where the marker should have been, then proceeds as if
/// the marker were present. The resulting AST is valid; the diagnostic
/// is the recovery's notice to the user.
///
/// This recovery is why `looksLikeAnonFunc` accepts the bare-group form
/// as a shape: the dispatcher routes to this function, and the function
/// reports the error at the exact position of the missing marker,
/// instead of the error appearing as an "expected expression" at the
/// start of the group.
///
/// ─── No generic params on a literal ───────────────────────────────────────
/// A function literal has no syntax for generic parameters. Its signature
/// is a single concrete `FuncTypeAST` chain (possibly curried). If the
/// enclosing context requires a generic function, the source must use a
/// declaration (`const identity<T> ...`), not a literal.
AnonFuncExprAST* parseAnonFuncExpr(TokenStream& stream,
                                   ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Leading `fn` marker ──────────────────────────────────────────────
    //
    // If the marker is present, consume it. If not, report the
    // missing-marker error and continue. The literal is still parsed;
    // the recovery produces a valid AST so the surrounding expression
    // is not lost.
    if (stream.check(TokenType::KW_FN_MARKER)) {
        stream.consume();
    } else if (stream.check(TokenType::LPAREN)) {
        // Bare-group form. The `fn` marker is missing.
        ctx.diag().errorAt(DiagCode::Syntax_MissingFuncShapeMarker,
                           loc,
                           "expected 'fn' before parameter group; a "
                           "function literal begins with 'fn'");
    } else {
        // Neither `fn` nor `(`. The caller shouldn't have dispatched
        // here; report and bail.
        ctx.diag().errorAt(DiagCode::Syntax_MissingFuncShapeMarker,
                           loc,
                           "expected 'fn' or '(' to begin a function "
                           "literal, got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Leading parameter group ──────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for function literal parameters, "
                           "got '", stream.peekValue(), "'");
        return nullptr;
    }

    std::vector<ParamAST*> leadingGroup = parseParamList(stream, ctx,
                                                         /*allowNames=*/true);

    // ─── Optional `->` return type ────────────────────────────────────────
    //
    // If the return type is another function type (a curried literal),
    // `parseType` produces a `FuncTypeAST` for the nested stage. The
    // caller does not need to distinguish; the type's own structure
    // tells the chain builder what to do.
    TypeAST* returnType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        returnType = parseType(stream, ctx);
        if (!returnType) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a return type after '->', got '",
                               stream.peekValue(), "'");
            returnType = ctx.arena().make<UnknownTypeAST>();
            returnType->loc = stream.currentLoc();
            returnType->hasSyntaxError = true;
        }
    }

    // ─── Body block ───────────────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for function literal body, got '",
                           stream.peekValue(), "'");
        auto* empty = ctx.arena().make<BlockStmtAST>();
        empty->loc = stream.currentLoc();
        empty->hasSyntaxError = true;

        // Build the funcType chain anyway.
        auto* ft = makeFuncType(ctx, std::move(leadingGroup), returnType);
        ft->loc = loc;
        auto* anon = ctx.arena().make<AnonFuncExprAST>(ft, empty);
        anon->loc = loc;
        anon->hasSyntaxError = true;
        return anon;
    }

    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected a block body for function literal");
        body = ctx.arena().make<UnknownStmtAST>();
        body->loc = stream.currentLoc();
        body->hasSyntaxError = true;
    }

    // ─── Build the signature chain ────────────────────────────────────────
    //
    // Collect the stages of the leading group and any curried return-type
    // stages. The leading group is one stage; the return type may
    // contain further `FuncTypeAST` stages.
    //
    // The stage list is built by walking the return type's chain of
    // FuncTypeASTs. Every FuncTypeAST in that chain is a stage; the
    // final non-function return type is the outermost return value.

    std::vector<FuncTypeAST*> stages;

    // Stage 0: the leading group.
    auto* leadingFT = makeFuncType(ctx, std::move(leadingGroup), nullptr);
    leadingFT->loc = loc;
    stages.push_back(leadingFT);

    // Walk the return type's FuncTypeAST chain.
    TypeAST* cursor = returnType;
    while (cursor && cursor->isa<FuncTypeAST>()) {
        stages.push_back(cursor->as<FuncTypeAST>());
        cursor = cursor->as<FuncTypeAST>()->returnType;
    }
    TypeAST* finalReturnType = cursor;   // may be nullptr for a void return

    // Link the stages: each stage's returnType is the next stage, or the
    // final return type for the innermost.
    for (size_t i = 0; i + 1 < stages.size(); ++i) {
        stages[i]->returnType = stages[i + 1];
    }
    stages.back()->returnType = finalReturnType;

    // ─── Build the AnonFuncExprAST chain ──────────────────────────────────
    //
    // The innermost stage gets the user's block. Each outer stage's
    // body is a ReturnStmt wrapping the inner AnonFuncExprAST.
    StmtAST* bodyCursor = body;
    AnonFuncExprAST* anon = nullptr;
    for (int i = static_cast<int>(stages.size()) - 1; i >= 0; --i) {
        anon = ctx.arena().make<AnonFuncExprAST>(stages[i], bodyCursor);
        anon->loc = loc;

        if (i == 0) break;

        // Wrap the inner anon in a ReturnStmt for the next-outer stage.
        auto* ret = ctx.arena().make<ReturnStmtAST>();
        ret->loc = loc;
        ret->value = anon;
        bodyCursor = ret;
    }

    if (body->hasSyntaxError ||
        (returnType && returnType->hasSyntaxError)) {
        anon->hasSyntaxError = true;
    }
    return anon;
}

// =============================================================================
// parseCallExpr — `f(args)` and `f(args)!`
// =============================================================================

/// @brief Parse a call: `callee(args)` or `callee(args)!`.
///
/// The callee is the expression to the left of the `(`. The arguments are
/// a comma-separated list inside the parentheses. The optional `!` marks
/// an argument pack, which is valid only inside a pipeline step.
///
/// The parser does not resolve the callee, does not check argument
/// counts, and does not check argument types. Sema does all of that.
///
/// ─── Argument pack `!` ────────────────────────────────────────────────────
/// The `!` after the closing `)` is the argument-pack marker:
///
///   `f(a, b)!`
///
/// The marker tells Sema that this call is a pipeline step whose first
/// argument is injected from the upstream value. The parser records the
/// marker as `hasArgPack` on the `CallExprAST`. Sema rejects a
/// `hasArgPack` call that is not a pipeline step.
///
/// A bare `!` without a preceding argument list is a syntax error
/// (handled elsewhere); the `!` here is always immediately after a `)`.
CallExprAST* parseCallExpr(TokenStream& stream,
                          ParserContext& ctx,
                          ExprAST* callee) {
    const SourceLocation loc = stream.currentLoc();

    if (!callee) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           loc,
                           "expected a callee for the call");
        return nullptr;
    }

    // ─── Argument list ────────────────────────────────────────────────────
    //
    // parseArgList consumes the `(` and `)` and produces the span.
    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);

    // ─── Optional argument pack `!` ───────────────────────────────────────
    const bool hasArgPack = stream.match(TokenType::BANG);

    auto* call = ctx.arena().make<CallExprAST>(hasArgPack);
    call->loc = loc;
    call->callee = callee;
    call->args = args;
    return call;
}

// =============================================================================
// parseFieldAccessExpr — `a.b`
// =============================================================================

/// @brief Parse a field access: `object.field`.
///
/// The object is the expression to the left of the `.`. The field is the
/// identifier to the right. The parser produces a `FieldAccessExprAST`;
/// Sema resolves the field against the object's type.
///
/// The form is also used for enum variant access (`Direction.North`).
/// Sema distinguishes the two by resolving the object's type: if the
/// object's type is an enum, the field is a variant; otherwise it is a
/// struct field.
///
/// ─── Field name ───────────────────────────────────────────────────────────
/// The field name must be an identifier. There is no syntax for a
/// computed field name; if the source writes `a.(expr)`, that is a syntax
/// error.
///
/// ─── No generic args on field access ──────────────────────────────────────
/// A field access cannot carry generic arguments. `a.field<T>` is a
/// syntax error; the parser does not consume the `<T>`. If the source
/// writes it, the `<` is a stray token that the enclosing expression
/// parser reports.
FieldAccessExprAST* parseFieldAccessExpr(TokenStream& stream,
                                         ParserContext& ctx,
                                         ExprAST* lhs) {
    const SourceLocation loc = stream.currentLoc();

    if (!lhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           loc,
                           "expected an object for field access");
        return nullptr;
    }

    if (!stream.match(TokenType::DOT)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '.', got '", stream.peekValue(), "'");
        return nullptr;
    }

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a field name after '.', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    Token fieldTok = stream.consume();
    InternedString fieldName = ctx.pool().intern(fieldTok.value);

    auto* fieldAccess = ctx.arena().make<FieldAccessExprAST>(fieldName);
    fieldAccess->loc = loc;
    fieldAccess->object = lhs;
    return fieldAccess;
}

// =============================================================================
// parseModuleAccessExpr — `a::b`
// =============================================================================

/// @brief Parse a module access or static member access.
///
/// The left-hand side is a name. The `::` separates it from the member
/// name. The parser produces a `ModuleAccessExprAST`; Sema resolves the
/// left-hand name against the module namespace and the struct namespace
/// and sets the appropriate flag on the node.
///
/// ─── Left-hand side ───────────────────────────────────────────────────────
/// The grammar requires the left-hand side to be a name:
///
///   `math::sqrt`       — a module name on the left
///   `Vec2::zero`       — a struct type name on the left
///
/// The parser reads an identifier for the LHS. If the source writes a
/// general expression on the left (`f()::member`, `a.b::c`), the parser
/// reports an error; the current grammar does not permit it.
///
/// ─── Member name and generics ─────────────────────────────────────────────
/// The member name is an identifier. It may carry generic arguments:
///
///   `Box<int>::default`   — a static member with generic context
//   `map::get<string, int>` — a module function with generic context
///
/// The parser reads the member name and any `<...>` that follows.
ModuleAccessExprAST* parseModuleAccessExpr(TokenStream& stream,
                                           ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Left-hand name ───────────────────────────────────────────────────
    //
    // The LHS is parsed as an identifier by the caller (parsePrimaryExpr
    // produces an IdentifierExprAST for the leading name). The caller
    // then sees a `::` and calls this function. But this function is
    // also called from `parsePostfixExpr` — wait, no. Let me re-check.
    //
    // In ParseExpr.cpp's postfix dispatch, `a::b` reaches this function
    // because the LHS `a` is already an expression (parsed by the
    // prefix/primary path as an IdentifierExprAST). This function reads
    // the `::` and the member name.
    //
    // So the LHS is not read here; it is passed in. But this function's
    // signature (per Parser.hpp) takes only `(stream, ctx)`. That's a
    // mismatch with how the caller dispatches.
    //
    // Two options:
    //   1. Change the signature to accept the LHS: `parseModuleAccessExpr
    //      (stream, ctx, lhs)`.
    //   2. Read the LHS inside this function: the caller guarantees the
    //      stream is positioned at the identifier before `::`.
    //
    // Option 2 matches the current signature but requires the caller to
    // have not yet consumed the LHS. Option 1 is cleaner: the caller has
    // already parsed the LHS (as part of the Pratt loop's prefix form),
    // and passes it in. See the note at the end of this file.

    // For now, assume the caller has not consumed the LHS and the stream
    // is on the identifier.
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           loc,
                           "expected a name before '::', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }
    Token lhsTok = stream.consume();
    InternedString lhsName = ctx.pool().intern(lhsTok.value);

    // ─── `::` ─────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::DOUBLE_COLON)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '::', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Member name ──────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a member name after '::', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }
    Token memberTok = stream.consume();
    InternedString memberName = ctx.pool().intern(memberTok.value);

    // ─── Optional generic arguments ───────────────────────────────────────
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }

    auto* moduleAccess = ctx.arena().make<ModuleAccessExprAST>(
        lhsName, memberName);
    moduleAccess->loc = loc;
    moduleAccess->genericArgs = genericArgs;
    return moduleAccess;
}

// =============================================================================
// parsePipelineExpr — `seed |> step |> step`
// =============================================================================

/// @brief Parse a pipeline chain.
///
/// The seed is the expression before the first `|>`. Each step is a
/// function value or a call with an argument pack, followed by `|>` to
/// continue the chain.
///
/// The parser produces a `PipelineExprAST` whose `seed` is the first
/// expression and whose `steps` is the span of parsed steps. The chain
/// is flat: `a |> b |> c` produces one PipelineExprAST with three
/// elements (the seed, then two steps).
///
/// ─── Precedence ───────────────────────────────────────────────────────────
/// `|>` is the loosest infix operator (level -1 in the grammar's table).
/// The seed is parsed by the caller at a lower precedence, so the seed
/// consumes every tighter operator before the pipeline begins.
ExprAST* parsePipelineExpr(TokenStream& stream,
                           ParserContext& ctx,
                           ExprAST* seed) {
    if (!seed) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a seed expression for the pipeline");
        return nullptr;
    }

    std::vector<PipelineStepAST*> steps;

    while (!stream.isAtEnd() && stream.check(TokenType::PIPELINE) &&
           ctx.canContinue()) {
        stream.consume();   // `|>`

        PipelineStepAST* step = parsePipelineStep(stream, ctx);
        if (step) {
            steps.push_back(step);
        } else {
            // parsePipelineStep reports its own error. Synchronize to
            // the next pipeline operator or to the end of the
            // expression.
            synchronizeTo(stream, ctx,
                          TokenType::PIPELINE,
                          TokenType::SEMICOLON,
                          TokenType::COMMA,
                          TokenType::RPAREN,
                          TokenType::RBRACKET,
                          TokenType::RBRACE);
            if (stream.check(TokenType::PIPELINE)) {
                continue;
            }
            break;
        }
    }

    auto builder = ctx.arena().makeBuilder<PipelineStepAST*>(steps.size());
    for (PipelineStepAST* s : steps) builder.push_back(s);

    auto* pipeline = ctx.arena().make<PipelineExprAST>(seed, builder.build());
    pipeline->loc = seed->loc;
    return pipeline;
}

// =============================================================================
// parsePipelineStep — one step in a pipeline
// =============================================================================

/// @brief Parse one step in a pipeline: `expr` or `expr(args)!`.
///
/// A step is a function value, or a call with an argument pack.
///
/// ─── Two forms ────────────────────────────────────────────────────────────
///
///   `expr`            the step is a function value. The upstream value
///                     is passed as its single argument.
///
///   `expr(args)!`     the step is a call with an argument pack. The
///                     upstream value is injected as the call's first
///                     argument; the other arguments fill the rest.
///
/// The `!` is required for the second form. `expr(args)` without the `!`
/// is not a valid step; the parser reports the missing `!`.
///
/// ─── Parser shape ─────────────────────────────────────────────────────────
/// The step is parsed as a general expression. If the result is a call
/// whose `hasArgPack` flag is true, the step's `callable` is the call's
/// callee and `packArgs` is the call's argument list. Otherwise, the
/// step's `callable` is the expression itself and `packArgs` is empty.
///
/// ─── Function literal as a step ───────────────────────────────────────────
/// A function literal in step position (`|> fn (x int) -> int { ... }`)
/// is a valid step. The parser handles it because a function literal is
/// an expression, and the general expression parse produces the
/// AnonFuncExprAST.
///
/// ─── Intrinsic calls are rejected ─────────────────────────────────────────
/// A `#host(...)` or `#builtin(...)` target cannot be a pipeline step's
/// callable in the current design: those are target markers, not
/// expressions. If the parser encounters one here, it reports and
/// recovers.
PipelineStepAST* parsePipelineStep(TokenStream& stream,
                                   ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (stream.isAtEnd()) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           loc,
                           "expected a pipeline step, but the input ended");
        return nullptr;
    }

    // ─── Parse the step's expression ──────────────────────────────────────
    //
    // A pipeline step has its own precedence floor: it consumes the
    // step's expression at a precedence above the pipeline operator, so
    // the expression parse stops at the next `|>`.
    constexpr int kPipelineStepPrec = 0;   // above `|>` at -1
    ExprAST* expr = parsePrattExpr(stream, ctx, kPipelineStepPrec);

    if (!expr) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected a pipeline step expression");
        return nullptr;
    }

    // ─── Distinguish the two forms ────────────────────────────────────────
    //
    //   - CallExprAST with hasArgPack: `f(args)!` — a call step.
    //   - Anything else: a bare step (a function value, a literal, ...).
    ExprAST*            callable = expr;
    ArenaSpan<ExprAST*> packArgs;

    if (expr->isa<CallExprAST>()) {
        CallExprAST* call = expr->as<CallExprAST>();
        if (call->hasArgPack) {
            // A call step. The step's callable is the call's callee;
            // the step's arguments are the call's arguments.
            callable = call->callee;
            packArgs = call->args;
        } else {
            // A call without `!`. This is not a valid step; the
            // upstream value would be discarded.
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               call->loc,
                               "expected '!' after the argument list in "
                               "a pipeline step; a call in step position "
                               "must use the argument-pack form 'f(args)!'");
            // Recover: treat the call as the step's callable with no
            // arguments. The upstream value will be injected as the
            // call's sole argument at Sema's check.
            callable = expr;
        }
    }

    auto* step = ctx.arena().make<PipelineStepAST>(callable, packArgs);
    step->loc = loc;
    return step;
}

} // namespace lucid::parser