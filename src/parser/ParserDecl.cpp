/**
 * @file ParserDecl.cpp
 * @brief Parses all declarations (functions, structs, enums, traits, impls, etc.).
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the declaration parsers for the Luc language.
 * Each function corresponds to a grammar rule from LUC_GRAMMAR.md and
 * builds the corresponding AST node.
 * 
 * ## Declarations Covered
 * 
 *   - Package       : `package math`
 *   - Use           : `use std.io`
 *   - Variable      : `let x int = 5`
 *   - Function      : `let add (a int)(b int) -> int = { ... }`
 *   - Struct        : `struct Vec2 { x float, y float }`
 *   - Enum          : `enum Direction { North, South }`
 *   - Trait         : `trait Drawable { draw () }`
 *   - Impl          : `impl Vec2 { length () -> float = { ... } }`
 *   - From          : `from Fahrenheit { (c Celsius) -> Fahrenheit = { ... } }`
 *   - Type Alias    : `type ID = int`
 * 
 * ## Sub‑Components
 * 
 *   - Generic parameters   : `<T, U : Drawable>`
 *   - Fields               : `x float = 0.0`
 *   - Enum variants        : `North = 1`
 *   - Trait methods        : `draw () -> Rect`
 *   - Impl methods         : `length () -> float = { ... }`
 * 
 * ## Token Consumption Guarantee
 * 
 * Every function in this file guarantees forward progress:
 *   - On success: consumes all tokens belonging to the declaration
 *   - On error: reports via `errorAt()` and either returns nullptr
 *     or skips to a safe recovery point (using `synchronize()`)
 * 
 * ## Loop Safety
 * 
 * Functions that parse lists (fields, variants, methods) use the saved
 * position pattern to detect infinite loops:
 * 
 *   size_t savedPos = ts_.getPos();
 *   while (!ts_.check(endToken)) {
 *       parseItem();
 *       if (ts_.getPos() == savedPos) {
 *           if (!ts_.isAtEnd()) ts_.advance();
 *           break;
 *       }
 *       savedPos = ts_.getPos();
 *   }
 * 
 * @see ParserDecl.cpp implementation
 * @see LUC_GRAMMAR.md for grammar rules
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

/**
 * @brief Parses a variable declaration (`let` or `const`).
 * 
 * Grammar: `let` IDENTIFIER type_ann [ `=` expr ]
 * 
 * Example: `let x int = 42`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at variable name (keyword already consumed)
 * On exit:  positioned after the optional initialiser
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 * - Type annotation is REQUIRED (no type inference)
 * - `const` must have an initialiser (enforced by semantic pass)
 * - `@extern` variables have no initialiser (semantic pass enforces)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing name: returns nullptr
 * - Missing type annotation: returns nullptr
 * - Missing expression after '=': reports error, node has null init
 */
ASTPtr<VarDeclAST> Parser::parseVarDecl(Visibility vis) {
    const Token& kwTok = ts_.peekAt(0);
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for variable '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr init;
    if (ts_.match(TokenType::ASSIGN)) {
        init = parseExpr();
        if (!init) {
            errorAt(DiagCode::E2008, "expected expression after '=' in variable declaration");
        }
    }

    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;

    return node;
}

/**
 * @brief Parses a function declaration.
 * 
 * Grammar:
 *   `let`/`const` IDENTIFIER [ `<` generic_params `>` ]
 *   [ `~async` | `~nullable` | `~parallel` ]*
 *   param_group+ [ `->` return_list ] [ `=` body ]
 * 
 * Example: `let add ~async (a int)(b int) -> int = { return a + b }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at function name (keyword already consumed)
 * On exit:  positioned after the body (or after the signature if no body)
 * 
 * ─── Body Parsing Variants ─────────────────────────────────────────────────
 *   1. Block body      : `= { ... }`
 *   2. Verbose anon-func: `= (params) -> ret { ... }`
 *   3. Expression body  : `= expr` (wrapped in ReturnStmt + BlockStmt)
 *   4. No body          : (valid for `@extern` declarations)
 * 
 * ─── Parameter Groups ──────────────────────────────────────────────────────
 *   - Function may have multiple `(params)` groups (currying)
 *   - Parameters are flattened into `allParams` with `groupSizes` tracking
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '(' after name: reports error, returns nullptr
 * - Missing body after '=': reports error, returns nullptr
 * - Invalid parameter group: skip group, continue parsing
 */
ASTPtr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis) {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    // Create a FuncTypeAST to hold signature and qualifiers
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        // Set bit based on qualifier name (semantic pass will validate)
        if (pool_.lookup(q) == "async") qualMask |= QualifierBits::Async;
        else if (pool_.lookup(q) == "nullable") qualMask |= QualifierBits::Nullable;
        else if (pool_.lookup(q) == "parallel") qualMask |= QualifierBits::Parallel;
        // Other qualifiers are ignored here; semantic pass reports errors
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: accumulate flat
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        std::vector<ParamPtr> group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        funcType->sig.returnTypes = parseReturnList();
    }

    // Store the complete function type in the declaration
    node->funcType = std::move(funcType);

    // Body (unchanged)
    if (!ts_.check(TokenType::ASSIGN)) {
        ts_.match(TokenType::SEMICOLON);
        return node;
    }

    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        node->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '='");
            return nullptr;
        }

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

        node->body = std::move(block);
    }
    
    ts_.match(TokenType::SEMICOLON);
    return node;
}

/**
 * @brief Parses a struct type declaration.
 * 
 * Grammar: `struct` IDENTIFIER [ `<` generic_params `>` ] `{` field_decl* `}`
 * 
 * Example: `pub struct Vec2<T> { x T, y T }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'struct' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing fields. If parseFieldDecl() makes
 * no progress, consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing struct name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid field: skips field, continues parsing remaining fields
 * - Missing '}': consume() reports error
 */
ASTPtr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::STRUCT, "expected 'struct'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open struct body");

    std::vector<FieldDeclPtr> fields;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        FieldDeclPtr field = parseFieldDecl();
        if (field) {
            fields.push_back(std::move(field));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<FieldDeclPtr>();
    for (auto& f : fields) builder.push_back(std::move(f));
    node->fields = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct body");
    return node;
}

/**
 * @brief Parses a struct field declaration.
 * 
 * Grammar: IDENTIFIER type [ `=` expr ]
 * 
 * Example: `r float = 1.0`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at field name
 * On exit:  positioned after optional default value expression
 * 
 * ─── Default Values ───────────────────────────────────────────────────────
 *   - Struct literals may omit fields with default values
 *   - Default value must be a compile‑time constant (semantic pass)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing field name: returns nullptr
 * - Missing type after name: reports error, returns nullptr
 * - Missing expression after '=': reports error (field still created)
 */
FieldDeclPtr Parser::parseFieldDecl() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for field '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr defaultVal;
    if (ts_.match(TokenType::ASSIGN)) {
        defaultVal = parseExpr();
        if (!defaultVal) {
            errorAt(DiagCode::E2008, "expected expression after '=' in field default value");
        }
    }

    auto field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = name;
    field->type = std::move(type);
    field->defaultVal = std::move(defaultVal);
    return field;
}
















/**
 * @brief Parses a `from` block defining implicit conversions to a type.
 * 
 * Grammar:
 *   from_decl := [ visibility_mod ] 'from' type [ generic_params ] '{' from_entry* '}'
 * 
 *   from_entry := param_group { param_group } '->' type '=' func_body
 * 
 * Examples:
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
 * ─── Parsing Strategy ──────────────────────────────────────────────────────
 *   1. Parse 'from' keyword and visibility
 *   2. Parse target type:
 *      a. If target is generic (IDENTIFIER '<'), parse generic parameters
 *         as declarations (GenericParamAST) and store on FromDeclAST
 *      b. Otherwise, parse as normal type (primitive or named)
 *   3. Parse '{' and zero or more from entries
 *   4. Parse '}' to close block
 * 
 * ─── Generic Parameters on From ────────────────────────────────────────────
 *   - Generic parameters are parsed as declarations (GenericParamAST)
 *   - They are stored in `node->genericParams`, NOT as arguments on the type
 *   - The target type's `NamedTypeAST` has empty `genericArgs`
 *   - Generic parameters are bound in the from entry bodies
 * 
 * ─── From Entry Format ────────────────────────────────────────────────────
 *   - Parameter groups define the source type(s) (may be curried)
 *   - '->' followed by return type (must match target type after substitution)
 *   - '=' followed by conversion body (block or expression)
 *   - No qualifiers (~async, ~nullable, ~parallel) allowed
 * 
 * ─── Target Type Rules (Semantic Pass) ─────────────────────────────────────
 *   - Target type can be ANY type (primitive, struct, enum, alias)
 *   - For generic structs, generic parameters must be declared on the from
 *   - Return types of entries must match the target type after substitution
 *   - Array and function types must be wrapped in a type alias first
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'from' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing from entries. If parseFromEntry()
 * makes no progress:
 *   - Consumes one token (advance)
 *   - Skips to next '(' or closing brace
 *   - Continues (does NOT push an entry)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: returns nullptr
 * - Missing '{' after target: reports error, returns nullptr
 * - Invalid entry: skips entry, continues
 * - Missing '->' in entry: reports error, skips entry
 * - Missing return type after '->': reports error, skips entry
 * - Missing '=' before body: reports error, skips entry
 * - Missing '}': consume() reports error
 * 
 * ─── Semantic Pass Validation (Not Parser Responsibility) ──────────────────
 * - Target type resolution (E3021)
 * - Return type matches target (E3022)
 * - Generic parameter usage correctness
 * - Conversion uniqueness in scope
 * 
 * @param vis Visibility modifier (Private, Package, or Export)
 *        determined by caller from 'pub'/'export' keywords
 * 
 * @return ASTPtr<FromDeclAST> – from node on success, nullptr on error
 */
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse target type: supports ANY type (primitive, struct, enum, alias)
    // But generic parameters are parsed separately for FromDeclAST
    
    // Check if we have a primitive type, identifier, or other type start
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected target type in from block");
        return nullptr;
    }
    
    // Peek ahead to see if this is a generic type with '<' 
    // We need to parse the base type name separately from generic parameters
    bool isGenericType = false;
    size_t beforeTypePos = ts_.getPos();
    
    // Check if current token is an identifier followed by '<' (generic type)
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t lookahead = ts_.skipCommentsFrom(ts_.getPos() + 1);

        if (lookahead < ts_.getTokenCount() && ts_.getTokenAt(lookahead).type == TokenType::LESS) {
            isGenericType = true;
        }
    }
    
    TypePtr targetType;
    
    if (isGenericType) {
        // Parse as named type with generic parameters as PART OF the type
        // For FromDeclAST, these become generic parameters, not arguments
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected type name");
            return nullptr;
        }
        
        InternedString typeName = pool_.intern(ts_.advance().value);
        
        // Parse generic parameters (these are declarations, not arguments)
        ArenaSpan<GenericParamPtr> genericParams = parseGenericParams();
        
        // Store generic parameters on the node
        node->genericParams = genericParams;
        
        // Build the target type as a NamedTypeAST with NO generic arguments
        // (generic parameters are stored separately on FromDeclAST)
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        // Intentionally leave genericArgs empty - the type is generic but
        // the parameters are stored at the from declaration level
        targetType = std::move(namedType);
    } else {
        // Non-generic type: parse normally
        targetType = parseType();
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid target type in from block");
            return nullptr;
        }
        
        // Check if the parsed type already contains generic arguments
        // (e.g., from Wrapper<int> where int is a concrete type argument)
        // In this case, genericParams should remain empty
        if (targetType->isa<NamedTypeAST>()) {
            auto namedType = static_cast<NamedTypeAST*>(targetType.get());
            if (!namedType->genericArgs.empty()) {
                // This is a concrete instantiation, not a generic declaration
                // No generic parameters to store
            }
        }
    }

    node->targetType = std::move(targetType);

    // Parse from block body
    ts_.consume(TokenType::LBRACE, "expected '{' to open from block");

    std::vector<FromEntryPtr> entries;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t entrySavedPos = ts_.getPos();

        if (!ts_.check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' to start parameter list for conversion entry");
            if (ts_.getPos() == entrySavedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::LPAREN)) ts_.advance();
            continue;
        }

        auto entry = arena_.make<FromEntryAST>();
        entry->loc = ts_.currentLoc();

        // Parameter groups: flat accumulation
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        while (ts_.check(TokenType::LPAREN)) {
            std::vector<ParamPtr> group = parseParamGroup();
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

        if (!ts_.check(TokenType::ARROW)) {
            errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
            continue;
        }
        ts_.advance();

        TypePtr returnType = parseType();
        if (!returnType) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            continue;
        }
        entry->returnType = std::move(returnType);

        if (!ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            continue;
        }
        ts_.advance();

        if (ts_.check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            SourceLocation bodyLoc = ts_.currentLoc();
            ExprPtr expr = parseExpr();
            if (expr) {
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
            } else {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
            }
        }

        entries.push_back(std::move(entry));
    }

    auto builder = arena_.makeBuilder<FromEntryPtr>();
    for (auto& e : entries) builder.push_back(std::move(e));
    node->entries = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close from block");
    return node;
}

/**
 * @brief Parses a type alias declaration.
 * 
 * Grammar: `type` IDENTIFIER [ `<` generic_params `>` ] `=` type
 * 
 * Example: `type Transform<T> = (v T) -> T`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'type' keyword
 * On exit:  positioned after the aliased type
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Does NOT create a new nominal type (unlike struct)
 *   - Alias is interchangeable with its target
 *   - Generic parameters allow instantiation with concrete types
 *   - Can be top‑level or local
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing alias name: returns nullptr
 * - Missing '=' after name/generics: reports error, returns nullptr
 * - Missing aliased type: reports error (node created with null aliasedType)
 */
ASTPtr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TYPE, "expected 'type' before type alias");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::ASSIGN, "expected '=' in type alias");

    node->aliasedType = parseType();
    if (!node->aliasedType) {
        errorAt(DiagCode::E2005, "expected type on the right-hand side of type alias");
    }

    return node;
}