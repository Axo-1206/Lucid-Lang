/**
 * @file PipelineParser.cpp
 * @brief Parses the pipeline operator (`|>`) for runtime function chaining.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of pipeline expressions where the output of one
 * function becomes the input to the next, executing left‑to‑right at runtime.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   pipeline_expr   := pipeline_seed { '|>' pipeline_step }
 *   pipeline_seed   := expr
 *   pipeline_step   := func_ref [ '(' arg_list ')' '!' ] | anon_func
 * 
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 * 
 * Examples:
 *   42 |> float |> sqrt
 *   getUser(id) |> validate |> save
 *   v |> Vec2:normalize |> scale(2.0)!
 *   numbers |> filter<int>(isPositive)! |> sum
 * 
 * ─── Important Rules ───────────────────────────────────────────────────────
 *   - The pipeline short‑circuits on Error when the error library is used.
 *   - Steps with `~async` are allowed; the entire pipeline becomes async and
 *     must be awaited.
 *   - Steps with `~nullable` are forbidden – guard before the pipeline.
 *   - Steps with `~parallel` are forbidden – pipeline execution is synchronous.
 *   - Curry functions cannot be used directly as steps; pre‑apply all but the
 *     last group first (e.g., `let addTen = add(10); 42 |> addTen`).
 *   - The `!` argument pack annotation marks an intentionally incomplete
 *     argument list; the upstream value is injected as the first argument.
 * 
 * ─── Error Recovery Strategy ──────────────────────────────────────────────
 *   parsePipelineStep() NEVER returns nullptr. On failure, it returns a step
 *   whose `callable` is an `UnknownExprAST`. It also consumes tokens until
 *   the next `|>` or a safe boundary (semicolon, brace, EOF). This allows
 *   the pipeline loop to continue parsing subsequent steps after an error.
 *   The resulting AST contains error placeholders, and semantic analysis
 *   can skip or report them.
 * 
 * @see ParserExpr.cpp for Pratt parser integration
 * @see ParserHelpers.cpp for parseFuncRef, parseExprList
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Pipeline Expression
// ============================================================================

ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    if (!seed) {
        errorAt(DiagCode::E1008, "expected pipeline seed before '|>'");
        return arena_.make<UnknownExprAST>();
    }

    std::vector<PipelineStepPtr> steps;

    while (ts_.check(TokenType::PIPELINE)) {
        ts_.advance();  // consume '|>'
        steps.push_back(parsePipelineStep());  // always returns a step
    }

    if (steps.empty()) {
        // No '|>' operators were found – this is not a pipeline
        return seed;
    }

    auto node = arena_.make<PipelineExprAST>();
    node->loc = seed->loc;
    node->seed = std::move(seed);

    auto builder = arena_.makeBuilder<PipelineStepPtr>();
    for (auto& s : steps) builder.push_back(std::move(s));
    node->steps = builder.build();

    return node;
}

// ============================================================================
// Pipeline Step
// ============================================================================

PipelineStepPtr Parser::parsePipelineStep() {
    // 1. Anonymous function step
    if (looksLikeAnonFunc()) {
        auto step = arena_.make<PipelineStepAST>();
        step->loc = ts_.currentLoc();
        step->callable = parseAnonFuncExpr();
        return step;
    }

    // 2. Parse function reference (may be generic, dotted, method)
    ExprPtr callable = parseFuncRef();

    // 3. Handle parse failure
    if (!callable || callable->isa<UnknownExprAST>()) {
        errorAt(DiagCode::E1002,
                "expected function name, method reference, or anonymous function");

        // Create error placeholder step
        auto step = arena_.make<PipelineStepAST>();
        step->loc = ts_.currentLoc();
        step->callable = arena_.make<UnknownExprAST>();

        // Recover: skip to next pipeline operator or safe boundary
        // This ensures we don't get stuck on invalid tokens and allows
        // the pipeline loop to continue with the next step.
        while (!ts_.isAtEnd() &&
               !ts_.check(TokenType::PIPELINE) &&
               !ts_.check(TokenType::SEMICOLON) &&
               !ts_.check(TokenType::RBRACE) &&
               !ts_.check(TokenType::EOF_TOKEN)) {
            ts_.advance();
        }

        return step;
    }

    // 4. Success – create step with parsed callable
    auto step = arena_.make<PipelineStepAST>();
    step->loc = callable->loc;
    step->callable = std::move(callable);

    // 5. Optional argument pack: '(' arg_list ')' '!'
    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();

        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) {
            packArgs = parseExprList(TokenType::RPAREN);
        }
        ts_.consume(TokenType::RPAREN, "expected ')'");

        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E1001,
                    "expected '!' after arguments for argument pack");
            // Still return the step – it just won't have packArgs
            return step;
        }

        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}