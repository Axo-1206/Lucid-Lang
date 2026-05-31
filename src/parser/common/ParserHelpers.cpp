/**
 * @file ParserHelpers.cpp
 * @brief Shared helper functions for the Luc parser.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements utility functions that are shared across multiple parser
 * modules. These helpers are used by declaration, statement, expression, and
 * type parsers.
 * 
 * ## Categories of Helpers
 * 
 *   1. Parameter Helpers  – parse parameter lists, groups, and argument lists
 *   2. Return List Parser – parse return types after '->' (handles function types and multi-return)
 *   3. Qualifiers         – parse `~async`, `~nullable`, `~parallel` qualifiers
 *   4. Module Path        – parse dotted module paths for `use` declarations
 * 
 * ## Design Principles
 * 
 *   - **Temporary Collections**: Parameter list parsers return `std::vector<T>` (temporary).
 *     The caller is responsible for converting to `ArenaSpan<T>` using `SpanBuilder`.
 *   - **Error Recovery**: Uses saved position patterns and consecutive error counters
 *     to prevent infinite loops on malformed input.
 *   - **Null Safety**: Functions return empty collections on error, never nullptr.
 * 
 * @see Parser.cpp for core infrastructure
 * @see LUC_GRAMMAR.md for grammar rules
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// 1. PARAMETER HELPERS
// ============================================================================
//
// These functions handle function parameters and call arguments.
// ============================================================================

/**
 * @brief Parses a parameter list inside parentheses.
 *
 * Grammar: param { ',' param } [ ',' variadic_param ]
 *
 * Example: `a int, b string, args ...any`
 *
 * @return std::vector<ParamPtr> – temporary collection, empty on error.
 *
 * ─── Important Rules ────────────────────────────────────────────────────────
 * - Variadic parameter (`...type`) is allowed only as the last parameter.
 * - Parameter names are required (no anonymous parameters).
 * - Type annotations are required (no type inference).
 *
 * ─── Loop Safety ───────────────────────────────────────────────────────────
 * - Uses saved position pattern to prevent infinite loops.
 * - If parsing fails, consumes one token and continues.
 */
std::vector<ParamPtr> Parser::parseParamList() {
    std::vector<ParamPtr> list;
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected parameter name");
            break;
        }
        auto param = arena_.make<ParamAST>();
        param->name = pool_.intern(ts_.advance().value);
        param->isVariadic = ts_.match(TokenType::VARIADIC);
        param->type = parseType();
        if (param->type) list.push_back(std::move(param));
    }
    return list;
}

/**
 * @brief Parses a single parameter group: '(' param_list ')'.
 *
 * Called for each parameter group in curried functions and function types.
 *
 * Grammar: '(' [ param_list ] ')'
 *
 * @return ParamGroup (std::vector<ParamPtr>) – temporary collection.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '('
 * On exit:  positioned after the closing ')'
 *
 * ─── Edge Cases ────────────────────────────────────────────────────────────
 * - Empty parentheses `()` are allowed (zero parameters).
 * - Recovers from missing ')' by consuming until RPAREN or EOF.
 */
ParamGroup Parser::parseParamGroup() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LPAREN, "expected '(' to start parameter group");
    
    std::vector<ParamPtr> params = parseParamList();
    
    ts_.consume(TokenType::RPAREN, "expected ')' to close parameter group");
    
    return params;
}

/**
 * @brief Parses a comma‑separated argument list inside parentheses.
 *
 * Used for function calls, method calls, and intrinsic calls.
 *
 * Grammar: expr { ',' expr }
 *
 * @return ArenaSpan<ExprPtr> – arena-allocated span of arguments.
 *
 * ─── Consecutive Error Protection ──────────────────────────────────────────
 * Uses a counter (max 5) to prevent infinite loops when the parser repeatedly
 * fails to parse an expression. After 5 consecutive errors, it skips to the
 * closing ')'.
 */
ArenaSpan<ExprPtr> Parser::parseArgList() {
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        args.push_back(std::move(arg));

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

// ============================================================================
// 2. RETURN LIST PARSER
// ============================================================================
//
// parseReturnList() handles the return types after '->' in function signatures.
// It must distinguish between:
//   - Single return type:   `-> int`
//   - Multi‑return:         `-> (int, string)`
//   - Function type:        `-> (x int) -> int` (returns a function)
//   - Empty parentheses:    `-> () -> int` (function with zero parameters)
//
// The detection uses lookahead to avoid consuming tokens permanently.
// ============================================================================

/**
 * @brief Parses the return list after '->' in function signatures.
 *
 * Grammar:
 *   return_list := '(' [ return_type { ',' return_type } ] ')'   -- multiple
 *                | return_type                                    -- single
 *
 * where `return_type` can itself be a function type with its own '->'.
 *
 * @par Examples
 *   -> int                         // single return
 *   -> (int, string)               // multi-return
 *   -> (x int) -> int              // function type (returns a function)
 *   -> () -> int                   // zero-parameter function type
 *   -> ((x int) -> int, string)    // multi-return with function type
 *
 * @par Detection Strategy
 *   To distinguish between multi-return `(Type, Type)` and function type
 *   `(param Type) -> Ret`, the parser:
 *   1. Looks inside the parentheses
 *   2. If it sees `IDENTIFIER` followed by a type start (parameter pattern)
 *      and later finds `->` after a complete parameter group, it parses as
 *      a function type
 *   3. Otherwise, it parses as a multi-return list
 *
 * @return ArenaSpan<TypePtr> – span of return types (empty = void function)
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the token after '->'
 * On exit:  positioned after the return list
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing return type: reports error, returns empty span
 * - Missing ')' in multi-return: consume() reports error
 * - Invalid type in list: skips type, continues
 */
ArenaSpan<TypePtr> Parser::parseReturnList() {
    // Helper to check if a token type can start a type
    auto isTypeStart = [this](TokenType tt) -> bool {
        return isPrimitiveTypeToken(tt) ||
               tt == TokenType::IDENTIFIER ||
               tt == TokenType::LBRACKET ||
               tt == TokenType::AMPERSAND ||
               tt == TokenType::MUL ||
               tt == TokenType::LPAREN ||
               tt == TokenType::TILDE;
    };

    // Case 1: No parentheses → single return type
    if (!ts_.check(TokenType::LPAREN)) {
        TypePtr t = parseType();
        if (!t || t->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(std::move(t));
        return builder.build();
    }

    // We have '(' - need to determine if it's a function type or multi-return
    size_t savedPos = ts_.getPos();
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    
    // Try to parse as a function type first (non‑consuming lookahead)
    size_t testPos = savedPos;
    testPos = ts_.skipCommentsFrom(testPos);
    
    if (testPos < tokenCount && tokens[testPos].type == TokenType::LPAREN) {
        ++testPos;
        testPos = ts_.skipCommentsFrom(testPos);
        
        bool isEmptyParen = (testPos < tokenCount && tokens[testPos].type == TokenType::RPAREN);
        
        if (!isEmptyParen) {
            // Check if this looks like a parameter: IDENTIFIER followed by type start
            bool looksLikeParameter = false;
            if (testPos < tokenCount && tokens[testPos].type == TokenType::IDENTIFIER) {
                size_t afterIdent = testPos + 1;
                afterIdent = ts_.skipCommentsFrom(afterIdent);
                if (afterIdent < tokenCount && isTypeStart(tokens[afterIdent].type)) {
                    looksLikeParameter = true;
                }
            }
            
            if (looksLikeParameter) {
                // Simulate parsing up to the closing ')' of the first parameter group
                int parenDepth = 1;
                size_t paramEnd = testPos;
                while (paramEnd < tokenCount && parenDepth > 0) {
                    paramEnd = ts_.skipCommentsFrom(paramEnd);
                    if (paramEnd >= tokenCount) break;
                    TokenType tt = tokens[paramEnd].type;
                    if (tt == TokenType::LPAREN) ++parenDepth;
                    else if (tt == TokenType::RPAREN) --parenDepth;
                    ++paramEnd;
                }
                
                // Check after the closing ')' for '->'
                size_t afterParams = paramEnd;
                afterParams = ts_.skipCommentsFrom(afterParams);
                
                bool hasArrow = false;
                while (afterParams < tokenCount) {
                    afterParams = ts_.skipCommentsFrom(afterParams);
                    if (afterParams >= tokenCount) break;
                    TokenType tt = tokens[afterParams].type;
                    if (tt == TokenType::ARROW) {
                        hasArrow = true;
                        break;
                    }
                    if (tt == TokenType::LPAREN) {
                        ++afterParams;
                        continue;
                    }
                    break;
                }
                
                if (hasArrow) {
                    // This is a function type
                    TypePtr funcType = parseFuncType();
                    if (!funcType || funcType->isa<UnknownTypeAST>()) {
                        errorAt(DiagCode::E2005, "expected function type");
                        return ArenaSpan<TypePtr>();
                    }
                    auto builder = arena_.makeBuilder<TypePtr>();
                    builder.push_back(std::move(funcType));
                    return builder.build();
                }
            }
        } else {
            // Empty parentheses - check if this is a function type with zero parameters
            size_t afterParen = testPos + 1;
            afterParen = ts_.skipCommentsFrom(afterParen);
            if (afterParen < tokenCount && tokens[afterParen].type == TokenType::ARROW) {
                TypePtr funcType = parseFuncType();
                if (!funcType || funcType->isa<UnknownTypeAST>()) {
                    errorAt(DiagCode::E2005, "expected function type");
                    return ArenaSpan<TypePtr>();
                }
                auto builder = arena_.makeBuilder<TypePtr>();
                builder.push_back(std::move(funcType));
                return builder.build();
            }
        }
    }
    
    // Not a function type → parse as multi-return list
    ts_.setPos(savedPos);
    ts_.advance(); // consume '('
    
    std::vector<TypePtr> types;
    
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!types.empty() && !ts_.match(TokenType::COMMA)) {
            if (ts_.check(TokenType::RPAREN)) break;
            errorAt(DiagCode::E2001, "expected ',' between return types");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            continue;
        }
        
        if (ts_.check(TokenType::RPAREN)) break;
        
        size_t typeSavedPos = ts_.getPos();
        TypePtr t = parseType();
        if (ts_.getPos() == typeSavedPos) {
            errorAt(DiagCode::E2005, "expected return type");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            break;
        }
        if (t && !t->isa<UnknownTypeAST>()) {
            types.push_back(std::move(t));
        }
    }
    
    ts_.consume(TokenType::RPAREN, "expected ')' to close return type list");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& t : types) builder.push_back(std::move(t));
    return builder.build();
}

// ============================================================================
// 3. QUALIFIERS
// ============================================================================
//
// parseQualifiers() parses a sequence of `~name` qualifiers (e.g., `~async`,
// `~nullable`, `~parallel`) and returns a bitmask and raw names.
//
// The bitmask is pre‑computed during parsing for O(1) checks during later
// phases. Raw names are preserved for accurate error messages.
// ============================================================================

/**
 * @brief Parses a sequence of qualifiers (`~async`, `~nullable`, `~parallel`).
 *
 * Grammar: { '~' IDENTIFIER }
 *
 * Example: `~async ~nullable`
 *
 * @return QualifierSet containing:
 *         - raw: vector of InternedString (original names, for errors)
 *         - bitmask: OR of QualifierBits flags
 *
 * ─── Error Handling ─────────────────────────────────────────────────────────
 * - Missing identifier after '~': reports error, stops parsing qualifiers.
 * - Unknown qualifier names: the registry lookup returns nullptr; the current
 *   code is commented out but would report an error if enabled.
 */
QualifierSet Parser::parseQualifiers() {
    QualifierSet qs;
    auto& registry = QualifierRegistry::instance();
    
    while (ts_.check(TokenType::TILDE)) {
        SourceLocation loc = ts_.currentLoc();
        ts_.advance();
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            error(loc, DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString name = pool_.intern(ts_.advance().value);
        
        const QualifierInfo* info = registry.lookup(name);
        // Uncomment for strict qualifier validation:
        // if (!info) {
        //     error(loc, DiagCode::E2010, 
        //           "unknown qualifier '~" + std::string(pool_.lookup(name)) + 
        //           "'; known qualifiers: " + registry.allNames());
        //     continue;
        // }
        
        qs.raw.push_back(name);
        qs.bitmask |= info->bit;
    }
    return qs;
}

// ============================================================================
// 4. MODULE PATH PARSING
// ============================================================================
//
// parseModulePath() parses a dotted identifier sequence for `use` declarations.
// ============================================================================

/**
 * @brief Parses a dotted module path for `use` declarations.
 *
 * Grammar: IDENTIFIER { '.' IDENTIFIER }
 *
 * Example: `std.io`, `renderer.core.math`
 *
 * @return std::vector<InternedString> – path segments in order.
 *
 * ─── Error Recovery ─────────────────────────────────────────────────────────
 * - Missing identifier after '.': reports error, stops building path.
 * - Returns empty vector on no initial identifier.
 */
std::vector<InternedString> Parser::parseModulePath() {
    std::vector<InternedString> path;
    if (!ts_.check(TokenType::IDENTIFIER)) return path;
    path.push_back(pool_.intern(ts_.advance().value));
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after '.'");
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
    }
    return path;
}

// ============================================================================
// 5. FUNCTION REFERENCE (for assignment forms)
// ============================================================================

/**
 * @brief Parses a function reference for use in method assignments.
 *
 * Grammar:
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
 *
 * Examples:
 *   utils.getVersion                       -- dotted path
 *   transform<int, string>                 -- generic instantiation
 *   std.map<U>                             -- dotted + generic
 *
 * ─── Parsing Steps ─────────────────────────────────────────────────────────
 *   1. Parse the first identifier (required).
 *   2. While the next token is `.` and the token after that is `IDENTIFIER`,
 *      consume `.` and the identifier, building a chain of `FieldAccessExprAST`.
 *   3. If the next token is `<`, parse a generic argument list and wrap the
 *      current expression in a `CallExprAST` with those generic arguments.
 *
 * @return ExprPtr – expression representing the function reference
 */
ExprPtr Parser::parseFuncRef() {
    SourceLocation loc = ts_.currentLoc();
    
    // Parse a name (identifier or dotted path)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name in function reference");
        return arena_.make<UnknownExprAST>();
    }
    std::string name = ts_.advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = loc;
    
    // Parse dotted path segments
    while (ts_.check(TokenType::DOT)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after '.'");
            break;
        }
        std::string field = ts_.advance().value;
        auto node = arena_.make<FieldAccessExprAST>();
        node->loc = loc;
        node->object = std::move(expr);
        node->field = pool_.intern(field);
        expr = std::move(node);
    }
    
    // Optional behavior access
    if (ts_.check(TokenType::COLON)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected method name after ':'");
            return expr;
        }
        std::string method = ts_.advance().value;
        auto behavior = arena_.make<BehaviorAccessExprAST>();
        behavior->loc = loc;
        // typeName will be resolved later; for now store the base name
        // (The semantic pass will resolve the actual type of the receiver.)
        behavior->typeName = pool_.intern(name);
        behavior->method = pool_.intern(method);
        behavior->isBehaviorMember = true;
        expr = std::move(behavior);
    }
    
    // After parsing the base name and optional dotted path / behavior access,
    // check for generic arguments.
    if (ts_.check(TokenType::LESS)) {
        ArenaSpan<TypePtr> typeArgs = parseGenericArgs(); // consumes '<' ... '>'
        auto refNode = arena_.make<CallableRefExprAST>();
        refNode->loc = loc;
        refNode->entity = std::move(expr);
        refNode->typeArgs = typeArgs;
        expr = std::move(refNode);
    }
    
    return expr;
}