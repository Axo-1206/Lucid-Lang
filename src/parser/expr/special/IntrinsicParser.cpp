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
 *   intrinsic_call := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
 *   intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }
 *   intrinsic_arg := expr | type
 * 
 * Intrinsics fall into two categories:
 *   - Type intrinsics: #sizeof(T), #alignof(T) – take a single type argument.
 *   - Value intrinsics: all others – take expression arguments.
 * 
 * Examples:
 *   #sizeof(Vertex)          → compile‑time size of type Vertex
 *   #sqrt(x)                 → hardware‑accelerated square root
 *   #memcpy(dst, src, len)   → LLVM memcpy intrinsic
 *   #clz(flags)              → count leading zero bits
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see IntrinsicCallExprAST for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Intrinsic Call
// ============================================================================

/**
 * @brief Parses a compiler intrinsic call: `#name(args)`.
 *
 * Grammar:
 *   intrinsic_call := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
 *   intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }
 *   intrinsic_arg := expr | type
 *
 * Two categories are distinguished by the intrinsic name:
 *   - Type intrinsics: `#sizeof`, `#alignof` – consume a single type argument.
 *   - Value intrinsics: all others – consume a comma‑separated list of expressions.
 *
 * @return ExprPtr – IntrinsicCallExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '#'.
 * On exit:  positioned after the closing ')'.
 *
 * ─── Type Intrinsics (#sizeof, #alignof) ──────────────────────────────────
 *   - Take exactly one type argument (no expressions allowed).
 *   - Return compile‑time constant (uint64).
 *   - Example: `#sizeof(Vertex)`, `#alignof(Vec2)`
 *
 * ─── Value Intrinsics ──────────────────────────────────────────────────────
 *   - Take zero or more expression arguments.
 *   - Arguments are parsed with `parseExpr()`.
 *   - Examples: `#sqrt(x)`, `#memcpy(dst, src, n)`, `#clz(flags)`
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '(' after intrinsic name: reports error, returns UnknownExprAST.
 *   - Missing type argument for type intrinsic: reports error.
 *   - Missing expression argument for value intrinsic: reports error, breaks loop.
 *   - Missing ')' after arguments: consume() reports error.
 *
 * ─── Semantic Validation (Not Parser Responsibility) ──────────────────────
 *   - The intrinsic name must be known to the compiler.
 *   - Argument count and types must match the intrinsic’s signature.
 *   - Type intrinsics may only appear in constant expressions.
 *   - Memory intrinsics (#memcpy, #memset, #memmove) require raw pointer arguments.
 */
ExprPtr Parser::parseIntrinsicCallExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::HASH, "expected '#'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected intrinsic name after '#'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc = loc;
    node->intrinsicName = pool_.intern(ts_.advance().value);

    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' after intrinsic name");
        return arena_.make<UnknownExprAST>();
    }
    ts_.advance();  // consume '('

    std::string intrinsicStr = std::string(pool_.lookup(node->intrinsicName));
    bool isTypeIntrinsic = (intrinsicStr == "sizeof" || intrinsicStr == "alignof");

    if (isTypeIntrinsic) {
        // Type intrinsics: #sizeof(T), #alignof(T)
        if (ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2005, "expected type argument");
        } else {
            TypePtr typeArg = parseType();
            if (!typeArg) errorAt(DiagCode::E2005, "invalid type argument");
            else node->typeArg = std::move(typeArg);
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after type argument");
    } else {
        // Value intrinsics: #name(expr, expr, ...)
        std::vector<ExprPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            size_t savedPos = ts_.getPos();
            ExprPtr arg = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E2008, "expected argument expression");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            args.push_back(std::move(arg));
            if (ts_.check(TokenType::RPAREN)) break;
            if (!ts_.match(TokenType::COMMA)) {
                errorAt(DiagCode::E2001, "expected ',' or ')' in intrinsic argument list");
                break;
            }
        }
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        node->args = builder.build();
        ts_.consume(TokenType::RPAREN, "expected ')' to close intrinsic call");
    }

    return node;
}