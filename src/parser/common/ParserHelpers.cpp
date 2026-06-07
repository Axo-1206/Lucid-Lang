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
 *   4. Module Path            – parse dotted module paths for `use` declarations
 *   5. Function Reference     – parse `func_ref` for assignments and references
 *   6. Precedence Helpers     – map tokens to precedence levels and operator enums
 *   7. isPrimitiveTypeToken   – check if a token is a primitive type
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
 * @see LUC_GRAMMAR.md for grammar rules
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
    LUC_LOG_DECL_EXTREME("parseParamList: entering");
    std::vector<ParamPtr> list;
    int paramCount = 0;
    
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseParamList: ERROR - expected parameter name");
            errorAt(DiagCode::E1003, "expected parameter name");
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
            LUC_LOG_DECL_EXTREME("parseParamList: parameter #" << paramCount 
                                 << " = " << pool_.lookup(param->name));
            list.push_back(param);
        } else {
            LUC_LOG_DECL("parseParamList: ERROR - failed to parse type for parameter");
        }
    }
    
    LUC_LOG_DECL_EXTREME("parseParamList: parsed " << paramCount << " parameter(s)");
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
    LUC_LOG_DECL_EXTREME("parseArgList: entering");
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;
    int argCount = 0;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            LUC_LOG_DECL("parseArgList: ERROR - too many consecutive errors in argument list");
            errorAt(DiagCode::E1002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            LUC_LOG_DECL("parseArgList: ERROR - expected argument expression");
            errorAt(DiagCode::E1008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        argCount++;
        LUC_LOG_DECL_EXTREME("parseArgList: argument #" << argCount);
        args.push_back(arg);

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            LUC_LOG_DECL("parseArgList: ERROR - expected ',' after argument");
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
    
    LUC_LOG_DECL_EXTREME("parseArgList: parsed " << argCount << " argument(s)");
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
    LUC_LOG_DECL_EXTREME("parseReturnList: entering");
    
    // Case 1: Single return type (no parentheses)
    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_DECL_EXTREME("parseReturnList: single return type (no parentheses)");
        // Use existing looksLikeType() to validate we have a type start
        if (!looksLikeType()) {
            LUC_LOG_DECL("parseReturnList: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        TypePtr t = parseType();
        if (!t || t->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseReturnList: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(t);
        LUC_LOG_DECL_EXTREME("parseReturnList: single return type parsed");
        return builder.build();
    }

    // We have '(' - need to determine if it's a function type or multi-return
    LUC_LOG_DECL_EXTREME("parseReturnList: detected '(' - checking if function type or multi-return");
    size_t savedPos = ts_.getPos();
    ts_.consume(TokenType::LPAREN, "expected '(' for return list");

    // Use lookahead to decide
    if (isFunctionTypeAfterParen(ts_.getPos())) {
        LUC_LOG_DECL_EXTREME("parseReturnList: this is a function type");
        // This is a function type (the parentheses belong to the function's parameter group)
        ts_.setPos(savedPos);
        TypePtr funcType = parseFuncType();
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseReturnList: ERROR - expected function type");
            errorAt(DiagCode::E1005, "expected function type");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(funcType);
        LUC_LOG_DECL_EXTREME("parseReturnList: function type parsed");
        return builder.build();
    }

    // Not a function type → parse as multi-return list
    LUC_LOG_DECL_EXTREME("parseReturnList: multi-return list");
    // We are already positioned after the '(' (from the consume above)
    std::vector<TypePtr> types;
    int typeCount = 0;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!types.empty() && !ts_.match(TokenType::COMMA)) {
            if (ts_.check(TokenType::RPAREN)) break;
            LUC_LOG_DECL("parseReturnList: ERROR - expected ',' between return types");
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
            LUC_LOG_DECL("parseReturnList: ERROR - expected return type");
            errorAt(DiagCode::E1005, "expected return type");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            break;
        }
        if (t && !t->isa<UnknownTypeAST>()) {
            typeCount++;
            LUC_LOG_DECL_EXTREME("parseReturnList: return type #" << typeCount);
            types.push_back(t);
        }
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close return type list");

    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& t : types) builder.push_back(t);
    
    LUC_LOG_DECL_EXTREME("parseReturnList: parsed " << typeCount << " return type(s)");
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
    LUC_LOG_DECL_EXTREME("parseQualifiers: entering");
    QualifierSet qs;
    int qualifierCount = 0;
    
    while (ts_.check(TokenType::TILDE)) {
        SourceLocation loc = ts_.currentLoc();
        ts_.advance(); // consume '~'
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseQualifiers: ERROR - expected qualifier name after '~'");
            error(loc, DiagCode::E1003, "expected qualifier name after '~'");
            break;
        }
        InternedString name = pool_.intern(ts_.advance().value);
        std::string_view nameStr = pool_.lookup(name);
        LUC_LOG_DECL_EXTREME("parseQualifiers: found qualifier '~" << nameStr << "'");
        
        // Check for duplicate qualifier
        if (std::find(qs.raw.begin(), qs.raw.end(), name) != qs.raw.end()) {
            LUC_LOG_DECL("parseQualifiers: ERROR - duplicate qualifier '~" << nameStr << "'");
            error(loc, DiagCode::E1018, 
                  "duplicate qualifier '~" + std::string(nameStr) + "'");
            continue;
        }
        
        const QualifierEntry* entry = qualifier::lookup(name);
        if (!entry) {
            LUC_LOG_DECL("parseQualifiers: ERROR - unknown qualifier '~" << nameStr << "'");
            error(loc, DiagCode::E1016, 
                  "unknown qualifier '~" + std::string(nameStr) + 
                  "'; known qualifiers: " + qualifier::allNames());
            continue;
        }
        
        qs.raw.push_back(name);
        qs.bitmask |= entry->bit;
        qualifierCount++;
        LUC_LOG_DECL_EXTREME("parseQualifiers: added qualifier (bitmask now 0x" 
                             << std::hex << qs.bitmask << std::dec << ")");
    }
    
    if (qualifierCount > 0) {
        LUC_LOG_DECL_EXTREME("parseQualifiers: total " << qualifierCount << " qualifier(s)");
    }
    return qs;
}

// ============================================================================
// 4. MODULE PATH PARSING
// ============================================================================
//
// parseUsePath() parses a dotted identifier sequence for `use` declarations.
// ============================================================================

/**
 * @brief Parses a dotted path for `use` declarations only.
 * 
 * Grammar:
 *   use_path := IDENTIFIER { '.' IDENTIFIER }
 * 
 * Example: `std.io`, `renderer.core.math`
 * 
 * @return std::vector<InternedString> – path segments in order.
 * 
 * @warning This function is ONLY for `use` declarations. For expression
 *          field access, use parsePrimaryExpr() which produces FieldAccessExprAST.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first identifier
 * On exit:  positioned after last identifier
 */
std::vector<InternedString> Parser::parseUsePath() {
    LUC_LOG_DECL_EXTREME("parseUsePath: entering");
    std::vector<InternedString> path;
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL_EXTREME("parseUsePath: no identifier, returning empty");
        return path;
    }
    
    path.push_back(pool_.intern(ts_.advance().value));
    LUC_LOG_DECL_EXTREME("parseUsePath: segment 1 = " << pool_.lookup(path.back()));
    
    int segmentCount = 1;
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseUsePath: ERROR - expected identifier after '.'");
            errorAt(DiagCode::E1003, "expected identifier after '.'");
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
        segmentCount++;
        LUC_LOG_DECL_EXTREME("parseUsePath: segment " << segmentCount 
                             << " = " << pool_.lookup(path.back()));
    }
    
    LUC_LOG_DECL_EXTREME("parseUsePath: " << segmentCount << " segment(s)");
    return path;
}

// ============================================================================
// 5. FUNCTION REFERENCE (for assignment forms)
// ============================================================================
//
// parseFuncRef() parses a function reference for use in:
//   - Method assignments in `impl` blocks
//   - Path entries in `from` blocks
//   - Pipeline steps (`|>`)
//   - Compose operands (`+>`)
//
// The grammar for func_ref is shared across all these contexts.
// ============================================================================

/**
 * @brief Parses a named function reference for use in declarations and expressions.
 *
 * Grammar (from LUC_GRAMMAR.md, Shared Productions):
 *   func_ref := IDENTIFIER                    -- local or imported name
 *             | IDENTIFIER '.' IDENTIFIER     -- module path: pkg.fn
 *             | func_ref generic_args        -- generic instantiation: fn<T>
 *
 * @par Examples
 *   utils.getVersion                       – dotted path
 *   transform<int, string>                 – generic instantiation
 *   std.map<U>                             – dotted + generic
 *   identity                               – plain identifier
 *
 * @par Valid Contexts
 *   - Method assignments in `impl` blocks: `id = identity<int>(i)!`
 *   - Path entries in `from` blocks: `from string { toString<int> }`
 *   - Pipeline steps (without `!`): `42 |> identity<int>`
 *   - Compose operands: `validate +> toString<int>`
 *   - Variable initialisers: `let f (int) -> int = identity<int>`
 *
 * @par Invalid (Not part of grammar)
 *   - Method references with colon (`vec:normalize`) – use `expr ':' IDENTIFIER` in
 *     pipeline/compose parsers instead.
 *   - Function calls or expressions – `func_ref` is a *name*, not a call.
 *   - Field access on an expression – only dotted module paths are allowed.
 *
 * @return ExprPtr – expression representing the function reference.
 *         Result may be:
 *         - `IdentifierExprAST`        – plain identifier (e.g., `identity`)
 *         - `FieldAccessExprAST`       – dotted module path (e.g., `math.utils.toString`)
 *         - Both may have `genericArgs` stored directly on them for generic
 *           function references (e.g., `identity<int>` has `genericArgs` on the
 *           IdentifierExprAST, `math.toString<float>` has `genericArgs` on the
 *           FieldAccessExprAST)
 *
 * ─── Parsing Steps ─────────────────────────────────────────────────────────
 *   1. Parse the first identifier (required).
 *   2. Parse optional dotted path segments (`.identifier`).
 *   3. Parse optional generic arguments (`<type-list>`).
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at an IDENTIFIER (the function name).
 * On exit:  positioned after the optional generic arguments (or after the name).
 *
 * ─── Generic Arguments ────────────────────────────────────────────────────
 *   - The opening `<` is consumed BEFORE calling `parseGenericArgs()`.
 *   - `parseGenericArgs()` parses the type list and consumes the closing `>`.
 *   - Generic arguments are stored directly on the `IdentifierExprAST` or
 *     `FieldAccessExprAST` node (not in a separate wrapper node).
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing identifier: returns `UnknownExprAST`, reports error.
 *   - Missing identifier after '.': reports error, stops building path.
 *   - Malformed generic arguments: `parseGenericArgs()` reports error,
 *     returns empty span (still creates the identifier/field access node
 *     with empty generic args).
 *
 * ─── Important Notes ──────────────────────────────────────────────────────
 *   - Colon `:` is NOT handled here – it belongs to pipeline steps and
 *     compose operands (`expr ':' IDENTIFIER` for method calls).
 *   - Dotted paths represent module/namespace resolution, not field access
 *     on runtime values. The semantic pass resolves them appropriately.
 *   - Generic arguments are always explicit (no inference). An uninstantiated
 *     generic function (e.g., `identity` without `<type>`) is not a valid
 *     `func_ref` – the caller must provide type arguments.
 *   - Generic arguments are stored directly on the AST node (IdentifierExprAST
 *     or FieldAccessExprAST) rather than in a separate wrapper node. This
 *     simplifies the AST and makes the semantic analysis more straightforward.
 *
 * @see parseGenericArgs() for the generic argument list parser
 */
ExprPtr Parser::parseFuncRef() {
    LUC_LOG_EXPR_VERBOSE("parseFuncRef: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();

    // Parse a name (identifier or dotted path)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_EXPR("parseFuncRef: ERROR - expected function name at line "
                     << ts_.peek().line << ", col " << ts_.peek().column);
        errorAt(DiagCode::E1003);
        return arena_.make<UnknownExprAST>();
    }
    
    SourceLocation firstTokenLoc = ts_.currentLoc();
    Token firstToken = ts_.advance();
    std::string name = firstToken.value;
    LUC_LOG_EXPR_EXTREME("parseFuncRef: base name = '" << name 
                         << "' at line " << firstToken.line << ", col " << firstToken.column);

    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = firstTokenLoc;

    // Parse dotted path segments (module path)
    while (ts_.check(TokenType::DOT)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_EXPR("parseFuncRef: ERROR - expected identifier after '.' at line "
                         << ts_.peek().line << ", col " << ts_.peek().column);
            errorAt(DiagCode::E1003);
            break;
        }
        SourceLocation fieldLoc = ts_.currentLoc();
        Token fieldToken = ts_.advance();
        LUC_LOG_EXPR_EXTREME("parseFuncRef: dotted segment ." << fieldToken.value
                             << " at line " << fieldToken.line << ", col " << fieldToken.column);
        
        auto node = arena_.make<FieldAccessExprAST>();
        node->loc = fieldLoc;
        node->object = expr;
        node->field = pool_.intern(fieldToken.value);
        expr = node;
    }

    // Optional generic arguments
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_EXPR_EXTREME("parseFuncRef: parsing generic arguments at line "
                             << ts_.peek().line << ", col " << ts_.peek().column);
        ts_.advance(); // consume '<'
        ArenaSpan<TypePtr> typeArgs = parseGenericArgs();
        
        // Apply generic arguments to the appropriate node type
        if (auto* ident = expr->as<IdentifierExprAST>()) {
            LUC_LOG_EXPR("parseFuncRef: adding " << typeArgs.size() 
                         << " generic args to identifier");
            ident->genericArgs = typeArgs;
        } else if (auto* field = expr->as<FieldAccessExprAST>()) {
            LUC_LOG_EXPR("parseFuncRef: adding " << typeArgs.size() 
                         << " generic args to field access");
            field->genericArgs = typeArgs;
        } else {
            LUC_LOG_EXPR("parseFuncRef: ERROR - cannot add generic args to node type "
                         << static_cast<int>(expr->kind));
        }
        
        LUC_LOG_EXPR_EXTREME("parseFuncRef: generic args count = " << typeArgs.size());
    }

    LUC_LOG_EXPR_VERBOSE("parseFuncRef: success");
    return expr;
}

// ============================================================================
// 6. PRECEDENCE HELPERS
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
    LUC_LOG_EXPR_EXTREME("infixPrec: " << LucDebug::tokenTypeToString(t) << " -> " << prec);
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
    LUC_LOG_EXPR_EXTREME("tokenToBinaryOp: " << LucDebug::tokenTypeToString(t) << " -> " << static_cast<int>(op));
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
    LUC_LOG_EXPR_EXTREME("tokenToAssignOp: " << LucDebug::tokenTypeToString(t) << " -> " << static_cast<int>(op));
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
    LUC_LOG_EXPR_EXTREME("isAssignOp: " << LucDebug::tokenTypeToString(t) << " -> " << result);
    return result;
}

// ============================================================================
// 7. isPrimitiveTypeToken
// ============================================================================
// 
// Static helper that returns true if a TokenType is a primitive type keyword.
// 
// Primitive types include:
//   - Boolean:   bool
//   - Signed:    byte, short, int, long, int8, int16, int32, int64
//   - Unsigned:  ubyte, ushort, uint, ulong, uint8, uint16, uint32, uint64
//   - Floating:  float, double, decimal
//   - Text:      string, char
//   - Dynamic:   any
// 
// Used by looksLikeType() and various type parsers.
// 
// ─── Complexity ────────────────────────────────────────────────────────────
// O(1) – switch statement on the enum value.
// 
// ─── Note ──────────────────────────────────────────────────────────────────
// This is static because it only depends on the TokenType, not on parser state.
// ============================================================================

bool Parser::isPrimitiveTypeToken(TokenType type) {
    switch (type) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
        case TokenType::TYPE_ANY:
            return true;
        default:
            return false;
    }
}