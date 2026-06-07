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
 *             | primitive_type                // int, float, string, etc.
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 * 
 * Examples:
 *   42 |> float |> sqrt
 *   getUser(id) |> validate |> save
 *   v |> Vec2:normalize |> scale(2.0)!
 *   numbers |> filter<int>(isPositive)! |> sum
 *   "42" |> int |> string                    // type references in pipeline
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
 * @see ParserHelpers.cpp for parseFuncRef
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Pipeline Expression
// ============================================================================

/**
 * @brief Parses a pipeline expression: seed followed by zero or more `|>` steps.
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   pipeline_expr   := pipeline_seed { '|>' pipeline_step }
 *   pipeline_seed   := expr
 *
 * This function is called from the Pratt parser when a `|>` operator is
 * encountered after a primary expression. The caller provides the seed
 * expression (the left operand of the first `|>`).
 *
 * @param seed The expression before the first `|>` operator.
 * @return ExprPtr – PipelineExprAST if at least one step was parsed,
 *         otherwise returns the seed unchanged (no pipeline).
 *
 * ─── Parsing Behaviour ─────────────────────────────────────────────────────
 * - Consumes one or more `|>` operators, each followed by a pipeline step.
 * - Each step is parsed by `parsePipelineStep()`, which handles errors
 *   gracefully (never returns nullptr).
 * - If no `|>` operator is found, the function returns the seed as-is.
 *
 * ─── AST Construction ──────────────────────────────────────────────────────
 * - Creates a PipelineExprAST node.
 * - Stores the seed as `seed`.
 * - Stores the list of steps (PipelineStepAST) as `steps`.
 * - The location of the pipeline is the seed's location.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Even if a step fails to parse, `parsePipelineStep()` returns a step
 *   containing an `UnknownExprAST`, so the pipeline loop continues.
 * - The resulting AST will contain error markers; semantic analysis can
 *   skip them and report diagnostics.
 *
 * @see parsePipelineStep()
 */
ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    LUC_LOG_EXPR_VERBOSE("parsePipelineExpr: entering");
    
    if (!seed) {
        LUC_LOG_EXPR("parsePipelineExpr: ERROR - expected pipeline seed before '|>'");
        errorAt(DiagCode::E1008, "expected pipeline seed before '|>'");
        return arena_.make<UnknownExprAST>();
    }

    std::vector<PipelineStepPtr> steps;
    int stepCount = 0;

    while (ts_.check(TokenType::PIPELINE)) {
        LUC_LOG_EXPR_EXTREME("parsePipelineExpr: found '|>' operator #" << stepCount + 1);
        ts_.advance();  // consume '|>'
        steps.push_back(parsePipelineStep());
        stepCount++;
    }

    if (steps.empty()) {
        // No '|>' operators were found – this is not a pipeline
        LUC_LOG_EXPR_EXTREME("parsePipelineExpr: no pipeline steps, returning seed");
        return seed;
    }

    auto node = arena_.make<PipelineExprAST>();
    node->loc = seed->loc;
    node->seed = seed;  // No std::move

    auto builder = arena_.makeBuilder<PipelineStepPtr>();
    for (auto& s : steps) builder.push_back(s);  // No std::move
    node->steps = builder.build();

    LUC_LOG_EXPR_VERBOSE("parsePipelineExpr: parsed " << stepCount << " step(s)");
    return node;
}

// ============================================================================
// Pipeline Step
// ============================================================================

/**
 * @brief Parses a single pipeline step: a function reference or anonymous
 *        function, optionally with an argument pack.
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   pipeline_step   := func_ref [ '(' arg_list ')' '!' ] | anon_func
 *
 *   func_ref := IDENTIFIER
 *             | primitive_type
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 *
 * This function is called for each `|>` operator in a pipeline expression.
 * It consumes tokens starting at the first token after `|>` and stops
 * at the next `|>` or a safe boundary (semicolon, brace, EOF).
 *
 * @return PipelineStepPtr – never returns nullptr. On error, returns a step
 *         whose `callable` is an `UnknownExprAST`.
 *
 * ─── Valid Step Forms ──────────────────────────────────────────────────────
 * 1. Bare function reference:
 *       42 |> sqrt
 *       42 |> vec:normalize
 *       42 |> math.utils.toString
 *       42 |> int                           // type reference
 *
 * 2. Function reference with argument pack (injection):
 *       42 |> scale(2.0)!
 *       42 |> filter<int>(isPositive)!
 *   The `!` indicates the argument list is intentionally incomplete;
 *   the upstream value is injected as the **first** argument at runtime.
 *   The parsed arguments are stored in `packArgs`.
 *
 * 3. Anonymous function:
 *       42 |> (x int) int { return x * x }
 *
 * ─── Parsing Steps ─────────────────────────────────────────────────────────
 * 1. Check if the next token looks like an anonymous function; if yes,
 *    parse it and return.
 * 2. Otherwise, parse a function reference using `parseFuncRef()`.
 *    - This handles identifiers, primitive types, dotted paths, method references (`:`),
 *      and generic arguments (e.g., `filter<int>`).
 * 3. If parsing fails, create an error placeholder step and skip to the
 *    next safe boundary (|>, ;, }, EOF).
 * 4. If successful, optionally parse an argument pack:
 *    - Expect '('
 *    - Parse a comma‑separated list of expressions (similar to `parseArgList`)
 *    - Expect ')'
 *    - **Require** a trailing `!` token – otherwise report an error and return
 *      the step without packArgs (the callable remains, but the step lacks
 *      injection).
 *    - If `!` is present, store the arguments in `packArgs`.
 *
 * ─── Why `!` is only parsed here (not in regular calls) ────────────────────
 * The argument pack annotation is a pipeline‑only feature. Regular function
 * calls (outside a pipeline) must never have a trailing `!`. The `CallParser.cpp`
 * rejects `!` with a clear error. This separation ensures the grammar is
 * unambiguous and the AST reflects the intent.
 *
 * ─── Error Recovery ────────────────────────────────────────────────────────
 * - If `parseFuncRef()` fails, we create an `UnknownExprAST` inside the step
 *   and skip tokens until a safe boundary. This allows parsing of subsequent
 *   pipeline steps even after an error.
 * - If the argument pack is malformed (e.g., missing `!`), we report an error
 *   but still return a valid step (without packArgs). The pipeline can continue.
 * - Consecutive errors in argument expressions are bounded to avoid infinite loops.
 *
 * @see parseFuncRef()
 * @see parseAnonFuncExpr()
 */

PipelineStepPtr Parser::parsePipelineStep() {
    LUC_LOG_EXPR_EXTREME("parsePipelineStep: entering");
    
    // 1. Anonymous function step
    if (looksLikeAnonFunc()) {
        LUC_LOG_EXPR_EXTREME("parsePipelineStep: anonymous function step");
        auto step = arena_.make<PipelineStepAST>();
        step->loc = ts_.currentLoc();
        step->callable = parseAnonFuncExpr();
        return step;
    }

    // 2. Parse function reference (may be generic, path, method, or primitive type)
    ExprPtr callable = parseFuncRef();

    // 3. Handle parse failure
    if (!callable || callable->isa<UnknownExprAST>()) {
        LUC_LOG_EXPR("parsePipelineStep: ERROR - expected function reference or anonymous function");
        errorAt(DiagCode::E1002,
                "expected function name, type name, method reference, or anonymous function");

        // Create error placeholder step
        auto step = arena_.make<PipelineStepAST>();
        step->loc = ts_.currentLoc();
        step->callable = arena_.make<UnknownExprAST>();

        // Recover: skip to next pipeline operator or safe boundary
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
    step->callable = callable;  // No std::move
    
    LUC_LOG_EXPR_EXTREME("parsePipelineStep: callable parsed");

    // 5. Optional argument pack: '(' arg_list ')' '!'
    if (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_EXPR_EXTREME("parsePipelineStep: parsing argument pack");
        ts_.advance();

        std::vector<ExprPtr> packArgs;
        int consecutiveErrors = 0;
        const int MAX_CONSECUTIVE_ERRORS = 5;
        int argCount = 0;

        // Parse comma‑separated expressions until closing ')'
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
                LUC_LOG_EXPR("parsePipelineStep: ERROR - too many consecutive errors in argument pack");
                errorAt(DiagCode::E1002, "too many consecutive errors in argument pack; skipping to ')'");
                while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
                break;
            }

            size_t savedPos = ts_.getPos();
            ExprPtr arg = parseExpr();

            if (ts_.getPos() == savedPos) {
                LUC_LOG_EXPR("parsePipelineStep: ERROR - expected argument expression");
                errorAt(DiagCode::E1008, "expected argument expression");
                if (!ts_.isAtEnd()) ts_.advance();
                consecutiveErrors++;
                if (ts_.check(TokenType::COMMA)) ts_.advance();
                continue;
            }

            consecutiveErrors = 0;
            argCount++;
            LUC_LOG_EXPR_EXTREME("parsePipelineStep: argument #" << argCount);
            packArgs.push_back(arg);  // No std::move

            if (ts_.check(TokenType::RPAREN)) break;
            if (!ts_.match(TokenType::COMMA)) {
                LUC_LOG_EXPR("parsePipelineStep: ERROR - expected ',' after argument");
                errorAt(DiagCode::E1001, "expected ',' after argument");
                while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                    ts_.advance();
                }
                if (ts_.check(TokenType::COMMA)) ts_.advance();
                break;
            }
        }

        ts_.consume(TokenType::RPAREN, "expected ')'");

        if (!ts_.match(TokenType::BANG)) {
            LUC_LOG_EXPR("parsePipelineStep: ERROR - expected '!' after arguments");
            errorAt(DiagCode::E1001,
                    "expected '!' after arguments for argument pack");
            // Still return the step – it just won't have packArgs
            return step;
        }

        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(a);  // No std::move
        step->packArgs = builder.build();
        
        LUC_LOG_EXPR_EXTREME("parsePipelineStep: argument pack with " << argCount << " argument(s)");
    }

    return step;
}