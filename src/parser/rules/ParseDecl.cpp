/**
 * @file ParseDecl.cpp
 * @brief Implementation of declaration parsers.
 * 
 * This file implements all declaration parsers:
 * - Import declarations
 * - Variable declarations
 * - Function declarations
 * - Struct declarations
 * - Enum declarations
 * - Trait declarations
 * - Field declarations
 * - Enum variants
 * - Trait fields
 * - Trait references
 * 
 * Note: All declaration parsers do NOT consume the terminating semicolon.
 * The caller (parseDecl) is responsible for consuming it.
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <vector>

namespace parser {

// =============================================================================
// parseImportDecl() – Parses a 'import' declaration
// =============================================================================

/**
 * @brief Parse a `import` declaration.
 * 
 * Grammar: `import path [as alias]`
 * 
 * Alias rules:
 * 1. If `as alias` is present, import the specified alias
 * 2. Otherwise, import the last component of the path as the alias
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ImportDeclAST* The parsed import declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 */
ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("Enter UseDecl");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Parse 'import' keyword ─────────────────────────────────────────
    if (!stream.check(TokenType::IMPORT)) {
        ctx.error(stream, DiagCode::E1001, "import", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume 'import'
    
    // ─── 2. Parse the import path ───────────────────────────────────────────
    auto pathParts = parseImportPath(stream, ctx);
    if (pathParts.empty()) {
        ctx.error(stream, DiagCode::E1101, stream.peekValue()); // Expected module path
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Build the full import path string (dot-separated)
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    InternedString usePath = ctx.pool.intern(fullPath);
    
    // ─── 3. Determine the alias ──────────────────────────────────────────
    InternedString alias;
    std::string aliasStr;
    
    if (stream.match(TokenType::AS)) {
        // Explicit alias: `import path as alias`
        Token aliasTok = stream.consume(TokenType::IDENTIFIER);
        if (aliasTok.type != TokenType::EOF_TOKEN) {
            alias = ctx.pool.intern(aliasTok.value);
            aliasStr = std::string(ctx.pool.lookup(alias));
        } else {
            ctx.error(stream, DiagCode::E1001, "name alias after keyword 'as'", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else {
        // Implicit alias: import the last component of the path
        // e.g., "std.math" → alias = "math"
        InternedString lastPart = pathParts.back();
        alias = lastPart;
        aliasStr = std::string(ctx.pool.lookup(alias));
    }
    
    // ─── 4. Create the ImportDeclAST ────────────────────────────────────────
    auto* useDecl = ctx.arena.make<ImportDeclAST>();
    useDecl->loc = loc;
    useDecl->path = usePath;
    useDecl->alias = alias;
    
    // ─── 5. Import the module ────────────────────────────────────────────
    if (!ctx.resolver) {
        ctx.error(stream, DiagCode::E0004, usePath);
        return useDecl;
    }

    // Resolve the import path to a file path
    InternedString filePath = ctx.resolver->resolveUsePath(usePath);
    if (!filePath.isValid()) {
        ctx.errorAt(loc, DiagCode::E0003);
        // No file to parse — nothing to hand to parse(), so no ModuleAST
        // is created for this path at all. useDecl is still returned;
        // the semantic pass will find nothing at ctx.resolver for this
        // path and can report accordingly.
        return useDecl;
    }

    std::string pathStr = std::string(ctx.pool.lookup(filePath));

    if (ctx.resolver->isParsing(filePath)) {
        // Circular import. Reported here rather than inside parse() —
        // this is the only place with a real source location (this `import`
        // statement) to point the diagnostic at.
        // E0005 "Cyclic module dependency." has no %s placeholder — no args.
        ctx.errorAt(loc, DiagCode::E0005);
        // Still call parse() — it detects the same condition and returns
        // an uncached dummy without touching `source` at all (its own
        // isParsing check runs before source is ever used), so passing an
        // empty string here is safe and avoids an unnecessary disk read.
        parse(pathStr, "", ctx);
        return useDecl;
    }

    // Perf only, not correctness: parse() itself cache-checks internally
    // and would no-op safely either way, but there's no reason to pay for
    // a disk read just to hand parse() a result it will immediately
    // discard because the cache already has it.
    if (!ctx.resolver->getParsedModule(filePath)) {
        std::string source = ctx.resolver->readModuleSource(filePath);
        // Always call parse(), even for an empty (but existing) file —
        // an empty module is still a real, valid ModuleAST that needs to
        // exist and be cached/ordered, not a case to special-case away.
        parse(pathStr, source, ctx);
        // Return value intentionally discarded: parseImportDecl only needs
        // {path, alias} in ImportDeclAST (see step 4 above). The semantic
        // pass resolves the actual ModuleAST* later via ctx.resolver,
        // by path — parse() has already cached it as a side effect.
    }
    
    LOG_PARSER_MINIMAL("Parsed import declaration: '", fullPath, "' as '", aliasStr, "'");
    
    return useDecl;
}

// =============================================================================
// parseVarDecl() – Parses a variable declaration
// =============================================================================

/**
 * @brief Parse a variable declaration.
 * 
 * Grammar: `('let' | 'const') IDENTIFIER type [ '=' expr ]`
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return VarDeclAST* The parsed variable declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 */
VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Parse keyword (let or const)
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst) {
        if (!stream.match(TokenType::LET)) {
            ctx.error(stream, DiagCode::E1001, "let/const", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    // Parse name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "variable name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Parse type (required)
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.error(stream, DiagCode::E1003, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Parse initializer (optional)
    ExprPtr init = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        init = parseExpr(stream, ctx);
        if (!init) {
            ctx.error(stream, DiagCode::E1006, "initializer", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else if (isConst) {
        // const requires an initializer
        ctx.error(stream, DiagCode::E1006, "initializer", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* varDecl = ctx.arena.make<VarDeclAST>();
    varDecl->loc = loc;
    varDecl->name = name;
    varDecl->keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    varDecl->type = type;
    varDecl->init = init;
    varDecl->isConst = isConst;
    
    LOG_PARSER_DETAIL("Parsed variable declaration: ", ctx.toString(name));
    return varDecl;
}

// =============================================================================
// parseFuncDecl() – Parses a function declaration
// =============================================================================

/**
 * @brief Parse a function declaration.
 * 
 * Grammar: 
 *   ('let' | 'const') IDENTIFIER [ generic_params ]
 *   param_group { param_group }
 *   [ '->' return_type ]
 *   '=' func_body
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return FuncDeclAST* The parsed function declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 */
FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse keyword (let or const) ──────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst) {
        if (!stream.match(TokenType::LET)) {
            ctx.error(stream, DiagCode::E1001, "let/const", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    // ─── 2. Parse function name ──────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "function name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse generic parameters (optional) ──────────────────────────
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse the function type (parameter groups + return types) ────
    TypeAST* type = parseFuncType(stream, ctx);
    if (!type) {
        ctx.error(stream, DiagCode::E1003, "function type", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Ensure it's a function type
    if (!type->isa<FuncTypeAST>()) {
        ctx.error(stream, DiagCode::E1003, "function type", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    FuncTypeAST* funcType = type->as<FuncTypeAST>();
    
    // ─── 5. Check for '=' before body ────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1007, "=", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 6. Parse function body ──────────────────────────────────────────
    StmtPtr body = nullptr;
    
    // Check for block body: { ... }
    if (stream.check(TokenType::LBRACE)) {
        stream.advance(); // Consume '{'

        // From here on the parser is inside the function body — push once,
        // and it pops automatically when this if-block ends below,
        // regardless of which of the '}' recovery paths is taken.
        ScopedContext bodyGuard(ctx, SyntacticContext::FuncBody, stream.currentLoc());

        body = parseBlock(stream, ctx);
        if (!stream.check(TokenType::RBRACE)) {
            ctx.error(stream, DiagCode::E1005, "}", "function body", stream.peekValue());
            synchronizeTo(stream, ctx, TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE)) {
                stream.advance(); // Consume '}' to recover
            }
        } else {
            stream.advance(); // Consume '}'
        }
    } else {
        // Parse expression body
        ExprPtr expr = parseExpr(stream, ctx);
        if (!expr) {
            ctx.error(stream, DiagCode::E1006, "function reference", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
        
        // Wrap the expression in a return statement
        auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
        returnStmt->loc = expr->loc;
        
        auto builder = ctx.arena.makeBuilder<ExprPtr>();
        builder.push_back(expr);
        returnStmt->values = builder.build();
        
        body = returnStmt;
    }
    
    // ─── 7. Build the AST node ──────────────────────────────────────────
    auto* funcDecl = ctx.arena.make<FuncDeclAST>();
    funcDecl->loc = loc;
    funcDecl->name = name;
    funcDecl->keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    funcDecl->genericParams = genericParams;
    funcDecl->funcType = funcType;
    funcDecl->body = body;
    funcDecl->isConst = isConst;
    funcDecl->valueType = funcType;
    
    LOG_PARSER_DETAIL("Parsed function declaration: ", ctx.toString(name));
    return funcDecl;
}

// =============================================================================
// parseStructDecl() – Parses a struct declaration
// =============================================================================

/**
 * @brief Parse a struct declaration.
 * 
 * Grammar: 
 *   'struct' IDENTIFIER [ generic_params ]
 *   [ ':' trait_ref { ',' trait_ref } ]
 *   '{' { struct_field } '}'
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return StructDeclAST* The parsed struct declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 */
StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse 'struct' keyword ──────────────────────────────────────
    if (!stream.check(TokenType::STRUCT)) {
        ctx.error(stream, DiagCode::E1001, "struct", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume 'struct'
    
    // ─── 2. Parse struct name ────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "struct name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse generic parameters (optional) ──────────────────────────
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse trait implementations (optional) ──────────────────────
    std::vector<TraitRefPtr> traitRefs;
    if (stream.match(TokenType::COLON)) {
        // ─── Skip initial trailing commas ──────────────────────────
        // Ex: : ,,, Vector2, Named
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, ",", "struct traits");
        }
        
        // ─── Parse trait references ──────────────────────────────────
        while (!stream.isAtEnd() && !stream.check(TokenType::LBRACE)) {
            // Parse a trait reference
            TraitRefPtr traitRef = parseTraitRef(stream, ctx);
            if (traitRef) {
                traitRefs.push_back(traitRef);
            } else {
                // Error already reported by parseTraitRef, try to recover
                // using the trait list's follow-set
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::LBRACE);
            }
            
            // ─── Comma Separator Handling ──────────────────────────
            // Consume at least 1 comma and skip consecutive commas
            if (stream.consumeTrailing(TokenType::COMMA) > 1) {
                ctx.error(stream, DiagCode::E1009, ",", "struct traits");
            }
        }
    }
    
    // ─── 5. Parse struct body ────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.error(stream, DiagCode::E1004, "{", "struct body", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '{'

    // ─── 6. Push StructBody context — auto-pops on every return path ────
    ScopedContext bodyGuard(ctx, SyntacticContext::StructBody, stream.currentLoc());

    std::vector<FieldDeclPtr> fields;
    
    // ─── 7. Check for empty body ──────────────────────────────────────────
    if (stream.check(TokenType::RBRACE)) {
        stream.advance(); // Consume '}'
        // Empty struct body - skip to building AST
        
        auto* structDecl = ctx.arena.make<StructDeclAST>();
        structDecl->loc = loc;
        structDecl->name = name;
        structDecl->genericParams = genericParams;
        structDecl->fields = ctx.arena.makeBuilder<FieldDeclPtr>().build();
        
        // Build trait refs span
        auto traitBuilder = ctx.arena.makeBuilder<TraitRefPtr>();
        for (auto* tr : traitRefs) {
            traitBuilder.push_back(tr);
        }
        structDecl->traitRefs = traitBuilder.build();
        
        LOG_PARSER_DETAIL("Parsed empty struct declaration: ", ctx.toString(name));
        return structDecl;
    }
    
    // ─── 8. Skip initial trailing semicolons ─────────────────────────────
    // Ex: { ;;; field1 int, field2 string }
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.error(stream, DiagCode::E1009, ";", "field declaration");
    }
    
    // ─── 9. Parse fields ──────────────────────────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Parse a field
        FieldDeclPtr field = parseFieldDecl(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Error already reported by parseFieldDecl, try to recover
            // using the struct body's follow-set
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
            // Check if we recovered to a semicolon or brace
        }
        
        // ─── 9.1 Comma Separator Handling ──────────────────────────────────
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.error(stream, DiagCode::E1009, ",", "enum variant");
        }
    }
    
    // ─── 10. Consume closing brace ────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "}", "struct body", stream.peekValue());
    } else {
        // We must be at '}' (loop condition guaranteed !stream.check(RBRACE) is false)
        stream.advance(); // Consume '}'
    }
    
    // ─── 11. Build the AST node ───────────────────────────────────────────
    auto* structDecl = ctx.arena.make<StructDeclAST>();
    structDecl->loc = loc;
    structDecl->name = name;
    structDecl->genericParams = genericParams;
    
    // Build fields span
    auto fieldBuilder = ctx.arena.makeBuilder<FieldDeclPtr>();
    for (auto* f : fields) {
        fieldBuilder.push_back(f);
    }
    structDecl->fields = fieldBuilder.build();
    
    // Build trait refs span
    auto traitBuilder = ctx.arena.makeBuilder<TraitRefPtr>();
    for (auto* tr : traitRefs) {
        traitBuilder.push_back(tr);
    }
    structDecl->traitRefs = traitBuilder.build();
    
    LOG_PARSER_DETAIL("Parsed struct declaration: ", ctx.toString(name), 
                      " with ", fields.size(), " fields and ", 
                      traitRefs.size(), " traits");
    return structDecl;
}

// =============================================================================
// parseFieldDecl() – Parses a struct field
// =============================================================================

/**
 * @brief Parse a struct field declaration.
 * 
 * Grammar: [ '@[' attr { ',' attr } ']' ] [ 'const' ] IDENTIFIER type [ '=' expr ]
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return FieldDeclAST* The parsed field declaration, or nullptr on error
 *
 * @note parseStructDecl already consumes extra ',' and ';'
 * @note Attributes are parsed and attached to the field declaration.
 */
FieldDeclPtr parseFieldDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes (if any) ─────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse 'const' modifier (optional) ────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    
    // ─── 3. Parse name ──────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "field name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 4. Parse type ──────────────────────────────────────────────────
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.error(stream, DiagCode::E1003, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 5. Parse default value (optional) ──────────────────────────────
    ExprPtr defaultVal = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        defaultVal = parseExpr(stream, ctx);
        if (!defaultVal) {
            ctx.error(stream, DiagCode::E1006, "default value", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    // ─── 6. Build the AST node ───────────────────────────────────────────
    auto* fieldDecl = ctx.arena.make<FieldDeclAST>();
    fieldDecl->loc = loc;
    fieldDecl->name = name;
    fieldDecl->type = type;
    fieldDecl->defaultVal = defaultVal;
    fieldDecl->isConst = isConst;
    fieldDecl->attributes = attrs;  // Attach attributes to the field

    // Attach doc comment to the declaration
    if (doc.has_value()) {
        fieldDecl->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed field: ", ctx.toString(name), 
                      (isConst ? " const" : ""),
                      " with ", attrs.size(), " attributes");
    return fieldDecl;
}

// =============================================================================
// parseEnumDecl() – Parses an enum declaration
// =============================================================================

/**
 * @brief Parse an enum declaration.
 * 
 * Grammar: 'enum' IDENTIFIER [ ':' int_type ] '{' { enum_variant } '}'
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return EnumDeclAST* The parsed enum declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 *    
 */
EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse 'enum' keyword ──────────────────────────────────────
    if (!stream.check(TokenType::ENUM)) {
        ctx.error(stream, DiagCode::E1001, "enum", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume 'enum'
    
    // ─── 2. Parse enum name ──────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "enum name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse backing type (optional) ──────────────────────────────
    PrimitiveTypeAST* backingType = nullptr;
    if (stream.match(TokenType::COLON)) {
        TypePtr type = parseType(stream, ctx);
        if (type && type->isa<PrimitiveTypeAST>()) {
            backingType = type->as<PrimitiveTypeAST>();
            // Verify it's an integer type
            // TODO: Check that backingType is an integer type
        } else {
            ctx.error(stream, DiagCode::E1003, "integer", stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    // ─── 4. Parse enum body ──────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.error(stream, DiagCode::E1004, "{", "enum body", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '{'

    // ─── 5. Push EnumBody context — auto-pops on every return path ────
    ScopedContext bodyGuard(ctx, SyntacticContext::EnumBody, stream.currentLoc());

    std::vector<EnumVariantPtr> variants;
    
    // ─── 6. Check for empty body ─────────────────────────────────────────
    if (stream.check(TokenType::RBRACE)) {
        stream.advance(); // Consume '}'
        // Empty enum body - skip to building AST
        auto* enumDecl = ctx.arena.make<EnumDeclAST>();
        enumDecl->loc = loc;
        enumDecl->name = name;
        enumDecl->backingType = backingType;
        enumDecl->variants = ctx.arena.makeBuilder<EnumVariantPtr>().build();
        
        LOG_PARSER_DETAIL("Parsed empty enum declaration: ", ctx.toString(name));
        return enumDecl;
    }
    
    // ─── 7. Skip initial trailing semicolons ─────────────────────────────────
    // Ex: { ;;; North = 0; South = 1 }
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.error(stream, DiagCode::E1009, ";", "enum variant");
    }
    
    // ─── 8. Parse variants ────────────────────────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Parse a variant
        EnumVariantPtr variant = parseEnumVariant(stream, ctx);
        if (variant) {
            variants.push_back(variant);
        } else {
            // Error already reported by parseEnumVariant, try to recover
            // using the enum body's follow-set
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
        
        // ─── 8.1 Semicolon Separator Handling ──────────────────────────────────
        // Consume at least 1 comma and skip consecutive commas
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.error(stream, DiagCode::E1009, ";", "enum variant");
        }
    }
    
    // ─── 9. Consume closing brace ────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "}", "enum body", stream.peekValue());
    } else {
        // We must be at '}' (loop condition guaranteed !stream.check(RBRACE) is false)
        stream.advance(); // Consume '}'
    }
    
    // ─── 10. Build the AST node ───────────────────────────────────────────
    auto* enumDecl = ctx.arena.make<EnumDeclAST>();
    enumDecl->loc = loc;
    enumDecl->name = name;
    enumDecl->backingType = backingType;
    
    // Build variants span
    auto builder = ctx.arena.makeBuilder<EnumVariantPtr>();
    for (auto* v : variants) {
        builder.push_back(v);
    }
    enumDecl->variants = builder.build();
    
    LOG_PARSER_DETAIL("Parsed enum declaration: ", ctx.toString(name), 
                      " with ", variants.size(), " variants");
    return enumDecl;
}

// =============================================================================
// parseEnumVariant() – Parses an enum variant
// =============================================================================

/**
 * @brief Parse an enum variant.
 * 
 * Grammar: [ '@[' attr { ',' attr } ']' ] IDENTIFIER '=' INT_LIT
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return EnumVariantAST* The parsed enum variant, or nullptr on error
 */
EnumVariantPtr parseEnumVariant(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes (if any) ─────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse name ──────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "variant name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Expect '=' ────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.error(stream, DiagCode::E1007, "=", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 4. Parse value (must be integer literal) ────────────────────────
    if (!stream.check(TokenType::INT_LITERAL) &&
        !stream.check(TokenType::HEX_LITERAL) &&
        !stream.check(TokenType::BINARY_LITERAL)) {
        ctx.error(stream, DiagCode::E1003, "integer literal", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token valueTok = stream.advance();
    int64_t value = std::stoll(valueTok.value);
    
    // ─── 5. Build the AST node ───────────────────────────────────────────
    auto* variant = ctx.arena.make<EnumVariantAST>(name, value);
    variant->loc = loc;
    variant->attributes = attrs;  // Attach attributes to the variant
    if (doc.has_value()) {
        variant->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed enum variant: ", ctx.toString(name), 
                      " = ", value,
                      " with ", attrs.size(), " attributes");
    return variant;
}

// =============================================================================
// parseTraitDecl() – Parses a trait declaration
// =============================================================================

/**
 * @brief Parse a trait declaration.
 * 
 * Grammar: 'trait' IDENTIFIER [ generic_params ] '{' { trait_field } '}'
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return TraitDeclAST* The parsed trait declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 *     
 */
TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse 'trait' keyword ──────────────────────────────────────
    if (!stream.check(TokenType::TRAIT)) {
        ctx.error(stream, DiagCode::E1001, "trait", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume 'trait'
    
    // ─── 2. Parse trait name ──────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "trait name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse generic parameters (optional) ──────────────────────────
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse trait body ──────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.error(stream, DiagCode::E1004, "{", "trait body", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '{'

    // ─── 5. Push TraitBody context — auto-pops on every return path ────
    ScopedContext bodyGuard(ctx, SyntacticContext::TraitBody, stream.currentLoc());

    std::vector<TraitFieldPtr> fields;
    
    // ─── 6. Check for empty body ─────────────────────────────────────────
    if (stream.check(TokenType::RBRACE)) {
        stream.advance(); // Consume '}'
        // Empty trait body - skip to building AST
        auto* traitDecl = ctx.arena.make<TraitDeclAST>();
        traitDecl->loc = loc;
        traitDecl->name = name;
        traitDecl->genericParams = genericParams;
        traitDecl->fields = ctx.arena.makeBuilder<TraitFieldPtr>().build();
        
        LOG_PARSER_DETAIL("Parsed empty trait declaration: ", ctx.toString(name));
        return traitDecl;
    }
    
    // ─── 7. Skip initial trailing semicolons ─────────────────────────────
    // Ex: { ;;; field1 int, field2 string }
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.error(stream, DiagCode::E1009, ";", "trait field");
    }
    
    // ─── 8. Parse fields ──────────────────────────────────────────────────
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Parse a trait field
        TraitFieldPtr field = parseTraitField(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Error already reported by parseTraitField, try to recover
            // using the trait body's follow-set
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
            // Check if we recovered to a semicolon or brace
            if (stream.check(TokenType::SEMICOLON)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RBRACE)) {
                break;
            } else {
                // No recovery token found - break to avoid infinite loop
                break;
            }
        }
        
        // ─── 8.1 Semicolon Separator Handling ──────────────────────────────
        // Consume at least 1 semicolon and skip consecutive semicolons
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.error(stream, DiagCode::E1009, ";", "trait field");
        }
    }
    
    // ─── 9. Consume closing brace ────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.error(stream, DiagCode::E1005, "}", "trait body", stream.peekValue());
    } else {
        // We must be at '}' (loop condition guaranteed !stream.check(RBRACE) is false)
        stream.advance(); // Consume '}'
    }
    
    // ─── 10. Build the AST node ───────────────────────────────────────────
    auto* traitDecl = ctx.arena.make<TraitDeclAST>();
    traitDecl->loc = loc;
    traitDecl->name = name;
    traitDecl->genericParams = genericParams;
    
    // Build fields span
    auto builder = ctx.arena.makeBuilder<TraitFieldPtr>();
    for (auto* f : fields) {
        builder.push_back(f);
    }
    traitDecl->fields = builder.build();
    
    LOG_PARSER_DETAIL("Parsed trait declaration: ", ctx.toString(name), 
                      " with ", fields.size(), " fields");
    return traitDecl;
}

// =============================================================================
// parseTraitField() – Parses a trait field
// =============================================================================

/**
 * @brief Parse a trait field declaration.
 * 
 * Grammar: [ '@[' attr { ',' attr } ']' ] [ 'const' ] IDENTIFIER type
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return TraitFieldPtr The parsed trait field, or nullptr on error
 */
TraitFieldPtr parseTraitField(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes (if any) ─────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse 'const' modifier (optional) ────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    
    // ─── 3. Parse name ──────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "trait field name", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 4. Parse type ──────────────────────────────────────────────────
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.error(stream, DiagCode::E1003, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 5. Build the AST node ───────────────────────────────────────────
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>();
    traitField->loc = loc;
    traitField->name = name;
    traitField->type = type;
    traitField->isConst = isConst;
    traitField->attributes = attrs;  // Attach attributes to the trait field
    if (doc.has_value()) {
        traitField->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed trait field: ", ctx.toString(name), 
                      (isConst ? " const" : ""),
                      " with ", attrs.size(), " attributes");
    return traitField;
}

} // namespace parser