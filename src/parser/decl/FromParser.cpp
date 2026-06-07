/**
 * @file FromParser.cpp
 * @brief Parses `from` blocks defining implicit conversions to a type.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of `from` blocks, which define implicit
 * conversions from source types to a target type.
 * 
 * ## From Declaration Grammar (from LUC_GRAMMAR.md)
 * 
 *   from_decl := [ visibility_mod ] 'from' from_target [ generic_params ]
 *                '{' from_entry* '}'
 * 
 *   from_target := type                        -- any named, primitive, array, or alias type
 *                | generic_array_type          -- [_, <T>], [*, <T>], [N, <T>]
 * 
 *   generic_array_type := '[' '_' ',' '<' IDENTIFIER '>' ']'
 *                       | '[' '*' ',' '<' IDENTIFIER '>' ']'
 *                       | '[' INT_LITERAL ',' '<' IDENTIFIER '>' ']'
 * 
 *   from_entry := param_group { param_group } '->' type '=' block   -- inline entry
 *               | func_ref                                          -- path entry
 * 
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
 * 
 * @note Expression bodies are NOT supported for from entries because the
 *       compiler cannot infer the return type. Only explicit block bodies
 *       are allowed.
 * 
 * @see ParserDecl.cpp for declaration dispatch
 * @see FromEntryAST for conversion entry representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// 1. FROM DECLARATION
// ============================================================================

/**
 * @brief Parses a `from` block defining implicit conversions to a type.
 *
 * Grammar:
 *   from_decl := [ visibility_mod ] 'from' from_target [ generic_params ]
 *                '{' from_entry* '}'
 *
 * @par Examples
 *   export from Fahrenheit {
 *       (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
 *   }
 *
 *   from Wrapper<T> {
 *       (val T) -> Wrapper<T> = { return Wrapper<T> { value = val } }
 *   }
 *
 *   from int {
 *       (s string) -> int = { return #parseInt(s) }
 *   }
 *
 *   from [_, int] {                        -- concrete array target
 *       (r Range) -> [_, int] = { ... }
 *   }
 *
 *   from [*, <T>] {                        -- generic array target
 *       (v T) -> [*, T] = { return [v] }
 *   }
 *
 * ─── Parsing Strategy ──────────────────────────────────────────────────────
 *   1. Parse 'from' keyword and visibility
 *   2. Parse target type:
 *      a. If target is '[': parse as array target (concrete or generic)
 *      b. If target is generic struct (IDENTIFIER '<'): parse generic parameters
 *         as declarations and store on FromDeclAST
 *      c. Otherwise, parse as normal type (primitive or named)
 *   3. Parse '{' and zero or more from entries (calls parseFromEntry)
 *   4. Parse '}' to close block
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'from' keyword
 * On exit:  positioned after the closing '}'
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: returns nullptr
 * - Missing '{' after target: reports error, returns nullptr
 * - Invalid entry: skips entry, continues
 * - Missing '}': consume() reports error
 *
 * @param vis Visibility modifier (Private, Package, or Export)
 * @return FromDeclPtr – from node on success, nullptr on error
 */
FromDeclPtr Parser::parseFromDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseFromDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse target type
    TypePtr targetType = nullptr;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        LUC_LOG_DECL_EXTREME("parseFromDecl: array target");
        // parseArrayType() handles both concrete and generic arrays
        targetType = parseArrayType();
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseFromDecl: ERROR - invalid array target");
            errorAt(DiagCode::E1005, "invalid array target in from block");
            return nullptr;
        }
        node->targetType = targetType;
        
        // Set TargetKind based on result
        if (targetType->isa<GenericArrayTypeAST>()) {
            node->targetKind = TargetKind::GenericArray;
            auto* genArray = targetType->as<GenericArrayTypeAST>();
            node->arrayTypeParamName = genArray->typeParamName;
            LUC_LOG_DECL_EXTREME("parseFromDecl: generic array target with param <" 
                                 << pool_.lookup(node->arrayTypeParamName) << ">");
        } else {
            node->targetKind = TargetKind::Concrete;
        }
        // Array targets cannot have additional generic parameters
    }
    // Case 2: Generic struct type or generic type alias (IDENTIFIER '<')
    else if (ts_.check(TokenType::IDENTIFIER) && 
             looksLikeGenericTypeInstantiation()) {
        
        InternedString typeName = pool_.intern(ts_.advance().value);
        LUC_LOG_DECL_EXTREME("parseFromDecl: generic struct target " << pool_.lookup(typeName));
        
        // Parse generic parameters (these are declarations, not arguments)
        ArenaSpan<GenericParamPtr> genericParams = parseGenericParams();
        node->genericParams = genericParams;
        node->targetKind = TargetKind::GenericNamed;
        LUC_LOG_DECL_EXTREME("parseFromDecl: parsed " << genericParams.size() << " generic parameter(s)");
        
        // Build the target type as a NamedTypeAST with NO generic arguments
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        targetType = namedType;
        node->targetType = targetType;
    }
    // Case 3: Normal type (primitive, named, or concrete array via parseType)
    else if (looksLikeType()) {
        LUC_LOG_DECL_EXTREME("parseFromDecl: normal type target");
        targetType = parseType();
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseFromDecl: ERROR - invalid target type");
            errorAt(DiagCode::E1005, "invalid target type in from block");
            return nullptr;
        }
        node->targetType = targetType;
        node->targetKind = TargetKind::Concrete;
    }
    // Case 4: Error
    else {
        LUC_LOG_DECL("parseFromDecl: ERROR - expected target type");
        errorAt(DiagCode::E1005, "expected target type in from block");
        return nullptr;
    }

    // Parse from block body
    ts_.consume(TokenType::LBRACE, "expected '{' to open from block");

    std::vector<FromEntryPtr> entries;
    int entryCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t entrySavedPos = ts_.getPos();

        // Parse a from entry (inline or path)
        FromEntryPtr entry = parseFromEntry();
        if (entry) {
            entryCount++;
            LUC_LOG_DECL_EXTREME("parseFromDecl: parsed entry #" << entryCount);
            entries.push_back(entry);
        } else {
            LUC_LOG_DECL("parseFromDecl: ERROR - failed to parse from entry");
            // Error recovery: skip to next entry or closing brace
            if (ts_.getPos() == entrySavedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::LPAREN) && !ts_.check(TokenType::IDENTIFIER)) {
                ts_.advance();
            }
        }
    }

    auto builder = arena_.makeBuilder<FromEntryPtr>();
    for (auto& e : entries) builder.push_back(e);
    node->entries = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close from block");
    
    LUC_LOG_DECL_VERBOSE("parseFromDecl: parsed " << entryCount << " entry(s)");
    return node;
}

// ============================================================================
// 2. FROM ENTRY PARSING
// ============================================================================

/**
 * @brief Parses a single `from` entry (conversion definition).
 *
 * Grammar:
 *   from_entry := param_group { param_group } '->' type '=' block   -- inline entry
 *               | func_ref                                          -- path entry
 *
 * @par Examples
 *   // Inline entry (must have block body)
 *   (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
 *
 *   // Path entry
 *   utils.floatToStr
 *
 *   // Path entry with generic instantiation
 *   utils.toString<int>
 *
 * ─── Detection ─────────────────────────────────────────────────────────────
 *   The function checks the current token:
 *   - If '(' is found, it's an inline entry with parameter groups
 *   - Otherwise, it's a path entry (function reference)
 *
 * ─── Important ────────────────────────────────────────────────────────────
 *   Expression bodies (e.g., `= expr`) are NOT allowed. The compiler cannot
 *   infer the return type from an expression body, so only explicit block
 *   bodies are supported.
 *
 * @return FromEntryPtr – parsed entry node, or nullptr on error
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '(' (inline) or identifier (path)
 * On exit:  positioned after the entry (after body for inline, or after func_ref for path)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Inline entry missing '->': reports error, returns nullptr
 * - Inline entry missing return type: reports error, returns nullptr
 * - Inline entry missing '=': reports error, returns nullptr
 * - Inline entry missing '{' for body: reports error, returns nullptr
 * - Inline entry missing block body: reports error, returns nullptr
 * - Path entry missing function reference: reports error, returns nullptr
 */
FromEntryPtr Parser::parseFromEntry() {
    LUC_LOG_DECL_EXTREME("parseFromEntry: entering");
    SourceLocation loc = ts_.currentLoc();
    auto entry = arena_.make<FromEntryAST>();
    entry->loc = loc;

    // Determine if this is an inline entry (starts with '(') or path entry
    if (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_DECL_EXTREME("parseFromEntry: inline entry");
        // ---------- Inline Entry ----------
        
        // Parse the entire function type (parameter groups + return types)
        TypePtr funcType = parseFuncType();
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseFromEntry: ERROR - invalid function signature");
            errorAt(DiagCode::E1005, "invalid function signature in conversion entry");
            return nullptr;
        }
        entry->funcType = funcType->as<FuncTypeAST>();

        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected '=' before body");
            errorAt(DiagCode::E1001, "expected '=' before body for conversion entry");
            return nullptr;
        }
        ts_.advance();

        // Parse body - ONLY block bodies allowed
        if (!ts_.check(TokenType::LBRACE)) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected '{' for body");
            errorAt(DiagCode::E1001, "expected '{' for body; from entries require explicit block bodies");
            return nullptr;
        }
        
        LUC_LOG_DECL_EXTREME("parseFromEntry: block body");
        entry->body = parseBlock();
        
    } else {
        // ---------- Path Entry ----------
        LUC_LOG_DECL_EXTREME("parseFromEntry: path entry");
        // Parse function reference (may include generic instantiation)
        ExprPtr funcRef = parseFuncRef();
        if (!funcRef || funcRef->isa<UnknownExprAST>()) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected function reference");
            errorAt(DiagCode::E1008, "expected function reference in from entry");
            return nullptr;
        }
        entry->path = funcRef;
        LUC_LOG_DECL_EXTREME("parseFromEntry: path entry parsed");
        // For path entries, signature and return type are read from the function
        // by the semantic pass. Body remains null.
    }

    return entry;
}