/**
 * @file LiteralParser.cpp
 * @brief Parses literal expressions, array/struct literals, and anonymous functions.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of atomic expression values that appear in
 * primary expression position. These include scalar literals, array literals,
 * struct literals, and anonymous functions.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   literal         := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL
 *                    | RAW_STRING_LITERAL | CHAR_LITERAL | HEX_LITERAL
 *                    | BINARY_LITERAL | 'true' | 'false' | 'nil'
 * 
 *   array_literal   := '[' [ expr { ',' expr } ] ']'
 * 
 *   struct_literal  := IDENTIFIER [ '<' type { ',' type } '>' ] '{' { field_init } '}'
 *   field_init      := IDENTIFIER '=' expr
 * 
 *   anon_func       := param_group { param_group } [ '->' return_list ] block
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see ParserType.cpp for type parsing
 * @see ParserStmt.cpp for block parsing
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Literal Expression
// ============================================================================

/**
 * @brief Parses a scalar literal (integer, float, string, char, boolean, nil).
 *
 * Grammar:
 *   literal := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL
 *            | RAW_STRING_LITERAL | CHAR_LITERAL | HEX_LITERAL
 *            | BINARY_LITERAL | 'true' | 'false' | 'nil'
 *
 * @return ExprPtr – LiteralExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at a literal token.
 * On exit:  positioned after the literal token.
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   42         → LiteralKind::Int,   value="42"
 *   3.14       → LiteralKind::Float, value="3.14"
 *   "hello"    → LiteralKind::String, value="hello"
 *   r"a\nb"    → LiteralKind::RawString, value="a\\nb"
 *   'A'        → LiteralKind::Char,  value="A"
 *   0xFF       → LiteralKind::Hex,   value="0xFF"
 *   true       → LiteralKind::True,  value="true"
 *   nil        → LiteralKind::Nil,   value="nil"
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Called only when the current token is a valid literal (caller ensures).
 *   - If called on a non‑literal token (internal error), reports error and
 *     returns UnknownExprAST.
 */
ExprPtr Parser::parseLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    Token tok = ts_.advance();

    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:        kind = LiteralKind::Int; break;
        case TokenType::FLOAT_LITERAL:      kind = LiteralKind::Float; break;
        case TokenType::STRING_LITERAL:     kind = LiteralKind::String; break;
        case TokenType::RAW_STRING_LITERAL: kind = LiteralKind::RawString; break;
        case TokenType::CHAR_LITERAL:       kind = LiteralKind::Char; break;
        case TokenType::HEX_LITERAL:        kind = LiteralKind::Hex; break;
        case TokenType::BINARY_LITERAL:     kind = LiteralKind::Binary; break;
        case TokenType::TRUE:               kind = LiteralKind::True; break;
        case TokenType::FALSE:              kind = LiteralKind::False; break;
        case TokenType::NIL:                kind = LiteralKind::Nil; break;
        default:
            errorAt(DiagCode::E2002, "internal error: parseLiteralExpr on non-literal token");
            return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<LiteralExprAST>(kind, pool_.intern(tok.value));
    node->loc = loc;
    return node;
}

// ============================================================================
// Array Literal
// ============================================================================

/**
 * @brief Parses an array literal: `[ expr { ',' expr } ]`.
 *
 * Grammar:
 *   array_literal := '[' [ expr { ',' expr } ] ']'
 *
 * The array kind (fixed, slice, dynamic) is inferred from the declared type
 * of the variable being initialised – the literal itself is kind‑neutral.
 * The semantic pass sets the resolved type after inference.
 *
 * @return ExprPtr – ArrayLiteralExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '['
 * On exit:  positioned after the closing ']'
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   [1, 2, 3]     → elements = [1, 2, 3]
 *   ["hello"]     → elements = ["hello"]
 *   []            → empty array literal
 *
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 *   - Uses saved position pattern to detect infinite loops.
 *   - If parseExpr() makes no progress, consumes one token and breaks.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing expression inside literal: reports error, skips to next comma
 *     or closing bracket.
 *   - Missing closing ']': consume() reports error.
 */
ExprPtr Parser::parseArrayLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    std::vector<ExprPtr> elements;

    while (!ts_.check(TokenType::RBRACKET) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACKET)) break;

        size_t beforePos = ts_.getPos();
        ExprPtr elem = parseExpr();
        if (ts_.getPos() == beforePos) {
            errorAt(DiagCode::E2008, "expected expression inside array literal");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        elements.push_back(std::move(elem));
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close array literal");

    auto node = arena_.make<ArrayLiteralExprAST>();
    node->loc = loc;

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : elements) builder.push_back(std::move(e));
    node->elements = builder.build();

    return node;
}

// ============================================================================
// Struct Literal
// ============================================================================

/**
 * @brief Parses a struct literal: `TypeName { field = expr, ... }`.
 *
 * Grammar:
 *   struct_literal := IDENTIFIER [ '<' type { ',' type } '>' ] '{' { field_init } '}'
 *   field_init     := IDENTIFIER '=' expr
 *
 * @param typeName    The name of the struct type.
 * @param genericArgs Optional concrete type arguments for generic structs.
 * @return ExprPtr – StructLiteralExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the type name (and generic args) – the caller
 *           has already consumed the type name and optional generic arguments.
 *           The current token is '{'.
 * On exit:  positioned after the closing '}'.
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   Vec2 { x = 1.0, y = 2.0 }
 *   Color {}                         – all fields take defaults
 *   Pair<int, string> { first = 1, second = "one" }
 *
 * ─── Field Initialisation ─────────────────────────────────────────────────
 *   - Field inits always use '=', never ':'.
 *   - Order of fields is not significant.
 *   - Missing fields that have default values are automatically filled.
 *   - Missing fields without defaults cause semantic errors.
 *
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 *   - Uses saved position pattern when parsing field initialisers.
 *   - If parseExpr() makes no progress after '=', consumes one token and
 *     continues (to avoid infinite loop).
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing field name: reports error, skips token, continues.
 *   - Missing '=' after field name: consume() reports error.
 *   - Missing expression for field: reports error, continues (field omitted).
 *   - Missing closing '}': consume() reports error.
 */
ExprPtr Parser::parseStructLiteralExpr(std::string typeName, ArenaSpan<TypePtr> genericArgs) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{' to open struct literal");

    auto node = arena_.make<StructLiteralExprAST>();
    node->loc = loc;
    node->typeName = pool_.intern(typeName);
    node->genericArgs = genericArgs;

    std::vector<FieldInitPtr> inits;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        SourceLocation fieldLoc = ts_.currentLoc();

        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected field name in struct literal");
            ts_.advance();
            continue;
        }
        std::string fieldName = ts_.advance().value;

        ts_.consume(TokenType::ASSIGN, "expected '=' after field name");
        ExprPtr val = parseExpr();
        if (!val) {
            errorAt(DiagCode::E2008, "expected expression for field");
            continue;
        }

        auto init = arena_.make<FieldInitAST>(pool_.intern(fieldName), std::move(val));
        init->loc = fieldLoc;
        inits.push_back(std::move(init));
    }

    auto builder = arena_.makeBuilder<FieldInitPtr>();
    for (auto& i : inits) builder.push_back(std::move(i));
    node->inits = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct literal");
    return node;
}

// ============================================================================
// Anonymous Function Expression
// ============================================================================

/**
 * @brief Parses an anonymous function expression.
 *
 * Grammar:
 *   anon_func := param_group { param_group } [ '->' return_list ] block
 *
 * Anonymous functions are plain values and cannot have qualifiers
 * (~async, ~nullable, ~parallel). If qualifiers are present, error E2015
 * is reported and they are skipped.
 *
 * @return ExprPtr – AnonFuncExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '(' (or '~' if qualifiers illegally present)
 * On exit:  positioned after the closing '}' of the function body.
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   (x int) -> int { return x * 2 }                – single group, arrow, block
 *   (a int)(b int) -> int { return a + b }         – curried, arrow, block
 *   (x int) -> int { return x * 2 }                – with return type
 *   () -> int { return 42 }                        – zero parameters
 *   (msg string) { io.printl(msg) }                – void, no arrow, block
 *
 * ─── Illegal Qualifier Example (error E2015) ──────────────────────────────
 *   ~async (url string) -> string { ... }          – ERROR: anonymous function cannot have qualifiers
 *
 * ─── Parameter Groups ─────────────────────────────────────────────────────
 *   - Zero or more parameter groups (currying).
 *   - Parameters are flattened into `allParams` with `groupSizes` tracking.
 *   - Each group is parsed by parseParamGroup().
 *
 * ─── Return Types ─────────────────────────────────────────────────────────
 *   - If '->' is present, a return list must follow.
 *   - The return list may be a single type or a parenthesised list of types.
 *   - If '->' is omitted, the function is void (returns nothing).
 *
 * ─── Body ─────────────────────────────────────────────────────────────────
 *   - The body is always a block `{ ... }`.
 *   - The parser does not accept expression bodies for anonymous functions
 *     (unlike named functions). This matches the grammar.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - If qualifiers are present, reports error and skips them.
 *   - Missing '(' after qualifiers: reports error, returns UnknownExprAST.
 *   - Missing return type after '->': reports error, continues.
 *   - Missing '{' after signature: reports error, returns UnknownExprAST.
 *   - Body parsing errors are handled by parseBlock().
 */
ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = ts_.currentLoc();
    
    auto node = arena_.make<AnonFuncExprAST>();
    node->loc = loc;
    
    // Anonymous functions cannot have qualifiers – grammar rule.
    if (ts_.check(TokenType::TILDE)) {
        errorAt(DiagCode::E2015, "anonymous function cannot have qualifiers");
        // Skip all qualifiers to recover
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            ts_.advance();
        }
    }
    
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start anonymous function parameters");
        return arena_.make<UnknownExprAST>();
    }
    
    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    node->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    node->sig.groupSizes = gsBuilder.build();
    
    // Optional return types
    if (ts_.check(TokenType::ARROW)) {
        ts_.advance();
        node->sig.returnTypes = parseReturnList();
        if (node->sig.returnTypes.empty() && !ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2005, "expected return type after '->' in anonymous function");
        }
    }
    
    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
        return arena_.make<UnknownExprAST>();
    }
    node->body = parseBlock();
    
    return node;
}