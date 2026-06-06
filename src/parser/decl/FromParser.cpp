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
 *   from_entry := param_group { param_group } '->' type '=' func_body   -- inline entry
 *               | func_ref                                               -- path entry
 * 
 *   func_ref := IDENTIFIER
 *             | IDENTIFIER '.' IDENTIFIER
 *             | func_ref generic_args
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
 * @return ASTPtr<FromDeclAST> – from node on success, nullptr on error
 */
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseFromDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse target type
    TypePtr targetType;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        LUC_LOG_DECL_EXTREME("parseFromDecl: array target");
        // Check if this is a generic array target (contains '<' after kind)
        if (looksLikeGenericArray()) {
            LUC_LOG_DECL_EXTREME("parseFromDecl: generic array target");
            targetType = parseGenericArray();
        } else {
            targetType = parseArrayType();
        }
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            LUC_LOG_DECL("parseFromDecl: ERROR - invalid array target");
            errorAt(DiagCode::E1005, "invalid array target in from block");
            return nullptr;
        }
        node->targetType = std::move(targetType);
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
        LUC_LOG_DECL_EXTREME("parseFromDecl: parsed " << genericParams.size() << " generic parameter(s)");
        
        // Build the target type as a NamedTypeAST with NO generic arguments
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        targetType = std::move(namedType);
        node->targetType = std::move(targetType);
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
        node->targetType = std::move(targetType);
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
            entries.push_back(std::move(entry));
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
    for (auto& e : entries) builder.push_back(std::move(e));
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
 *   from_entry := param_group { param_group } '->' type '=' func_body   -- inline entry
 *               | func_ref                                               -- path entry
 *
 * @par Examples
 *   // Inline entry
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
 * - Inline entry missing body: reports error, returns nullptr
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
        // Parameter groups: flat accumulation
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        int groupCount = 0;
        
        while (ts_.check(TokenType::LPAREN)) {
            groupCount++;
            ParamGroup group = parseParamGroup();
            groupSizes.push_back(group.size());
            LUC_LOG_DECL_EXTREME("parseFromEntry: parameter group #" << groupCount 
                                 << " has " << group.size() << " parameter(s)");
            for (auto& p : group) {
                allParams.push_back(std::move(p));
            }
        }
        
        auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
        for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
        entry->sig.allParams = paramsBuilder.build();

        auto gsBuilder = arena_.makeBuilder<size_t>();
        for (auto& sz : groupSizes) gsBuilder.push_back(sz);
        entry->sig.groupSizes = gsBuilder.build();
        
        LUC_LOG_DECL_EXTREME("parseFromEntry: total " << allParams.size() << " parameters");

        // Expect '->'
        if (!ts_.check(TokenType::ARROW)) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected '->' before return type");
            errorAt(DiagCode::E1001, "expected '->' before return type for conversion entry");
            return nullptr;
        }
        ts_.advance();

        // Parse return type
        TypePtr returnType = parseType();
        if (!returnType) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected return type after '->'");
            errorAt(DiagCode::E1005, "expected return type after '->'");
            return nullptr;
        }
        entry->returnType = std::move(returnType);
        LUC_LOG_DECL_EXTREME("parseFromEntry: return type parsed");

        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            LUC_LOG_DECL("parseFromEntry: ERROR - expected '=' before body");
            errorAt(DiagCode::E1001, "expected '=' before body for conversion entry");
            return nullptr;
        }
        ts_.advance();

        // Parse body (block or expression)
        if (ts_.check(TokenType::LBRACE)) {
            LUC_LOG_DECL_EXTREME("parseFromEntry: block body");
            entry->body = parseBlock();
        } else {
            LUC_LOG_DECL_EXTREME("parseFromEntry: expression body (will be wrapped)");
            SourceLocation bodyLoc = ts_.currentLoc();
            ExprPtr expr = parseExpr();
            if (!expr) {
                LUC_LOG_DECL("parseFromEntry: ERROR - expected expression after '='");
                errorAt(DiagCode::E1008, "expected expression after '=' in conversion entry");
                return nullptr;
            }

            // Wrap expression in a return statement inside a block
            auto ret = arena_.make<ReturnStmtAST>();
            ret->loc = bodyLoc;
            std::vector<ExprPtr> vals;
            vals.push_back(std::move(expr));
            auto valsBuilder = arena_.makeBuilder<ExprPtr>();
            for (auto& v : vals) valsBuilder.push_back(std::move(v));
            ret->values = valsBuilder.build();

            auto block = arena_.make<BlockStmtAST>();
            block->loc = bodyLoc;
            std::vector<StmtPtr> stmts;
            stmts.push_back(std::move(ret));
            auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
            for (auto& s : stmts) stmtsBuilder.push_back(std::move(s));
            block->stmts = stmtsBuilder.build();

            entry->body = std::move(block);
        }
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
        entry->path = std::move(funcRef);
        LUC_LOG_DECL_EXTREME("parseFromEntry: path entry parsed");
        // For path entries, signature and return type are read from the function
        // by the semantic pass. Body remains null.
    }

    return entry;
}