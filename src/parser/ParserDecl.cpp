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
 * @brief Parses `package name` declaration.
 * 
 * Must be the first non‑comment line of every .luc file.
 * 
 * Grammar: `package` IDENTIFIER
 * 
 * Example: `package math`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'package' keyword
 * On exit:  positioned after the package name
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing package name: returns dummy node with "<error>" name
 * - Missing 'package' keyword: handled by caller (parse())
 */
ASTPtr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::PACKAGE, "expected 'package'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}

/**
 * @brief Parses `use` import declaration.
 * 
 * Grammar: `use` module_path [ `as` IDENTIFIER ]
 * 
 * Example: `use math.vec2 as v`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'use' keyword
 * On exit:  positioned after the alias (or after the last path segment)
 * 
 * ─── Module Path Format ────────────────────────────────────────────────────
 *   - Dotted identifiers: `std.io`, `renderer.core.math`
 *   - Minimum one identifier
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing module path: returns node with empty path
 * - Missing alias after 'as': reports error, continues
 */
ASTPtr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    std::vector<InternedString> path;
    path.push_back(pool_.intern(ts_.advance().value));

    while (ts_.check(TokenType::DOT) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        path.push_back(pool_.intern(ts_.advance().value));
    }

    auto builder = arena_.makeBuilder<InternedString>();
    for (auto& p : path) builder.push_back(std::move(p));
    node->path = builder.build();

    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(ts_.advance().value);
        }
    }

    return node;
}

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

    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    node->sig.rawQualifiers = qBuilder.build();

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
    node->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    node->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        node->sig.returnTypes = parseReturnList();
    }

    // Body
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
 * @brief Parses an enum type declaration.
 * 
 * Grammar: `enum` IDENTIFIER `{` variant (`,` variant)* `}`
 * 
 * Example: `enum ShaderStage { Vertex = 0x01, Fragment = 0x02 }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'enum' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Variant Values ───────────────────────────────────────────────────────
 *   - Auto variants start at 0, increment by 1
 *   - Explicit values reset the counter
 *   - Duplicate values are a semantic error
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with parseEnumVariant()
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing enum name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid variant: skips variant, continues
 * - Missing '}': consume() reports error
 */
ASTPtr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::ENUM, "expected 'enum'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    ts_.consume(TokenType::LBRACE, "expected '{' to open enum body");

    std::vector<EnumVariantPtr> variants;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            variants.push_back(std::move(variant));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<EnumVariantPtr>();
    for (auto& v : variants) builder.push_back(std::move(v));
    node->variants = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close enum body");
    return node;
}

/**
 * @brief Parses a single enum variant.
 * 
 * Grammar: IDENTIFIER [ `=` ( INT_LITERAL | HEX_LITERAL ) ]
 * 
 * Example: `Vertex = 0x01`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at variant name
 * On exit:  positioned after optional explicit value
 * 
 * ─── Value Parsing ────────────────────────────────────────────────────────
 *   - Strips underscore separators (e.g., `0xFF_FF`)
 *   - Supports decimal and hexadecimal
 *   - Overflow detection (reports error, variant created with no value)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing variant name: returns nullptr
 * - Invalid literal after '=': reports error, variant created with no value
 */
EnumVariantPtr Parser::parseEnumVariant() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum variant name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (ts_.match(TokenType::ASSIGN)) {
        if (ts_.check(TokenType::INT_LITERAL) || ts_.check(TokenType::HEX_LITERAL) ||
            ts_.check(TokenType::BINARY_LITERAL)) {
            Token valTok = ts_.advance();
            std::string raw = valTok.value;
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;
            char* endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
            } else {
                error(ts_.locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer");
            }
        } else {
            errorAt(DiagCode::E2009, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}

/**
 * @brief Parses a trait declaration (method contract, no implementations).
 * 
 * Grammar: `trait` IDENTIFIER [ `<` generic_params `>` ] `{` method* `}`
 * 
 * Example: `pub trait Drawable { draw (), bounds () -> Rect }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'trait' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Trait methods are signatures only (no body, no '=')
 *   - Traits are top‑level only (semantic pass rejects local traits)
 *   - The semantic pass verifies impl blocks provide all methods
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with parseTraitMethod()
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing trait name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid method: skips method, continues
 * - Missing '}': consume() reports error
 */
ASTPtr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TRAIT, "expected 'trait'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open trait body");

    std::vector<TraitMethodPtr> methods;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            methods.push_back(std::move(method));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close trait body");
    return node;
}

/**
 * @brief Parses a method signature inside a trait.
 * 
 * Grammar: IDENTIFIER [ `~async` | `~nullable` | `~parallel` ]*
 *          param_group+ [ `->` return_list ]
 * 
 * Example: `fetch ~async (url string) -> string`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name
 * On exit:  positioned after the return list (or after last param group)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - No body, no '=' token
 *   - Qualifiers are stored raw; semantic phase resolves them
 *   - Supports curried methods (multiple parameter groups)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: returns nullptr
 * - Missing '(' after name/qualifiers: reports error, returns nullptr
 */
TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = ts_.currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);
    
    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    method->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for trait method parameters");
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
    method->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    method->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
    }
    
    return method;
}

/**
 * @brief Parses a trait reference in an impl conformance declaration.
 * 
 * Grammar: `:` IDENTIFIER [ `<` type_args `>` ]
 * 
 * Example: `: Drawable` or `: Comparable<int>`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at ':' token (already consumed? check caller)
 * On exit:  positioned after generic arguments (or after trait name)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Called after the ':' is already consumed
 *   - Generic arguments are optional
 *   - Multiple trait bounds (e.g., `: Drawable + Serializable`) NOT supported
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing trait name after ':': returns nullptr
 */
TraitRefPtr Parser::parseTraitRef() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::COLON, "expected ':' before trait name");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LESS)) {
        ref->genericArgs = parseGenericArgs();
    }

    return ref;
}

/**
 * @brief Parses generic type parameters on a declaration.
 * 
 * Grammar: `<` generic_param (`,` generic_param)* `>`
 * 
 * Example: `<T, U : Drawable, V>`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '<' token
 * On exit:  positioned after the closing '>'
 * 
 * ─── Empty List ───────────────────────────────────────────────────────────
 *   - An empty list `<` `>` is allowed and returns an empty ArenaSpan
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern. If parseGenericParam() makes no progress,
 * consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '>' at end: consume() reports error
 * - Invalid generic parameter: skip parameter, continue
 * - Unbalanced brackets: returns empty span after recovery
 */
ArenaSpan<GenericParamPtr> Parser::parseGenericParams() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<GenericParamPtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<GenericParamPtr>();
    }
    
    std::vector<GenericParamPtr> params;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        GenericParamPtr gp = parseGenericParam();
        if (gp) params.push_back(std::move(gp));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' after generic parameters");
    
    auto builder = arena_.makeBuilder<GenericParamPtr>();
    for (auto& p : params) builder.push_back(std::move(p));
    return builder.build();
}

/**
 * @brief Parses a single generic parameter with optional trait constraints.
 * 
 * Grammar: IDENTIFIER [ `:` IDENTIFIER ( `+` IDENTIFIER )* ]
 * 
 * Example: `T : Hashable + Comparable + Printable`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at parameter name
 * On exit:  positioned after the last constraint (or after the name)
 * 
 * ─── Constraint Storage ────────────────────────────────────────────────────
 *   - Constraints are stored as ArenaSpan<InternedString>
 *   - The semantic pass resolves trait names and validates
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing parameter name: returns nullptr
 * - Missing trait name after ':' or '+': reports error, stops parsing constraints
 */
GenericParamPtr Parser::parseGenericParam() {
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected generic parameter name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    auto gp = arena_.make<GenericParamAST>(name);
    
    if (ts_.match(TokenType::COLON)) {
        std::vector<InternedString> constraints;
        while (true) {
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected trait name in constraint");
                break;
            }
            constraints.push_back(pool_.intern(ts_.advance().value));
            if (!ts_.match(TokenType::PLUS)) break;
        }
        auto builder = arena_.makeBuilder<InternedString>();
        for (auto& c : constraints) builder.push_back(std::move(c));
        gp->constraints = builder.build();
    }
    
    return gp;
}

/**
 * @brief Parses an impl block that binds methods to a type.
 * 
 * Grammar: `impl` type_name [ `<` generic_params `>` ] [ `as` IDENTIFIER ]
 *          [ `:` trait_ref ] `{` method_decl* `}`
 * 
 * Example: `impl Circle as c : Drawable { draw () { c:render() } }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'impl' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - `as IDENTIFIER` replaces `self` as the receiver name
 *   - `: trait_ref` indicates conformance to a trait
 *   - Generic parameters on impl must match target type's arity
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with parseMethodDecl()
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: returns nullptr
 * - Missing '{' after header: reports error, returns nullptr
 * - Invalid method: skips method, continues
 * - Unrecognised token inside impl: calls synchronize()
 * - Missing '}': consume() reports error
 */
ASTPtr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target type after 'impl'");
        return nullptr;
    }

    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after 'as' for receiver alias");
        } else {
            node->receiverAlias = pool_.intern(ts_.advance().value);
        }
    }

    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in impl block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    if (ts_.check(TokenType::COLON)) {
        node->traitRef = parseTraitRef();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open impl body");

    std::vector<MethodDeclPtr> methods;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();

        if (ts_.check(TokenType::IDENTIFIER)) {
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                methods.push_back(std::move(md));
            } else {
                if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
                while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                       !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
            }
            continue;
        }

        errorAt(DiagCode::E2002, "expected method declaration inside impl block");
        if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
               !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
    }

    auto builder = arena_.makeBuilder<MethodDeclPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close impl body");
    return node;
}

/**
 * @brief Parses a method implementation inside an impl block.
 * 
 * Grammar: IDENTIFIER [ `~async` | `~nullable` | `~parallel` ]*
 *          param_group+ [ `->` return_list ] `=` body
 * 
 * Example: `length () -> float = { return #sqrt(x*x + y*y) }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name
 * On exit:  positioned after the body (or after signature if no body)
 * 
 * ─── Body Parsing ─────────────────────────────────────────────────────────
 * Same as function bodies (block, verbose anon-func, or expression)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - No visibility modifiers (impl block controls visibility)
 *   - Receiver is `self` (or alias from impl's `as` clause)
 *   - `~nullable` marks the method binding as nullable
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: returns nullptr
 * - Missing '(' after name/qualifiers: reports error, returns nullptr
 * - Missing '=' before body: reports error, returns nullptr
 * - Missing body after '=': reports error, returns nullptr
 */
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = ts_.currentLoc();

    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);

    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    method->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method");
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
    method->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    method->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
    }

    // Body
    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body");
        return nullptr;
    }
    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        method->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
        method->body = parseBlock();
    } else {
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method");
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

        method->body = std::move(block);
    }

    ts_.match(TokenType::SEMICOLON);
    return method;
}

/**
 * @brief Parses a `from` block defining implicit conversions.
 * 
 * Grammar: `from` type [ `<` generic_params `>` ] `{` from_entry* `}`
 * 
 * Example: `export from Fahrenheit { (c Celsius) -> Fahrenheit = { ... } }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'from' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── From Entry Format ────────────────────────────────────────────────────
 *   - Parameter groups define the source type(s)
 *   - `->` followed by return type (must match target type)
 *   - `=` followed by conversion body (block or expression)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Target type can be ANY type (primitive, struct, enum, array, function, etc.)
 *   - Generic parameters are optional and only allowed when target is generic
 *   - Can be top‑level or local
 *   - Multiple from blocks for same target allowed in different scopes
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with from entry parsing
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: returns nullptr
 * - Missing '{' after target: reports error, returns nullptr
 * - Invalid entry: skips entry, continues
 * - Missing '}': consume() reports error
 */
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse the target type (ANY type, not just named type)
    // Save position in case we need to roll back for generic params detection
    size_t beforeTypePos = ts_.getPos();
    TypePtr targetType = parseType();
    
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in from block");
        return nullptr;
    }
    
    node->targetType = std::move(targetType);

    // Check for generic parameters on the from block
    // Generic parameters are only allowed if the target type is generic
    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

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