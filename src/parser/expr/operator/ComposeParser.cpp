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
 *             | primitive_type                // int, float, string, etc.
 *             | IDENTIFIER '.' IDENTIFIER
 *             | IDENTIFIER ':' IDENTIFIER
 *             | func_ref generic_args
 * 
 * Examples:
 *   let process = validate +> transform +> render
 *   let pipeline = toString<int> +> parseFloat +> double
 *   let chain = math.utils.normalize +> vec:scale
 *   let convert = int +> string               // type references as functions
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
#include "debug/DebugMacros.hpp"

// ============================================================================
// Composition Expression
// ============================================================================

ExprPtr Parser::parseComposeExpr(ExprPtr lhs) {
    LUC_LOG_EXPR_VERBOSE("parseComposeExpr: entering");
    
    auto node = arena_.make<ComposeExprAST>();
    node->loc = lhs->loc;
    node->left = lhs;  // No std::move

    std::vector<ComposeOperandPtr> operands;
    int operandCount = 0;

    while (ts_.check(TokenType::COMPOSE)) {
        LUC_LOG_EXPR_EXTREME("parseComposeExpr: found '+>' operator #" << operandCount + 1);
        ts_.advance();  // consume '+>'
        operands.push_back(parseComposeOperand());
        operandCount++;
    }

    // If no operands were parsed, this wasn't actually a composition.
    if (operands.empty()) {
        LUC_LOG_EXPR_EXTREME("parseComposeExpr: no operands, returning left expression");
        return node->left;  // No std::move
    }

    // Build ArenaSpan for the operands
    auto builder = arena_.makeBuilder<ComposeOperandPtr>();
    for (auto& op : operands) builder.push_back(op);  // No std::move
    node->operands = builder.build();

    LUC_LOG_EXPR_VERBOSE("parseComposeExpr: parsed " << operandCount << " operand(s)");
    return node;
}

// ============================================================================
// Compose Operand
// ============================================================================

ComposeOperandPtr Parser::parseComposeOperand() {
    LUC_LOG_EXPR_EXTREME("parseComposeOperand: entering");
    
    // Parse a function reference using the shared helper.
    ExprPtr callable = parseFuncRef();

    auto operand = arena_.make<ComposeOperandAST>();
    operand->loc = ts_.currentLoc();

    // Check for parse failure
    if (!callable || callable->isa<UnknownExprAST>()) {
        LUC_LOG_EXPR("parseComposeOperand: ERROR - expected function reference after '+>'");
        errorAt(DiagCode::E1002,
                "expected function name, type name, method reference, or dotted path after '+>'");
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

    LUC_LOG_EXPR_EXTREME("parseComposeOperand: success");
    operand->callable = callable;  // No std::move
    return operand;
}