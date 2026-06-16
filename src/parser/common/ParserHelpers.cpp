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
 *   1. Parameter Helpers      – parse parameter lists and argument lists
 *   2. Return List Parser     – parse return types after '->' (handles function types and multi-return)
 *   3. Qualifiers             – parse `~async`, `~nullable`, `~parallel` qualifiers
 *   4. Function Reference     – parse `func_ref` for assignments and references
 *   5. Precedence Helpers     – map tokens to precedence levels and operator enums
 * 
 * ## Design Principles
 * 
 *   - **Temporary Collections**: Parameter list parsers return `std::vector<T>`.
 *     The caller is responsible for converting to `ArenaSpan<T>` using `SpanBuilder`.
 *   - **Error Recovery**: Uses saved position patterns and consecutive error counters
 *     to prevent infinite loops on malformed input.
 *   - **Null Safety**: Functions return empty collections on error, never nullptr.
 * 
 * @see Parser.cpp for core infrastructure
 * @see GRAMMAR.md for grammar rules
 */

#include "ast/BaseAST.hpp"
#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// 1. PARAMETER HELPERS
// ============================================================================

/**
 * @brief Parses a parameter list inside already-consumed parentheses.
 *
 * Grammar:
 *   param_list := param { ',' param } [ ',' variadic_param ]
 *   param := IDENTIFIER type
 *   variadic_param := IDENTIFIER '...' type
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
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first token of first parameter (or ')' for empty)
 * On exit:  positioned at the closing ')' of the parameter list
 *
 * ─── Loop Safety ───────────────────────────────────────────────────────────
 * - Uses saved position pattern to prevent infinite loops.
 * - If parsing fails, consumes one token and continues.
 *
 * @note This function does NOT consume the surrounding parentheses.
 *       The caller is responsible for consuming '(' before calling
 *       and ')' after this function returns.
 */
std::vector<ParamPtr> Parser::parseParamList() {
    LOG_DECL_EXTREME("parseParamList: entering");
    std::vector<ParamPtr> list;
    int paramCount = 0;
    
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseParamList: ERROR - expected parameter name");
            errorAt(DiagCode::E1002, "parameter name", ts_.peek().value);
            break;
        }
        auto param = arena_.make<ParamAST>();
        param->name = pool_.intern(ts_.advance().value);
        
        // Check if we have a variadic parameter
        param->isVariadic = ts_.match(TokenType::VARIADIC);
        
        // Special diagnostic: Check if the next token is ')' or ',' or '->'
        // This means the user forgot the type annotation
        if (ts_.check(TokenType::RPAREN) || ts_.check(TokenType::COMMA) || ts_.check(TokenType::ARROW)) {
            std::string paramName = std::string(pool_.lookup(param->name));
            
            // Use the specific diagnostic code E1025
            errorAt(DiagCode::E1025, paramName, paramName);
            
            // Skip the parameter and continue
            continue;
        }
        
        // Check if we have a type annotation
        if (!looksLikeType()) {
            // Report a helpful error
            std::string msg = "expected type annotation for parameter '" + 
                              std::string(pool_.lookup(param->name)) + 
                              "'. In Luc, every parameter must have an explicit type. "
                              "Example: '(" + std::string(pool_.lookup(param->name)) + " int)'";
            errorAt(DiagCode::E1005, msg);
            // Skip to recover - consume until ',' or ')'
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            continue;
        }
        
        param->type = parseType();
        if (param->type) {
            paramCount++;
            LOG_DECL_EXTREME("parseParamList: parameter #" << paramCount 
                                 << " = " << pool_.lookup(param->name));
            list.push_back(param);
        } else {
            LOG_DECL("parseParamList: ERROR - failed to parse type for parameter");
        }
    }
    
    LOG_DECL_EXTREME("parseParamList: parsed " << paramCount << " parameter(s)");
    return list;
}

/**
 * @brief Parses a comma‑separated argument list inside parentheses.
 *
 * Used for function calls, method calls, and intrinsic calls.
 *
 * Grammar:
 *   arg_list := expr { ',' expr }
 *
 * @return ArenaSpan<ExprPtr> – arena-allocated span of arguments.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first argument (or ')' for empty)
 * On exit:  positioned after the closing ')'
 *
 * ─── Consecutive Error Protection ──────────────────────────────────────────
 * Uses a counter (max 5) to prevent infinite loops when the parser repeatedly
 * fails to parse an expression. After 5 consecutive errors, it skips to the
 * closing ')'.
 */
ArenaSpan<ExprPtr> Parser::parseArgList() {
    LOG_DECL_EXTREME("parseArgList: entering");
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;
    int argCount = 0;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            LOG_DECL("parseArgList: ERROR - too many consecutive errors in argument list");
            errorAt(DiagCode::E1002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            LOG_DECL("parseArgList: ERROR - expected argument expression");
            errorAt(DiagCode::E1008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        argCount++;
        LOG_DECL_EXTREME("parseArgList: argument #" << argCount);
        args.push_back(arg);

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            LOG_DECL("parseArgList: ERROR - expected ',' after argument");
            errorAt(DiagCode::E1001, "expected ',' after argument");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : args) builder.push_back(a);
    
    LOG_DECL_EXTREME("parseArgList: parsed " << argCount << " argument(s)");
    return builder.build();
}

// ============================================================================
// 2. RETURN LIST PARSER
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
    LOG_DECL_EXTREME("parseReturnList: entering");
    
    // Case 1: Single return type (no parentheses)
    if (!ts_.check(TokenType::LPAREN)) {
        LOG_DECL_EXTREME("parseReturnList: single return type (no parentheses)");
        // Use existing looksLikeType() to validate we have a type start
        if (!looksLikeType()) {
            LOG_DECL("parseReturnList: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        TypePtr t = parseType();
        if (!t || t->isa<UnknownTypeAST>()) {
            LOG_DECL("parseReturnList: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(t);
        LOG_DECL_EXTREME("parseReturnList: single return type parsed");
        return builder.build();
    }

    // We have '(' - need to determine if it's a function type or multi-return
    LOG_DECL_EXTREME("parseReturnList: detected '(' - checking if function type or multi-return");
    size_t savedPos = ts_.getPos();
    ts_.consume(TokenType::LPAREN, "expected '(' for return list");

    // Use lookahead to decide
    if (isFunctionTypeAfterParen(ts_.getPos())) {
        LOG_DECL_EXTREME("parseReturnList: this is a function type");
        // This is a function type (the parentheses belong to the function's parameter group)
        ts_.setPos(savedPos);
        TypePtr funcType = parseFuncType();
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            LOG_DECL("parseReturnList: ERROR - expected function type");
            errorAt(DiagCode::E1005, "expected function type");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(funcType);
        LOG_DECL_EXTREME("parseReturnList: function type parsed");
        return builder.build();
    }

    // Not a function type → parse as multi-return list
    LOG_DECL_EXTREME("parseReturnList: multi-return list");
    // We are already positioned after the '(' (from the consume above)
    std::vector<TypePtr> types;
    int typeCount = 0;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!types.empty() && !ts_.match(TokenType::COMMA)) {
            if (ts_.check(TokenType::RPAREN)) break;
            LOG_DECL("parseReturnList: ERROR - expected ',' between return types");
            errorAt(DiagCode::E1001, "expected ',' between return types");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            continue;
        }

        if (ts_.check(TokenType::RPAREN)) break;

        size_t typeSavedPos = ts_.getPos();
        TypePtr t = parseType();
        if (ts_.getPos() == typeSavedPos) {
            LOG_DECL("parseReturnList: ERROR - expected return type");
            errorAt(DiagCode::E1005, "expected return type");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            break;
        }
        if (t && !t->isa<UnknownTypeAST>()) {
            typeCount++;
            LOG_DECL_EXTREME("parseReturnList: return type #" << typeCount);
            types.push_back(t);
        }
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close return type list");

    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& t : types) builder.push_back(t);
    
    LOG_DECL_EXTREME("parseReturnList: parsed " << typeCount << " return type(s)");
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
 * Grammar:
 *   qualifier_list := { '~' IDENTIFIER }
 *
 * Example: `~async ~nullable`
 *
 * @return QualifierSet containing:
 *         - raw: vector of InternedString (original names, for errors)
 *         - bitmask: OR of QualifierBits flags
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first '~' (if any)
 * On exit:  positioned after the last qualifier (or at same position if none)
 *
 * ─── Error Handling ─────────────────────────────────────────────────────────
 * - Missing identifier after '~': reports error (E1003), stops parsing qualifiers.
 * - Unknown qualifier name: reports error (E1016), skips this qualifier.
 * - Duplicate qualifier: reports error (E1018), skips the duplicate.
 */
QualifierSet Parser::parseQualifiers() {
    LOG_DECL_EXTREME("parseQualifiers: entering");
    QualifierSet qs;
    int qualifierCount = 0;
    
    while (ts_.check(TokenType::TILDE)) {
        SourceLocation loc = ts_.currentLoc();
        ts_.advance(); // consume '~'
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseQualifiers: ERROR - expected qualifier name after '~'");
            error(loc, DiagCode::E1003, "expected qualifier name after '~'");
            break;
        }
        InternedString name = pool_.intern(ts_.advance().value);
        std::string_view nameStr = pool_.lookup(name);
        LOG_DECL_EXTREME("parseQualifiers: found qualifier '~" << nameStr << "'");
        
        // Check for duplicate qualifier
        if (std::find(qs.raw.begin(), qs.raw.end(), name) != qs.raw.end()) {
            LOG_DECL("parseQualifiers: ERROR - duplicate qualifier '~" << nameStr << "'");
            error(loc, DiagCode::E1018, 
                  "duplicate qualifier '~" + std::string(nameStr) + "'");
            continue;
        }
        
        const QualifierEntry* entry = qualifier::lookup(name);
        if (!entry) {
            LOG_DECL("parseQualifiers: ERROR - unknown qualifier '~" << nameStr << "'");
            error(loc, DiagCode::E1016, 
                  "unknown qualifier '~" + std::string(nameStr) + 
                  "'; known qualifiers: " + qualifier::allNames());
            continue;
        }
        
        qs.raw.push_back(name);
        qs.bitmask |= entry->bit;
        qualifierCount++;
        LOG_DECL_EXTREME("parseQualifiers: added qualifier (bitmask now 0x" 
                             << std::hex << qs.bitmask << std::dec << ")");
    }
    
    if (qualifierCount > 0) {
        LOG_DECL_EXTREME("parseQualifiers: total " << qualifierCount << " qualifier(s)");
    }
    return qs;
}

// ============================================================================
// 4. FUNCTION REFERENCE (for assignment forms, pipelines, composition)
// ============================================================================
//
// parseFuncRef() parses a function reference for use in:
//   - Method assignments in `impl` blocks
//   - Path entries in `from` blocks
//   - Pipeline steps (`|>`)
//   - Compose operands (`+>`)
//   - Variable initialisers
//
// The grammar for func_ref is shared across all these contexts.
// ============================================================================

/**
 * @brief Parses a function reference for use in declarations and expressions.
 *
 * Grammar (from GRAMMAR.md, Shared Productions):
 *   func_ref := type_reference                    -- int, string, Box<int>
 *             | IDENTIFIER                        -- local or imported name
 *             | IDENTIFIER '.' IDENTIFIER         -- module path: pkg.fn
 *             | expr ':' IDENTIFIER               -- method reference: obj:method
 *             | func_ref generic_args             -- generic instantiation: fn<T>
 *
 *   type_reference := primitive_type              -- int, float, string, etc.
 *                   | IDENTIFIER [ '<' type_list '>' ]  -- named type with optional generics
 *
 * @par Examples
 *   int                                          – primitive type as function (PrimitiveTypeAST)
 *   string                                       – primitive type as function (PrimitiveTypeAST)
 *   Box<int>                                     – generic type as function (NamedTypeAST with genericArgs)
 *   utils.getVersion                             – dotted path (FieldAccessExprAST)
 *   transform<int, string>                       – generic function (IdentifierExprAST with genericArgs)
 *   std.map<U>                                   – dotted + generic (FieldAccessExprAST with genericArgs)
 *   identity                                     – plain identifier (IdentifierExprAST)
 *   list:map                                     – method reference (BehaviorAccessExprAST)
 *   (getValue()):process                         – method reference with complex object
 *
 * @par Important Distinction
 *   This parses a REFERENCE to a function or type, NOT a call.
 *   - PrimitiveTypeAST: primitive type as function value
 *   - NamedTypeAST: named type as function value (may have genericArgs)
 *   - FieldAccessExprAST: module paths and struct fields (may have genericArgs)
 *   - BehaviorAccessExprAST: method references (NO genericArgs - resolved from receiver)
 *   - IdentifierExprAST: plain identifiers (may have genericArgs)
 *
 *   For actual calls, use `parseCallExpr()` which consumes parentheses.
 *
 * @return ExprPtr – expression representing the function reference.
 *
 * ─── Parsing Strategy ─────────────────────────────────────────────────────────
 *   The function parses left-to-right, building the expression incrementally:
 *   1. Parse the leftmost atom (identifier, type reference, or parenthesized expr)
 *   2. While we see '.' or ':', extend the expression
 *   3. After building the base, parse optional generic arguments
 *
 * ─── Generic Arguments ────────────────────────────────────────────────────
 *   - Generic arguments can appear on identifiers and dotted paths
 *   - They CANNOT appear on method references (generics resolved from receiver)
 *   - For type references (e.g., Box<int>), the generics are part of the type
 *     and stored on the NamedTypeAST
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at an IDENTIFIER, primitive type, or '('
 * On exit:  positioned after the optional generic arguments (or after the name)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing identifier: returns `UnknownExprAST`, reports error.
 *   - Missing identifier after '.': reports error, stops building path.
 *   - Missing identifier after ':': reports error, returns nullptr.
 *   - Malformed generic arguments: `parseGenericArgs()` reports error,
 *     returns empty span (still creates the node with empty generic args).
 */
ExprPtr Parser::parseFuncRef() {
    LOG_EXPR_VERBOSE("parseFuncRef: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();

    // Parse the base expression
    ExprPtr expr = nullptr;
    
    // Handle parenthesized expression for method reference object
    if (ts_.check(TokenType::LPAREN)) {
        LOG_EXPR_EXTREME("parseFuncRef: parenthesized object expression");
        ts_.advance(); // consume '('
        expr = parseExpr(false);
        if (!expr) {
            LOG_EXPR("parseFuncRef: ERROR - expected expression in parentheses");
            errorAt(DiagCode::E1008, "expected expression in parentheses");
            return arena_.make<UnknownExprAST>();
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after parenthesized expression");
    }
    // Handle identifier or primitive type name
    else if (ts_.check(TokenType::IDENTIFIER) || ts_.isPrimitiveTypeToken(ts_.peekType())) {
        SourceLocation idenLoc = ts_.currentLoc();
        
        InternedString name;
        if (ts_.check(TokenType::IDENTIFIER)) {
            Token idenTok = ts_.advance();
            name = pool_.intern(idenTok.value);
            LOG_EXPR_EXTREME("parseFuncRef: base identifier = '" << pool_.lookup(name) << "'");
        } else {
            Token typeTok = ts_.advance();
            name = pool_.intern(typeTok.value);
            LOG_EXPR_EXTREME("parseFuncRef: primitive type name = '" << pool_.lookup(name) << "'");
        }
        
        // Check for optional generic arguments
        ArenaSpan<TypePtr> genericArgs;
        if (ts_.check(TokenType::LESS) && looksLikeGenericTypeInstantiation()) {
            LOG_EXPR_EXTREME("parseFuncRef: generic arguments for '" << pool_.lookup(name) << "'");
            ts_.advance(); // consume '<'
            genericArgs = parseGenericArgs();
            LOG_EXPR("parseFuncRef: parsed " << genericArgs.size() 
                         << " generic args for '" << pool_.lookup(name) << "'");
        }
        
        auto node = arena_.make<IdentifierExprAST>(name);
        node->loc = idenLoc;
        node->genericArgs = genericArgs;
        expr = node;
    }
    else {
        LOG_EXPR("parseFuncRef: ERROR - expected identifier or '('");
        errorAt(DiagCode::E1003, "expected function name or '(' for method reference");
        return arena_.make<UnknownExprAST>();
    }

    // Parse postfix operators: '.' and ':' 
    bool isMethodReference = false;
    
    while (!ts_.isAtEnd() && !isMethodReference) {
        // Field access / module path: .identifier
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                LOG_EXPR("parseFuncRef: ERROR - expected identifier after '.'");
                errorAt(DiagCode::E1003, "expected identifier after '.'");
                break;
            }
            SourceLocation fieldLoc = ts_.currentLoc();
            Token fieldTok = ts_.advance();
            LOG_EXPR_EXTREME("parseFuncRef: dotted segment ." << fieldTok.value);
            
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = fieldLoc;
            node->object = expr;
            node->field = pool_.intern(fieldTok.value);
            expr = node;
        }
        // Method reference: :identifier
        else if (ts_.check(TokenType::COLON)) {
            ts_.advance(); // consume ':'
            if (!ts_.check(TokenType::IDENTIFIER)) {
                LOG_EXPR("parseFuncRef: ERROR - expected method name after ':'");
                errorAt(DiagCode::E1003, "expected method name after ':'");
                return nullptr;
            }
            SourceLocation methodLoc = ts_.currentLoc();
            Token methodTok = ts_.advance();
            LOG_EXPR_EXTREME("parseFuncRef: method reference :" << methodTok.value);
            
            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = methodLoc;
            node->object = expr;
            node->method = pool_.intern(methodTok.value);
            expr = node;
            isMethodReference = true;
        }
        else {
            break;
        }
    }

    // Generic arguments after dotted path
    if (!isMethodReference && !expr->isa<BehaviorAccessExprAST>() && ts_.check(TokenType::LESS)) {
        LOG_EXPR_EXTREME("parseFuncRef: parsing generic arguments after path");
        
        ts_.advance(); // consume '<'
        ArenaSpan<TypePtr> typeArgs = parseGenericArgs();
        
        if (auto* ident = expr->as<IdentifierExprAST>()) {
            if (ident->genericArgs.size() > 0) {
                LOG_EXPR("parseFuncRef: WARNING - identifier already has generic args");
            }
            ident->genericArgs = typeArgs;
        } else if (auto* field = expr->as<FieldAccessExprAST>()) {
            field->genericArgs = typeArgs;
        } else {
            LOG_EXPR("parseFuncRef: ERROR - cannot add generic args to node type");
            errorAt(DiagCode::E1006, "generic arguments are only allowed on identifiers and module paths");
        }
    }

    return expr;
}

// ============================================================================
// 5. PRECEDENCE HELPERS
// ============================================================================
//
// These functions map token types to precedence levels and operator enums.
// They are used by the Pratt parser to implement operator precedence.
//
// Precedence levels (higher = tighter binding):
//   Level 12 : '^' (exponentiation, right‑associative)
//   Level 11 : '*', '/', '%'
//   Level 10 : '+', '-'
//   Level 8  : '&&', '||', '~^', '<<', '>>' (bitwise)
//   Level 7  : '==', '!=', '<', '>', '<=', '>=', 'is', '==='
//   Level 6  : 'and'
//   Level 5  : 'or'
//   Level 4  : '??' (null coalesce)
//   Level 3  : '|>' (pipeline)
//   Level 2  : '+>' (composition)
//   Level 1  : '=', '+=', '-=', etc. (assignment)
// ============================================================================

/**
 * @brief Returns the precedence level of an infix operator token.
 *
 * @param t The token type.
 * @return int Precedence level (higher = tighter binding), or PREC_NONE.
 */
int Parser::infixPrec(TokenType t) const {
    int prec;
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            prec = PREC_ASSIGN;
            break;
        case TokenType::COMPOSE:            prec = PREC_COMPOSE; break;
        case TokenType::PIPELINE:           prec = PREC_PIPE; break;
        case TokenType::QUESTION_QUESTION:  prec = PREC_NULLCOAL; break;
        case TokenType::OR:                 prec = PREC_OR; break;
        case TokenType::AND:                prec = PREC_AND; break;
        case TokenType::EQUAL_EQUAL:
        case TokenType::EQUAL_EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::GREATER:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER_EQUAL:
        case TokenType::IS:
            prec = PREC_CMP;
            break;
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::SHL:
        case TokenType::SHR:
            prec = PREC_BITWISE;
            break;
        case TokenType::PLUS:
        case TokenType::MINUS:
            prec = PREC_ADD;
            break;
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
            prec = PREC_MUL;
            break;
        case TokenType::POW:
            prec = PREC_POW;
            break;
        default:
            prec = PREC_NONE;
            break;
    }
    LOG_EXPR_EXTREME("infixPrec: " << LucDebug::tokenTypeToString(t) << " -> " << prec);
    return prec;
}

/**
 * @brief Converts a token type to a BinaryOp enum value.
 *
 * Used for arithmetic, comparison, logical, and bitwise operators.
 *
 * @param t The token type.
 * @return BinaryOp – corresponding enum value.
 */
BinaryOp Parser::tokenToBinaryOp(TokenType t) const {
    BinaryOp op;
    switch (t) {
        case TokenType::PLUS:                op = BinaryOp::Add; break;
        case TokenType::MINUS:               op = BinaryOp::Sub; break;
        case TokenType::MUL:                 op = BinaryOp::Mul; break;
        case TokenType::DIV:                 op = BinaryOp::Div; break;
        case TokenType::POW:                 op = BinaryOp::Pow; break;
        case TokenType::MOD:                 op = BinaryOp::Mod; break;
        case TokenType::EQUAL_EQUAL:         op = BinaryOp::Eq; break;
        case TokenType::EQUAL_EQUAL_EQUAL:   op = BinaryOp::RefEq; break;
        case TokenType::NOT_EQUAL:           op = BinaryOp::Ne; break;
        case TokenType::LESS:                op = BinaryOp::Lt; break;
        case TokenType::GREATER:             op = BinaryOp::Gt; break;
        case TokenType::LESS_EQUAL:          op = BinaryOp::Le; break;
        case TokenType::GREATER_EQUAL:       op = BinaryOp::Ge; break;
        case TokenType::AND:                 op = BinaryOp::And; break;
        case TokenType::OR:                  op = BinaryOp::Or; break;
        case TokenType::BIT_AND:             op = BinaryOp::BitAnd; break;
        case TokenType::BIT_OR:              op = BinaryOp::BitOr; break;
        case TokenType::BIT_XOR:             op = BinaryOp::BitXor; break;
        case TokenType::SHL:                 op = BinaryOp::Shl; break;
        case TokenType::SHR:                 op = BinaryOp::Shr; break;
        default:                             op = BinaryOp::Add; break;
    }
    LOG_EXPR_EXTREME("tokenToBinaryOp: " << LucDebug::tokenTypeToString(t) << " -> " << static_cast<int>(op));
    return op;
}

/**
 * @brief Converts a token type to an AssignOp enum value.
 *
 * Used for assignment and compound assignment operators.
 *
 * @param t The token type.
 * @return AssignOp – corresponding enum value.
 */
AssignOp Parser::tokenToAssignOp(TokenType t) const {
    AssignOp op;
    switch (t) {
        case TokenType::ASSIGN:          op = AssignOp::Assign; break;
        case TokenType::PLUS_ASSIGN:     op = AssignOp::AddAssign; break;
        case TokenType::MINUS_ASSIGN:    op = AssignOp::SubAssign; break;
        case TokenType::MUL_ASSIGN:      op = AssignOp::MulAssign; break;
        case TokenType::DIV_ASSIGN:      op = AssignOp::DivAssign; break;
        case TokenType::POW_ASSIGN:      op = AssignOp::PowAssign; break;
        case TokenType::MOD_ASSIGN:      op = AssignOp::ModAssign; break;
        case TokenType::BIT_AND_ASSIGN:  op = AssignOp::BitAndAssign; break;
        case TokenType::BIT_OR_ASSIGN:   op = AssignOp::BitOrAssign; break;
        case TokenType::BIT_XOR_ASSIGN:  op = AssignOp::BitXorAssign; break;
        case TokenType::SHL_ASSIGN:      op = AssignOp::ShlAssign; break;
        case TokenType::SHR_ASSIGN:      op = AssignOp::ShrAssign; break;
        default:                         op = AssignOp::Assign; break;
    }
    LOG_EXPR_EXTREME("tokenToAssignOp: " << LucDebug::tokenTypeToString(t) << " -> " << static_cast<int>(op));
    return op;
}

/**
 * @brief Checks if a token type is an assignment operator.
 *
 * @param t The token type.
 * @return true if the token is an assignment operator.
 */
bool Parser::isAssignOp(TokenType t) const {
    bool result;
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            result = true;
            break;
        default:
            result = false;
            break;
    }
    LOG_EXPR_EXTREME("isAssignOp: " << LucDebug::tokenTypeToString(t) << " -> " << result);
    return result;
}
