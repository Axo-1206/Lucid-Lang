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
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse target type
    TypePtr targetType;

    // Case 1: Array target (concrete or generic)
    if (ts_.check(TokenType::LBRACKET)) {
        // Check if this is a generic array target (contains '<' after kind)
        if (looksLikeGenericArray()) {
            targetType = parseGenericArray();
        } else {
            targetType = parseArrayType();
        }
        
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid array target in from block");
            return nullptr;
        }
        node->targetType = std::move(targetType);
        // Array targets cannot have additional generic parameters
    }
    // Case 2: Generic struct type (IDENTIFIER '<')
    else if (ts_.check(TokenType::IDENTIFIER) && 
             looksLikeGenericTypeInstantiation()) {
        
        InternedString typeName = pool_.intern(ts_.advance().value);
        
        // Parse generic parameters (these are declarations, not arguments)
        ArenaSpan<GenericParamPtr> genericParams = parseGenericParams();
        node->genericParams = genericParams;
        
        // Build the target type as a NamedTypeAST with NO generic arguments
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        targetType = std::move(namedType);
        node->targetType = std::move(targetType);
    }
    // Case 3: Normal type (primitive, named, or concrete array via parseType)
    else if (looksLikeType()) {
        targetType = parseType();
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid target type in from block");
            return nullptr;
        }
        node->targetType = std::move(targetType);
    }
    // Case 4: Error
    else {
        errorAt(DiagCode::E2005, "expected target type in from block");
        return nullptr;
    }

    // Parse from block body
    ts_.consume(TokenType::LBRACE, "expected '{' to open from block");

    std::vector<FromEntryPtr> entries;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t entrySavedPos = ts_.getPos();

        // Parse a from entry (inline or path)
        FromEntryPtr entry = parseFromEntry();
        if (entry) {
            entries.push_back(std::move(entry));
        } else {
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
    SourceLocation loc = ts_.currentLoc();
    auto entry = arena_.make<FromEntryAST>();
    entry->loc = loc;

    // Determine if this is an inline entry (starts with '(') or path entry
    if (ts_.check(TokenType::LPAREN)) {
        // ---------- Inline Entry ----------
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
        entry->sig.allParams = paramsBuilder.build();

        auto gsBuilder = arena_.makeBuilder<size_t>();
        for (auto& sz : groupSizes) gsBuilder.push_back(sz);
        entry->sig.groupSizes = gsBuilder.build();

        // Expect '->'
        if (!ts_.check(TokenType::ARROW)) {
            errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
            return nullptr;
        }
        ts_.advance();

        // Parse return type
        TypePtr returnType = parseType();
        if (!returnType) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            return nullptr;
        }
        entry->returnType = std::move(returnType);

        // Expect '='
        if (!ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            return nullptr;
        }
        ts_.advance();

        // Parse body (block or expression)
        if (ts_.check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            SourceLocation bodyLoc = ts_.currentLoc();
            ExprPtr expr = parseExpr();
            if (!expr) {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
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
        // Parse function reference (may include generic instantiation)
        ExprPtr funcRef = parseFuncRef();
        if (!funcRef || funcRef->isa<UnknownExprAST>()) {
            errorAt(DiagCode::E2008, "expected function reference in from entry");
            return nullptr;
        }
        entry->path = std::move(funcRef);
        // For path entries, signature and return type are read from the function
        // by the semantic pass. Body remains null.
    }

    return entry;
}