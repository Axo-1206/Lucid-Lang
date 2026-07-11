/**
 * @file ParseDecl.cpp
 * @brief Implementation of declaration parsers.
 * 
 * This file implements all declaration parsers:
 * - Use declarations
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
// parseUseDecl() – Parses a 'use' declaration
// =============================================================================

/**
 * @brief Parse a `use` declaration.
 * 
 * Grammar: `use path [as alias]`
 * 
 * Alias rules:
 * 1. If `as alias` is present, use the specified alias
 * 2. Otherwise, use the last component of the path as the alias
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return UseDeclAST* The parsed use declaration, or nullptr on error
 * 
 * @note This function does NOT consume the terminating semicolon.
 *       The caller (parseDecl) consumes it.
 */
UseDeclAST* parseUseDecl(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("Enter UseDecl");

    SourceLocation loc = stream.currentLoc();

    // ─── 1. Parse 'use' keyword ─────────────────────────────────────────
    if (!stream.check(TokenType::USE)) {
        ctx.error(stream, DiagCode::E1001, "use", stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume 'use'
    
    // ─── 2. Parse the use path ───────────────────────────────────────────
    auto pathParts = parseUsePath(stream, ctx);
    if (pathParts.empty()) {
        ctx.error(stream, DiagCode::E1102, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Build the full use path string (dot-separated)
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
        // Explicit alias: `use path as alias`
        Token aliasTok = stream.consume(TokenType::IDENTIFIER);
        if (aliasTok.type != TokenType::EOF_TOKEN) {
            alias = ctx.pool.intern(aliasTok.value);
            aliasStr = std::string(ctx.pool.lookup(alias));
        } else {
            ctx.error(stream, DiagCode::E1102, stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else {
        // Implicit alias: use the last component of the path
        // e.g., "std.math" → alias = "math"
        InternedString lastPart = pathParts.back();
        alias = lastPart;
        aliasStr = std::string(ctx.pool.lookup(alias));
    }
    
    // ─── 4. Create the UseDeclAST ────────────────────────────────────────
    auto* useDecl = ctx.arena.make<UseDeclAST>();
    useDecl->loc = loc;
    useDecl->path = usePath;
    useDecl->alias = alias;
    
    // ─── 5. Import the module ────────────────────────────────────────────
    if (ctx.resolver) {
        // Resolve the use path to a file path
        InternedString filePath = ctx.resolver->resolveUsePath(usePath);
        if (!filePath.isValid()) {
            ctx.errorAt(loc, DiagCode::E0003);
            synchronizeToContext(stream, ctx);
            return useDecl;
        }
        
        // Check if already parsed
        ModuleAST* importedModule = ctx.resolver->getParsedModule(filePath);
        
        if (!importedModule) {
            // Read the source file
            std::string source = ctx.resolver->readModuleSource(filePath);
            if (source.empty()) {
                // Empty module - just return the declaration
                return useDecl;
            }
            
            // Parse the module (recursive call!)
            std::string pathStr = std::string(ctx.pool.lookup(filePath));
            importedModule = parse(pathStr, source, ctx);
            
            if (!importedModule) {
                ctx.error(stream, DiagCode::E0004, usePath);
                synchronizeToContext(stream, ctx);
                return useDecl;
            }
            
            // No cacheModule() call here — parse() already caches on success,
            // guarded by !ctx.hasErrors. Caching unconditionally here would
            // cache a module that had parse errors, which parse()'s own
            // guard exists specifically to prevent.
        }
        
        // Imported declarations are automatically collected by parse()
        // when it parses the imported module.
        
    } else {
        ctx.error(stream, DiagCode::E0004, usePath);
        synchronizeToContext(stream, ctx);
        return useDecl;
    }
    
    LOG_PARSER_MINIMAL("Parsed use declaration: '", fullPath, "' as '", aliasStr, "'");
    
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
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else if (isConst) {
        // const requires an initializer
        ctx.error(stream, DiagCode::E1006, stream.peekValue());
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
        ctx.error(stream, DiagCode::E1006, stream.peekValue());
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 6. Parse function body ──────────────────────────────────────────
    StmtPtr body = nullptr;
    
    // Check for block body: { ... }
    if (stream.check(TokenType::LBRACE)) {
        stream.advance(); // Consume '{'
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
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
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
    
    // ─── 3. Parse generic parameters (optional) ─────────────────────────
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse trait implementations (optional) ──────────────────────
    std::vector<TraitRefPtr> traitRefs;
    if (stream.match(TokenType::COLON)) {
        // Check for empty trait list: `: {` (no traits)
        if (stream.check(TokenType::LBRACE)) {
            // No traits - just proceed to body
        } else {
            // Parse trait references separated by ','
            while (!stream.isAtEnd()) {
                // ─── Skip consecutive separators ────────────────────────
                if (stream.consumeTrailing(TokenType::COMMA) > 0) {
                    ctx.error(stream, DiagCode::E1009, stream.peekValue(), "struct traits");
                }
                
                // ─── Check if we've reached a terminator ──────────────────
                if (stream.check(TokenType::LBRACE)) {
                    break; // End of trait list, proceed to body
                }
                
                if (stream.isAtEnd()) {
                    ctx.error(stream, DiagCode::E1005, "}", "struct body", "<EOF>");
                    break;
                }
                
                // Parse a trait reference
                TraitRefPtr traitRef = parseTraitRef(stream, ctx);
                if (traitRef) {
                    traitRefs.push_back(traitRef);
                } else {
                    ctx.error(stream, DiagCode::E1003, "trait reference", stream.peekValue());
                    synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::LBRACE);
                    if (stream.check(TokenType::COMMA)) {
                        stream.advance();
                        continue;
                    } else if (stream.check(TokenType::LBRACE)) {
                        break;
                    } else {
                        // No recovery token found - break to avoid infinite loop
                        break;
                    }
                }
                
                // After parsing a trait, check if we're at the end
                if (stream.check(TokenType::LBRACE)) {
                    break;
                } else if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::LBRACE)) {
                    // If not a comma or brace, we might have an error
                    // Let the loop continue to handle it
                }
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
    
    std::vector<FieldDeclPtr> fields;
    
    // ─── 6. Check for empty body ─────────────────────────────────────────
    if (stream.check(TokenType::RBRACE)) {
        stream.advance(); // Consume '}'
        // Empty struct body - skip to building AST
        auto* structDecl = ctx.arena.make<StructDeclAST>();
        structDecl->loc = loc;
        structDecl->name = name;
        structDecl->genericParams = genericParams;
        structDecl->fields = ctx.arena.makeBuilder<FieldDeclPtr>().build();
        
        auto traitBuilder = ctx.arena.makeBuilder<TraitRefPtr>();
        for (auto* tr : traitRefs) {
            traitBuilder.push_back(tr);
        }
        structDecl->traitRefs = traitBuilder.build();
        
        LOG_PARSER_DETAIL("Parsed empty struct declaration: ", ctx.toString(name));
        return structDecl;
    }
    
    // ─── 7. Parse fields ──────────────────────────────────────────────────
    while (!stream.isAtEnd()) {
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
            ctx.error(stream, DiagCode::E1009, stream.peekValue(), "field declaration");
        }
        
        // ─── Check if we've reached a terminator ──────────────────────────
        if (stream.check(TokenType::RBRACE)) {
            break; // End of fields
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, "}", "struct body", "<EOF>");
            break;
        }
        
        // Parse a field
        FieldDeclPtr field = parseFieldDecl(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
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
    }
    
    // ─── 8. Consume closing brace ────────────────────────────────────────
    if (!stream.check(TokenType::RBRACE)) {
        ctx.error(stream, DiagCode::E1005, "}", "struct body", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACE);
        if (stream.check(TokenType::RBRACE)) {
            stream.advance(); // Consume '}' to recover
        }
    } else {
        stream.advance(); // Consume '}'
    }
    
    // ─── 9. Build the AST node ───────────────────────────────────────────
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
            ctx.error(stream, DiagCode::E1006, stream.peekValue());
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
 * parseEnumDecl()
 *    │
 *    ├── Parse 'enum' keyword
 *    ├── Parse enum name
 *    ├── Parse backing type (optional)
 *    │
 *    ├── Parse enum body
 *    │   │
 *    │   ├── Check for empty body: `}` → return empty enum
 *    │   │
 *    │   └── while (!stream.isAtEnd()) {
 *    │         │
 *    │         ├── Skip consecutive commas
 *    │         │   while (check(COMMA)) {
 *    │         │       count++,
 *    │         │       advance()
 *    │         │   }
 *    │         │
 *    │         ├── If count > 0: report E1009 once
 *    │         │
 *    │         ├── Check for terminator
 *    │         │   if (check(RBRACE)) → break
 *    │         │   if (isAtEnd()) → error, break
 *    │         │
 *    │         ├── Parse variant
 *    │         │
 *    │         └── After variant: loop continues (handles separator in next iteration)
 *    │         }
 *    │
 *    ├── Consume closing brace '}'
 *    │
 *    └── Build AST node
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
    
    std::vector<EnumVariantPtr> variants;
    
    // ─── 5. Check for empty body ─────────────────────────────────────────
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
    
    // ─── 6. Parse variants ────────────────────────────────────────────────
    while (!stream.isAtEnd()) {
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.error(stream, DiagCode::E1009, stream.peekValue(), "enum variant");
        }
        
        // ─── Check if we've reached a terminator ──────────────────────────
        if (stream.check(TokenType::RBRACE)) {
            break; // End of variants
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, "}", "enum body", "<EOF>");
            break;
        }
        
        // Parse a variant
        EnumVariantPtr variant = parseEnumVariant(stream, ctx);
        if (variant) {
            variants.push_back(variant);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACE);
            if (stream.check(TokenType::COMMA)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RBRACE)) {
                break;
            } else {
                // No recovery token found - break to avoid infinite loop
                break;
            }
        }
    }
    
    // ─── 7. Consume closing brace ────────────────────────────────────────
    if (!stream.check(TokenType::RBRACE)) {
        ctx.error(stream, DiagCode::E1005, "}", "enum body", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACE);
        if (stream.check(TokenType::RBRACE)) {
            stream.advance(); // Consume '}' to recover
        }
    } else {
        stream.advance(); // Consume '}'
    }
    
    // ─── 8. Build the AST node ───────────────────────────────────────────
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
        ctx.error(stream, DiagCode::E1006, stream.peekValue());
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
 * parseTraitDecl()
 *     │
 *     ├── Parse 'trait' keyword
 *     ├── Parse trait name
 *     ├── Parse generic parameters (optional)
 *     │
 *     ├── Parse trait body
 *     │   │
 *     │   ├── Check for empty body: `}` → return empty trait
 *     │   │
 *     │   └── while (!stream.isAtEnd()) {
 *     │         │
 *     │         ├── Skip consecutive separators (',' or ';')
 *     │         │   while (check(COMMA) || check(SEMICOLON)) {
 *     │         │       count++,
 *     │         │       advance()
 *     │         │   }
 *     │         │
 *     │         ├── If count > 0: report E1009 once
 *     │         │
 *     │         ├── Check for terminator
 *     │         │   if (check(RBRACE)) → break
 *     │         │   if (isAtEnd()) → error, break
 *     │         │
 *     │         ├── Parse trait field
 *     │         │
 *     │         └── After field: loop continues (handles separator in next iteration)
 *     │         }
 *     │
 *     ├── Consume closing brace '}'
 *     │
 *     └── Build AST node
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
    
    std::vector<TraitFieldPtr> fields;
    
    // ─── 5. Check for empty body ─────────────────────────────────────────
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
    
    // ─── 6. Parse fields ──────────────────────────────────────────────────
    while (!stream.isAtEnd()) {
        // ─── Skip consecutive separators ────────────────────────────────
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
            ctx.error(stream, DiagCode::E1009, stream.peekValue(), "trait field");
        }
        
        // ─── Check if we've reached a terminator ──────────────────────────
        if (stream.check(TokenType::RBRACE)) {
            break; // End of fields
        }
        
        if (stream.isAtEnd()) {
            ctx.error(stream, DiagCode::E1005, "}", "trait body", "<EOF>");
            break;
        }
        
        // Parse a trait field
        TraitFieldPtr field = parseTraitField(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Error already reported, try to recover
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON)) {
                stream.advance();
                continue;
            } else if (stream.check(TokenType::RBRACE)) {
                break;
            } else {
                // No recovery token found - break to avoid infinite loop
                break;
            }
        }
    }
    
    // ─── 7. Consume closing brace ────────────────────────────────────────
    if (!stream.check(TokenType::RBRACE)) {
        ctx.error(stream, DiagCode::E1005, "}", "trait body", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACE);
        if (stream.check(TokenType::RBRACE)) {
            stream.advance(); // Consume '}' to recover
        }
    } else {
        stream.advance(); // Consume '}'
    }
    
    // ─── 8. Build the AST node ───────────────────────────────────────────
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
    
    // Trait fields cannot be nullable or fallible
    // The semantic pass will enforce this
    
    // ─── 5. Build the AST node ───────────────────────────────────────────
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>();
    traitField->loc = loc;
    traitField->name = name;
    traitField->type = type;
    traitField->isConst = isConst;
    traitField->attributes = attrs;  // Attach attributes to the trait field
    
    LOG_PARSER_DETAIL("Parsed trait field: ", ctx.toString(name), 
                      (isConst ? " const" : ""),
                      " with ", attrs.size(), " attributes");
    return traitField;
}

} // namespace parser