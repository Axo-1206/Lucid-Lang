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
 * ─── Anonymous Function Parsing ───────────────────────────────────────────
 * 
 * Example: `(a int)(b int) -> int { return a + b }` becomes:
 *   AnonFuncExprAST
 *     └── funcType: FuncTypeAST
 *           ├── params: [a]
 *           └── returnTypes: [FuncTypeAST
 *                 ├── params: [b]
 *                 └── returnTypes: [int]]
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see ParserType.cpp for type parsing
 * @see ParserStmt.cpp for block parsing
 */

#include "ast/BaseAST.hpp"
#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Literal Expression
// ============================================================================

/**
 * @brief Parses a scalar literal expression.
 * 
 * Grammar:
 *   literal := INT_LITERAL | FLOAT_LITERAL | STRING_LITERAL
 *            | RAW_STRING_LITERAL | CHAR_LITERAL | HEX_LITERAL
 *            | BINARY_LITERAL | 'true' | 'false' | 'nil'
 * 
 * @return ExprPtr – LiteralExprAST node, or UnknownExprAST on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at a literal token
 * On exit:  positioned after the literal token
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * Called only when a literal token is expected; reports internal error if
 * the token is not a literal type.
 */
ExprPtr Parser::parseLiteralExpr() {
    LUC_LOG_EXPR_EXTREME("parseLiteralExpr: entering");
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
            LUC_LOG_EXPR("parseLiteralExpr: ERROR - non-literal token");
            errorAt(DiagCode::E1002, "internal error: parseLiteralExpr on non-literal token");
            return arena_.make<UnknownExprAST>();
    }

    LUC_LOG_EXPR_EXTREME("parseLiteralExpr: value = '" << tok.value << "', kind = " << static_cast<int>(kind));
    auto node = arena_.make<LiteralExprAST>(kind, pool_.intern(tok.value));
    node->loc = loc;
    return node;
}

// ============================================================================
// Array Literal
// ============================================================================

/**
 * @brief Parses an array literal expression.
 * 
 * Grammar:
 *   array_literal := '[' [ expr { ',' expr } ] ']'
 * 
 * Examples:
 *   [1, 2, 3]
 *   ["hello", "world"]
 *   []  – empty array literal
 * 
 * @return ExprPtr – ArrayLiteralExprAST node.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '['
 * On exit:  positioned after the closing ']'
 * 
 * ─── Loop Safety ───────────────────────────────────────────────────────────
 * Uses saved position pattern to detect progress failures.
 * 
 * ─── Note ──────────────────────────────────────────────────────────────────
 * The array kind (fixed/slice/dynamic) is inferred from the declared type
 * of the variable being initialised – the literal itself is kind-neutral.
 */
ExprPtr Parser::parseArrayLiteralExpr() {
    LUC_LOG_EXPR_VERBOSE("parseArrayLiteralExpr: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    std::vector<ExprPtr> elements;
    int elemCount = 0;

    while (!ts_.check(TokenType::RBRACKET) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACKET)) break;

        size_t beforePos = ts_.getPos();
        ExprPtr elem = parseExpr();
        if (ts_.getPos() == beforePos) {
            LUC_LOG_EXPR("parseArrayLiteralExpr: ERROR - expected expression inside array literal");
            errorAt(DiagCode::E1008, "expected expression inside array literal");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        elemCount++;
        LUC_LOG_EXPR_EXTREME("parseArrayLiteralExpr: element #" << elemCount);
        elements.push_back(elem);
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close array literal");

    auto node = arena_.make<ArrayLiteralExprAST>();
    node->loc = loc;

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : elements) builder.push_back(e);
    node->elements = builder.build();

    LUC_LOG_EXPR_VERBOSE("parseArrayLiteralExpr: " << elemCount << " element(s)");
    return node;
}

// ============================================================================
// Struct Literal
// ============================================================================

/**
 * @brief Parses a struct literal expression.
 * 
 * Grammar:
 *   struct_literal := IDENTIFIER [ '<' type { ',' type } '>' ] '{' { field_init } '}'
 *   field_init     := IDENTIFIER '=' expr
 * 
 * Examples:
 *   Vec2 { x = 1.0, y = 2.0 }
 *   Color {}  – all fields take their defaults
 *   Pair<int, string> { first = 1, second = "one" }
 * 
 * @param typeName The struct type name (already parsed)
 * @param genericArgs Generic arguments for the struct (already parsed)
 * @return ExprPtr – StructLiteralExprAST node.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the type name and optional generic args
 * On exit:  positioned after the closing '}'
 * 
 * ─── Field Initializers ────────────────────────────────────────────────────
 * Field inits always use '=', never ':'. The ':' only appears inside match
 * struct patterns (handled by StructPatternAST).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '{' after type: error, returns UnknownExprAST
 * - Missing field name: reports error, breaks loop
 * - Missing '=' after field name: reports error, continues
 * - Missing '}' at end: consume() reports error
 */
ExprPtr Parser::parseStructLiteralExpr(InternedString typeName, ArenaSpan<TypePtr> genericArgs) {
    LUC_LOG_EXPR_VERBOSE("parseStructLiteralExpr: type = " << pool_.lookup(typeName) 
                         << ", generic args = " << genericArgs.size()
                         << " at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    auto node = arena_.make<StructLiteralExprAST>();
    node->loc = ts_.currentLoc();
    node->typeName = typeName;
    node->genericArgs = genericArgs;
    
    ts_.consume(TokenType::LBRACE, "expected '{' to start struct literal");
    
    // Parse field initializers
    std::vector<FieldInitPtr> inits;
    int initCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        if (ts_.check(TokenType::IDENTIFIER)) {
            SourceLocation fieldLoc = ts_.currentLoc();
            Token fieldTok = ts_.advance();
            
            if (!ts_.match(TokenType::ASSIGN)) {
                errorAt(DiagCode::E1001, "expected '=' in field initializer for '" + fieldTok.value + "'");
                // Skip to recover
                while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RBRACE)) {
                    ts_.advance();
                }
                continue;
            }
            
            ExprPtr value = parseExpr();
            if (!value) {
                errorAt(DiagCode::E1008, "expected expression in field initializer");
                continue;
            }
            
            initCount++;
            LUC_LOG_EXPR_EXTREME("parseStructLiteralExpr: field #" << initCount 
                                 << " = " << fieldTok.value);
            
            auto init = arena_.make<FieldInitAST>();
            init->loc = fieldLoc;
            init->name = pool_.intern(fieldTok.value);
            init->value = value;
            inits.push_back(init);
            
            ts_.match(TokenType::COMMA); // Optional trailing comma
        } else {
            errorAt(DiagCode::E1003, "expected field name in struct literal");
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<FieldInitPtr>();
    for (auto& init : inits) builder.push_back(init);
    node->inits = builder.build();
    
    ts_.consume(TokenType::RBRACE, "expected '}' to close struct literal");
    
    LUC_LOG_EXPR_VERBOSE("parseStructLiteralExpr: " << initCount << " field initializer(s)");
    return node;
}

// ============================================================================
// Anonymous Function Expression
// ============================================================================

/**
 * @brief Parses an anonymous function expression (closure/lambda).
 * 
 * Grammar:
 *   anon_func := param_group { param_group } [ '->' return_list ] block
 *   param_group := '(' [ param_list ] ')'
 * 
 * Examples:
 *   (x int) int { return x * 2 }
 *   (a int)(b int) -> int { return a + b }   – curried
 *   () { io.printl("no params") }
 *   (x int) -> string { return "value: " + string(x) }
 * 
 * ─── AST Construction (New Design) ────────────────────────────────────────
 * 
 * Multiple parameter groups desugar to nested FuncTypeAST nodes:
 * 
 *   (a int)(b int) -> int { ... }
 * 
 * Becomes:
 *   AnonFuncExprAST
 *     └── funcType: FuncTypeAST
 *           ├── params: [ParamAST(name="a", type=Int)]
 *           └── returnTypes: [FuncTypeAST
 *                 ├── params: [ParamAST(name="b", type=Int)]
 *                 └── returnTypes: [Int]]
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first '(' (or '~' but that's an error)
 * On exit:  positioned after the function body block
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Qualifiers (e.g., ~async) are illegal: reports error, skips them
 * - Missing '(' for parameters: reports error, returns UnknownExprAST
 * - Missing ')' after parameter list: consume() reports error
 * - Missing '{' for body: reports error, returns UnknownExprAST
 * - Missing return type after '->': parseReturnList() reports error
 * 
 * ─── Important Rules ──────────────────────────────────────────────────────
 * - Anonymous functions cannot have qualifiers (~async, ~nullable, ~parallel)
 * - Body is always a BlockStmtAST (curly braces required)
 * - Parameters must have explicit type annotations
 * - Return type is optional (void inferred if omitted)
 */
ExprPtr Parser::parseAnonFuncExpr() {
    LUC_LOG_EXPR_VERBOSE("parseAnonFuncExpr: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    
    auto anonFunc = arena_.make<AnonFuncExprAST>();
    anonFunc->loc = loc;
    
    // Anonymous functions cannot have qualifiers – grammar rule.
    if (ts_.check(TokenType::TILDE)) {
        LUC_LOG_EXPR("parseAnonFuncExpr: ERROR - anonymous function cannot have qualifiers");
        errorAt(DiagCode::E1006, "anonymous function cannot have qualifiers");
        // Skip all qualifiers to recover
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E1003, "expected qualifier name after '~'");
                break;
            }
            ts_.advance();
        }
    }
    
    // First parameter group is required
    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_EXPR("parseAnonFuncExpr: ERROR - expected '(' for parameters");
        errorAt(DiagCode::E1001, "expected '(' to start anonymous function parameters");
        return arena_.make<UnknownExprAST>();
    }
    
    // Parse first parameter group
    LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: parsing first parameter group");
    ts_.consume(TokenType::LPAREN, "expected '(' to start parameter group");
    std::vector<ParamPtr> firstGroup = parseParamList();
    ts_.consume(TokenType::RPAREN, "expected ')' to close parameter group");
    
    // Create the root FuncTypeAST for the first group
    FuncTypeAST* currentFuncType = arena_.make<FuncTypeAST>();
    currentFuncType->loc = loc;
    
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto* p : firstGroup) paramsBuilder.push_back(p);
    currentFuncType->params = paramsBuilder.build();
    
    // Parse additional parameter groups (currying)
    // Each additional group becomes a nested FuncTypeAST in the return chain
    FuncTypeAST* rootFuncType = currentFuncType;
    
    while (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: parsing additional parameter group for currying");
        
        auto nextFuncType = arena_.make<FuncTypeAST>();
        nextFuncType->loc = ts_.currentLoc();
        
        ts_.consume(TokenType::LPAREN, "expected '(' to start parameter group");
        std::vector<ParamPtr> groupParams = parseParamList();
        ts_.consume(TokenType::RPAREN, "expected ')' to close parameter group");
        
        auto nextParamsBuilder = arena_.makeBuilder<ParamPtr>();
        for (auto* p : groupParams) nextParamsBuilder.push_back(p);
        nextFuncType->params = nextParamsBuilder.build();
        
        // Link: currentFuncType's return type becomes nextFuncType
        auto retBuilder = arena_.makeBuilder<TypePtr>();
        retBuilder.push_back(nextFuncType);
        currentFuncType->returnTypes = retBuilder.build();
        
        currentFuncType = nextFuncType;
    }
    
    LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: parsed " 
                         << firstGroup.size() << " parameters in first group"
                         << (currentFuncType != rootFuncType ? " (curried)" : ""));
    
    // Parse optional return types
    if (ts_.match(TokenType::ARROW)) {
        LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: parsing return types");
        currentFuncType->returnTypes = parseReturnList();
        LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: " << currentFuncType->returnTypes.size() << " return type(s)");
        
        if (currentFuncType->returnTypes.empty() && !ts_.check(TokenType::LBRACE)) {
            LUC_LOG_EXPR("parseAnonFuncExpr: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->' in anonymous function");
        }
    } else {
        LUC_LOG_EXPR_EXTREME("parseAnonFuncExpr: void return type (no '->')");
        // Void function - returnTypes remains empty
    }
    
    // Parse function body (must be a block)
    if (!ts_.check(TokenType::LBRACE)) {
        LUC_LOG_EXPR("parseAnonFuncExpr: ERROR - expected '{' for body");
        errorAt(DiagCode::E1001, "expected '{' to start anonymous function body");
        return arena_.make<UnknownExprAST>();
    }
    
    anonFunc->funcType = rootFuncType;
    anonFunc->body = parseBlock();
    
    LUC_LOG_EXPR_VERBOSE("parseAnonFuncExpr: success");
    return anonFunc;
}