/**
 * @file IntrinsicParser.cpp
 * @brief Parses compiler intrinsic calls prefixed with `#`.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of intrinsic function calls – compiler‑provided
 * built‑ins that perform low‑level operations such as type queries, math,
 * memory manipulation, and bit operations.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   intrinsic_call := '#' IDENTIFIER '(' [ arg_list ] ')'
 *   arg_list := expr { ',' expr }
 * 
 * Parser Responsibility:
 *   - Parse the `#` prefix
 *   - Parse the intrinsic name (identifier)
 *   - Parse the argument list inside parentheses using parseArgList()
 *   - Store ALL arguments as expressions in `IntrinsicCallExprAST::args`
 * 
 * Semantic Responsibility (NOT Parser):
 *   - Validate the intrinsic name is known
 *   - Check argument count matches the intrinsic's signature
 *   - Determine if arguments are types or expressions
 *   - Resolve types and perform any necessary conversions
 * 
 * Examples:
 *   #sizeof(Vertex)          → parsed as intrinsic name "sizeof", args=[Vertex]
 *   #sqrt(x)                 → parsed as intrinsic name "sqrt", args=[x]
 *   #memcpy(dst, src, len)   → parsed as intrinsic name "memcpy", args=[dst, src, len]
 *   #clz(flags)              → parsed as intrinsic name "clz", args=[flags]
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see IntrinsicCallExprAST for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Intrinsic Call
// ============================================================================

/**
 * @brief Parses a compiler intrinsic call: `#name(args)`.
 *
 * Grammar:
 *   intrinsic_call := '#' IDENTIFIER '(' [ arg_list ] ')'
 *   arg_list := expr { ',' expr }
 *
 * IMPORTANT: The parser treats ALL arguments as expressions. The semantic
 * pass is responsible for:
 *   - Validating the intrinsic name
 *   - Determining which arguments are actually type arguments
 *   - Converting type arguments from identifier expressions to type AST nodes
 *
 * This design keeps the parser simple and maintainable. Adding a new
 * intrinsic requires NO changes to the parser – only semantic validation.
 *
 * @return ExprPtr – IntrinsicCallExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '#'.
 * On exit:  positioned after the closing ')'.
 *
 * ─── Argument Parsing ─────────────────────────────────────────────────────
 *   - All arguments are parsed using parseArgList()
 *   - This includes what may later be interpreted as type names (e.g., in
 *     `#sizeof(Vertex)`, "Vertex" is parsed as IdentifierExprAST)
 *   - The semantic pass will convert identifier expressions to type AST nodes
 *     when appropriate for the intrinsic
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '(' after intrinsic name: reports error, returns UnknownExprAST
 *   - parseArgList() handles argument parsing errors
 *   - Missing ')' after arguments: consume() reports error
 */
ExprPtr Parser::parseIntrinsicCallExpr() {
    LUC_LOG_EXPR_VERBOSE("parseIntrinsicCallExpr: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::HASH, "expected '#'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_EXPR("parseIntrinsicCallExpr: ERROR - expected intrinsic name after '#'");
        errorAt(DiagCode::E1003, "expected intrinsic name after '#'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc = loc;
    node->intrinsicName = pool_.intern(ts_.advance().value);
    
    std::string intrinsicStr = std::string(pool_.lookup(node->intrinsicName));
    LUC_LOG_EXPR_EXTREME("parseIntrinsicCallExpr: intrinsic name = #" << intrinsicStr);

    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_EXPR("parseIntrinsicCallExpr: ERROR - expected '(' after intrinsic name");
        errorAt(DiagCode::E1001, "expected '(' after intrinsic name");
        return arena_.make<UnknownExprAST>();
    }
    ts_.advance();  // consume '('

    // Parse argument list using the shared helper
    // parseArgList() handles empty argument list, commas, and error recovery
    node->args = parseArgList();
    
    LUC_LOG_EXPR_EXTREME("parseIntrinsicCallExpr: " << node->args.size() << " argument(s)");
    
    ts_.consume(TokenType::RPAREN, "expected ')' to close intrinsic call");

    LUC_LOG_EXPR_VERBOSE("parseIntrinsicCallExpr: success");
    return node;
}