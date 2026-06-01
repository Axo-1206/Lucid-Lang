/**
 * @file ComposeParser.cpp
 * @brief Parses the composition operator (`+>`) for compile‑time function chaining.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of function composition expressions using the
 * `+>` operator. Composition wires functions together at compile time,
 * producing a new function that is the sequential application of the operands.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   compose_expr    := pipeline_expr { '+>' compose_operand }
 *   compose_operand := func_ref
 * 
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 * 
 * Examples:
 *   let process = validate +> transform +> render
 *   let pipeline = toString<int> +> parseFloat +> double
 *   let chain = math.utils.normalize +> vec:scale
 * 
 * ─── Important Rules ───────────────────────────────────────────────────────
 *   - The output type of the left operand must exactly match the input type
 *     of the right operand (enforced by semantic pass).
 *   - Composition produces a new plain function; qualifiers (~async, ~nullable)
 *     live on the binding, not on the composed result.
 *   - Generic functions must be instantiated with explicit type arguments
 *     before composition (e.g., `toString<int>`, not `toString`).
 *   - Curry functions are forbidden as operands — pre‑apply all but the last
 *     group before composing.
 *   - Nullable function operands are forbidden unless guarded by a nil check
 *     before the composition expression.
 * 
 * ─── Error Recovery Strategy ──────────────────────────────────────────────
 *   - `parseComposeOperand()` never returns nullptr. On error, it returns a
 *     node with `UnknownExprAST` as the callable and consumes tokens until
 *     the next `+>` or a safe boundary (semicolon, brace, EOF).
 *   - The loop in `parseComposeExpr()` continues after an error, allowing
 *     subsequent operands to be parsed.
 *   - No infinite loops occur because invalid tokens are consumed.
 * 
 * @see ParserExpr.cpp for Pratt parser integration
 * @see ParserHelpers.cpp for parseFuncRef
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Composition Expression
// ============================================================================

/**
 * @brief Parses a function composition expression (`lhs +> operand +> ...`).
 *
 * Grammar:
 *   compose_expr := pipeline_expr { '+>' compose_operand }
 *
 * The left‑hand side is already parsed by the Pratt parser (as a pipeline
 * expression). This function consumes one or more `+>` operators and their
 * right‑hand operands, building a chain of composed functions.
 *
 * @param lhs The left‑hand side expression (already parsed).
 * @return ExprPtr – ComposeExprAST on success, or the original lhs if no
 *         `+>` operators are present (meaning this is not a composition).
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the left‑hand side expression.
 * On exit:  positioned after the last compose operand.
 *
 * ─── Example ──────────────────────────────────────────────────────────────
 *   Input:  `validate +> transform +> render`
 *   Steps:  parseComposeExpr(validate)
 *             → consumes `+> transform +> render`
 *             → returns ComposeExprAST { left = validate, operands = [transform, render] }
 *
 * ─── Edge Cases ───────────────────────────────────────────────────────────
 *   - If no `+>` is found, the function returns the lhs unchanged.
 *   - A single `+>` with one operand produces a ComposeExprAST with one operand.
 *   - Multiple `+>` operators chain left‑associatively.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - `parseComposeOperand()` always returns a valid node (never nullptr),
 *     even on error (with an UnknownExprAST placeholder).
 *   - The loop continues after an error, so all operands are collected.
 *   - The error recovery inside `parseComposeOperand()` consumes invalid
 *     tokens, preventing infinite loops.
 */
ExprPtr Parser::parseComposeExpr(ExprPtr lhs) {
    auto node = arena_.make<ComposeExprAST>();
    node->loc = lhs->loc;
    node->left = std::move(lhs);

    std::vector<ComposeOperandPtr> operands;

    while (ts_.check(TokenType::COMPOSE)) {
        ts_.advance();  // consume '+>'
        operands.push_back(parseComposeOperand());  // always returns a node
    }

    // If no operands were parsed, this wasn't actually a composition.
    if (operands.empty()) {
        return std::move(node->left);
    }

    // Build ArenaSpan for the operands
    auto builder = arena_.makeBuilder<ComposeOperandPtr>();
    for (auto& op : operands) builder.push_back(std::move(op));
    node->operands = builder.build();

    return node;
}

// ============================================================================
// Compose Operand
// ============================================================================

/**
 * @brief Parses a single composition operand (right‑hand side of `+>`).
 *
 * Grammar:
 *   compose_operand := func_ref
 *
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 *
 * @return ComposeOperandPtr – always a valid node (never nullptr).
 *         On error, returns a node with an UnknownExprAST as the callable.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the start of a function reference.
 * On exit:  positioned after the function reference, or after skipping
 *           invalid tokens to the next `+>` or safe boundary.
 *
 * ─── Supported Forms ──────────────────────────────────────────────────────
 *   - Plain identifier:        `validate`
 *   - Dotted path:             `math.utils.normalize`
 *   - Method reference:        `vec:scale`
 *   - Generic instantiation:   `toString<int>`
 *   - Any combination:         `math.utils.toString<int>`, `list:map<U>`
 *
 * ─── What is NOT Allowed ───────────────────────────────────────────────────
 *   - Anonymous functions (not allowed in composition operands)
 *   - Argument pack `(args)!` (only valid in pipeline steps, not composition)
 *   - Parenthesised expressions
 *   - Binary operators
 *
 * ─── Error Recovery Details ───────────────────────────────────────────────
 *   - If `parseFuncRef()` fails (returns nullptr or UnknownExprAST), an error
 *     is reported.
 *   - A placeholder operand is created with `UnknownExprAST` as the callable.
 *   - The parser then consumes tokens until it finds:
 *       - Another `+>` operator (so the loop can continue)
 *       - A semicolon or closing brace (statement boundary)
 *       - End of file
 *   - This ensures that invalid operands do not cause infinite loops and that
 *     subsequent valid operands after another `+>` are still parsed.
 *
 * ─── Semantic Validation (Not Parser Responsibility) ──────────────────────
 *   - The resolved callable must be a plain function (no curry, no ~nullable,
 *     no ~parallel) – enforced by semantic pass.
 *   - Generic function references must have all type arguments supplied.
 *   - The output type of the previous operand must match the input type of
 *     this operand.
 */
ComposeOperandPtr Parser::parseComposeOperand() {
    // Parse a function reference using the shared helper.
    ExprPtr callable = parseFuncRef();

    auto operand = arena_.make<ComposeOperandAST>();
    operand->loc = ts_.currentLoc();

    // Check for parse failure
    if (!callable || callable->isa<UnknownExprAST>()) {
        errorAt(DiagCode::E2002,
                "expected function name, method reference, or dotted path after '+>'");
        operand->callable = arena_.make<UnknownExprAST>();

        // Recover: skip to next compose operator or safe boundary
        // This prevents infinite loops when parseFuncRef consumes no tokens.
        while (!ts_.isAtEnd() &&
               !ts_.check(TokenType::COMPOSE) &&
               !ts_.check(TokenType::SEMICOLON) &&
               !ts_.check(TokenType::RBRACE) &&
               !ts_.check(TokenType::EOF_TOKEN)) {
            ts_.advance();
        }

        return operand;
    }

    operand->callable = std::move(callable);
    return operand;
}