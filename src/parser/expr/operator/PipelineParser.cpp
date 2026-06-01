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

/**
 * @brief Parses a pipeline expression (`seed |> step |> step |> ...`).
 *
 * Grammar:
 *   pipeline_expr := pipeline_seed { '|>' pipeline_step }
 *
 * The seed is already parsed by the Pratt parser (as a logical expression).
 * This function consumes one or more `|>` operators and their right‑hand
 * steps, building a chain of runtime function applications.
 *
 * @param seed The left‑hand side expression (already parsed).
 * @return ExprPtr – PipelineExprAST if at least one step is found,
 *         otherwise the original seed (no pipeline).
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the seed expression.
 * On exit:  positioned after the last pipeline step.
 *
 * ─── Example ──────────────────────────────────────────────────────────────
 *   Input:  `42 |> float |> sqrt`
 *   Steps:  parsePipelineExpr(42)
 *             → consumes `|> float |> sqrt`
 *             → returns PipelineExprAST { seed = 42, steps = [float, sqrt] }
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - If a `|>` is found but parsePipelineStep() returns a step with an
 *     UnknownExprAST (because of parsing error), the loop still adds it to
 *     the steps vector and continues. The error is already reported.
 *   - If no steps are collected (should not happen because parsePipelineStep
 *     always returns a step), the function returns the seed.
 */
ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    if (!seed) {
        errorAt(DiagCode::E2008, "expected pipeline seed before '|>'");
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

/**
 * @brief Parses a single pipeline step (right‑hand side of `|>`).
 *
 * Grammar:
 *   pipeline_step := func_ref [ '(' arg_list ')' '!' ] | anon_func
 *
 * This function is guaranteed to never return nullptr. On error, it returns
 * a step with an `UnknownExprAST` as the callable and skips to the next
 * pipeline operator or safe boundary.
 *
 * @return PipelineStepPtr – always a valid node (never nullptr).
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the start of a function reference or anonymous
 *           function.
 * On exit:  positioned after the step (and optional argument pack), or after
 *           skipping invalid tokens to the next '|>' or safe boundary.
 *
 * ─── Supported Step Forms ─────────────────────────────────────────────────
 *   | Form                          | Example                          |
 *   |-------------------------------|----------------------------------|
 *   | Plain identifier              | `validate`                       |
 *   | Dotted path                   | `math.utils.normalize`           |
 *   | Method reference              | `Vec2:normalize`                 |
 *   | Generic instantiation         | `identity<int>`                  |
 *   | Generic method reference      | `list:map<U>`                    |
 *   | Argument pack (plain)         | `scale(2.0)!`                    |
 *   | Argument pack (method)        | `list:push(item)!`               |
 *   | Argument pack (generic)       | `map<int, string>(toString)!`    |
 *   | Anonymous function            | `(x int) -> int { return x * 2 }`|
 *
 * ─── What is NOT Allowed ───────────────────────────────────────────────────
 *   - Parenthesised expressions (e.g., `(f)(x)` – not valid as a step)
 *   - Binary operators (e.g., `a + b` – not a function reference)
 *   - Curry functions with multiple groups (e.g., `add` with `(a)(b)`)
 *     – pre‑apply all but the last group first.
 *
 * ─── Argument Pack (`!`) ──────────────────────────────────────────────────
 *   The `!` annotation marks an intentionally incomplete argument list.
 *   The upstream value from the pipeline is injected as the first argument
 *   when the step is executed.
 *
 *   Example:
 *     let scale (v Vec2, factor float) -> Vec2 = { ... }
 *     v |> scale(2.0)!     -- calls scale(v, 2.0)
 *
 * ─── Error Recovery Details ───────────────────────────────────────────────
 *   - If `parseFuncRef()` fails (returns nullptr or UnknownExprAST), an error
 *     is reported. A placeholder step is created with `UnknownExprAST` as the
 *     callable.
 *   - The parser then consumes tokens until it finds:
 *       - Another pipeline operator `|>` (so the loop can continue)
 *       - A semicolon or closing brace (statement boundary)
 *       - End of file
 *   - This ensures that invalid steps do not cause infinite loops and that
 *     subsequent valid steps after another `|>` are still parsed.
 *   - The placeholder step is added to the steps vector, preserving the
 *     structure of the pipeline.
 *
 * ─── Semantic Validation (Not Parser Responsibility) ──────────────────────
 *   - The resolved callable must be a function (not a type or value).
 *   - Generic function references must have all type arguments supplied.
 *   - The input type of the step must match the output type of the previous
 *     step or seed.
 *   - The step cannot be ~nullable or ~parallel (enforced by semantic pass).
 *   - If the step is ~async, the entire pipeline becomes async.
 */
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
        errorAt(DiagCode::E2002,
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
            errorAt(DiagCode::E2001,
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