/**
 * @file ParseDecl.cpp
 * @brief Implementation of declaration parsers.
 * 
 * This file implements all declaration parsers:
 * - Import, Variable, Function, Struct, Enum, Trait declarations
 * - Field, Enum variant, Trait field parsers
 * 
 * Note: All declaration parsers do NOT consume the terminating semicolon.
 * The caller (parseDecl) is responsible for consuming it.
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <vector>

namespace parser {

// =============================================================================
// parseImportDecl
// =============================================================================

ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("Enter UseDecl");

    SourceLocation loc = stream.currentLoc();

    // 1. Parse 'import' keyword
    if (!stream.check(TokenType::IMPORT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'import', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume(); // Consume 'import'
    
    // 2. Parse the import path
    auto pathParts = parseImportPath(stream, ctx);
    if (pathParts.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedModulePath, stream.currentLoc(),
                                "expected module path, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Build full import path
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    InternedString usePath = ctx.pool.intern(fullPath);
    
    // 3. Determine the alias
    InternedString alias;
    std::string aliasStr;
    
    if (stream.match(TokenType::AS)) {
        Token aliasTok = stream.consume(TokenType::IDENTIFIER);
        if (aliasTok.type != TokenType::EOF_TOKEN) {
            alias = ctx.pool.intern(aliasTok.value);
            aliasStr = std::string(ctx.pool.lookup(alias));
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected alias name after 'as'");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else {
        // Implicit alias: last component of path
        alias = pathParts.back();
        aliasStr = std::string(ctx.pool.lookup(alias));
    }
    
    // 4. Create the ImportDeclAST
    auto* useDecl = ctx.arena.make<ImportDeclAST>();
    useDecl->loc = loc;
    useDecl->path = usePath;
    useDecl->alias = alias;
    
    // 5. Import the module
    if (!ctx.resolver) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedModule, loc,
                                "no module resolver available for '", fullPath, "'");
        return useDecl;
    }

    InternedString filePath = ctx.resolver->resolveUsePath(usePath);
    if (!filePath.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedModule, loc,
                                "module '", fullPath, "' not found");
        return useDecl;
    }

    std::string pathStr = std::string(ctx.pool.lookup(filePath));

    if (ctx.resolver->isParsing(filePath)) {
        ctx.diagnostics.errorAt(DiagCode::Sem_ModuleCycle, loc,
                                "circular module dependency detected: '", fullPath, "'");
        parse(pathStr, "", ctx);
        return useDecl;
    }

    if (!ctx.resolver->getParsedModule(filePath)) {
        std::string source = ctx.resolver->readModuleSource(filePath);
        parse(pathStr, source, ctx);
    }
    
    LOG_PARSER_MINIMAL("Parsed import: '", fullPath, "' as '", aliasStr, "'");
    return useDecl;
}

// =============================================================================
// parseVarDecl
// =============================================================================

VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Parse keyword
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Parse name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Parse type (required)
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Parse initializer
    ExprPtr init = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        init = parseExpr(stream, ctx);
        if (!init) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected initializer expression");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    } else if (isConst) {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingInitializer, stream.currentLoc(),
                                "const variable '", ctx.pool.lookup(name), "' requires an initializer");
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
    
    LOG_PARSER_DETAIL("Parsed variable: ", ctx.pool.lookup(name));
    return varDecl;
}

/// @brief Parse a function declaration.
/// 
/// Grammar:
///   func_decl = ('let' | 'const') IDENTIFIER [ generic_params ] func_type '=' func_body
/// 
/// Rules:
///   1. Block body is allowed: `const f () -> T = { ... }`  ✅
///   2. Function reference is allowed: `const f () -> T = existingFn`  ✅
///   3. Module function reference is allowed: `const f () -> T = module:fn`  ✅
///   4. Generic function instantiation is allowed: `const f () -> T = map<int>`  ✅
///   5. Anonymous function is REJECTED: `const f () -> T = (x int) -> int { ... }`  ❌
///      (use a block body instead: `const f () -> T = { ... }`)
/// 
/// @param stream The token stream.
/// @param ctx The parsing context.
/// @return FuncDeclAST* The parsed function declaration, or nullptr on error.
FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse keyword ──────────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Parse function name ─────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected function name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 3. Parse generic parameters ────────────────────────────────────────
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse function type ─────────────────────────────────────────────
    TypeAST* type = parseFuncType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected function type");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    if (!type->isa<FuncTypeAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected function type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    FuncTypeAST* funcType = type->as<FuncTypeAST>();
    
    // ─── 5. Parse '=' ───────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 6. Parse body ──────────────────────────────────────────────────────
    StmtPtr body = nullptr;
    
    if (stream.check(TokenType::LBRACE)) {
        // ─── Block body ──────────────────────────────────────────────────────
        stream.consume(); // Consume '{'
        ScopedContext bodyGuard(ctx, SyntacticContext::FuncBody, stream.currentLoc());

        body = parseBlock(stream, ctx);
        if (!stream.check(TokenType::RBRACE)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                    "expected '}' to close function body");
            synchronizeTo(stream, ctx, TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE)) {
                stream.consume();
            }
        } else {
            stream.consume(); // Consume '}'
        }
        
    } else {
        // ─── Expression body ─────────────────────────────────────────────
        // Grammar: func_body = expr (NOT func_literal)
        // 
        // Valid expression bodies:
        //   - Named function reference: `f = add`
        //   - Module function reference: `f = module:add`
        //   - Generic instantiation: `f = add<int>`
        //   - Call returning function: `f = getHandler("double")`
        //   - Any expression that evaluates to a function value
        //
        // Invalid expression bodies:
        //   - Anonymous function: `f = (x int) -> int { ... }` ❌
        //   - Block body without braces: `f = { ... }` ❌ (use braces)
        
        if (looksLikeAnonFunc(stream, ctx)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration, 
                                    stream.currentLoc(),
                                    "anonymous function not allowed at declaration site");
            ctx.diagnostics.noteAt(stream.currentLoc(),
                                   "Use a block body instead: '{ ... }'");
            ctx.diagnostics.noteAt(stream.currentLoc(),
                                   "The block body borrows its signature from the function declaration");
            
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.check(TokenType::SEMICOLON)) {
                stream.consume();
            }
            return nullptr;
        }
        
        ExprPtr exprBody = parseExpr(stream, ctx);
        if (!exprBody) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected function body expression");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
        
        // ─── Determine if this is a pure function reference ──────────────
        // A pure function reference is:
        //   - IdentifierExprAST (with optional generic args)
        //   - ModuleAccessExprAST (with optional generic args)
        //   - NOT: FieldAccessExprAST (struct field access)
        //   - NOT: CallExprAST (function call that returns a function)
        //   - NOT: Any other expression
        
        bool isPureFunctionRef = false;
        
        if (exprBody->isa<IdentifierExprAST>()) {
            // Pure function reference: `add` or `add<int>`
            isPureFunctionRef = true;
        } else if (exprBody->isa<ModuleAccessExprAST>()) {
            // Pure module function reference: `module:add` or `module:add<int>`
            isPureFunctionRef = true;
        } else if (exprBody->isa<FieldAccessExprAST>()) {
            // Field access is NOT a pure function reference
            // This would be `struct.field` - struct field access
            // Such references are rejected because the struct may outlive the function
            isPureFunctionRef = true;
        } else if (exprBody->isa<CallExprAST>()) {
            // Call that returns a function is NOT a pure reference
            // Example: `getHandler("double")`
            isPureFunctionRef = true;
        }
        
        if (isPureFunctionRef) {
            // ─── Pure function reference ──────────────────────────────────
            // Store as FuncRefStmtAST for semantic validation
            auto* refStmt = ctx.arena.make<FuncRefStmtAST>();
            refStmt->loc = exprBody->loc;
            refStmt->target = exprBody;
            body = refStmt;
            
            LOG_PARSER_DETAIL("Parsed function reference: ", ctx.pool.lookup(name));
        } else {
            // ─── General expression body ──────────────────────────────────
            // Wrap in ReturnStmtAST
            auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
            returnStmt->loc = exprBody->loc;
            returnStmt->value = exprBody;
            body = returnStmt;
            
            LOG_PARSER_DETAIL("Parsed expression body for: ", ctx.pool.lookup(name));
        }
    }
    
    // ─── Build AST ──────────────────────────────────────────────────────────
    auto* funcDecl = ctx.arena.make<FuncDeclAST>();
    funcDecl->loc = loc;
    funcDecl->name = name;
    funcDecl->keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    funcDecl->genericParams = genericParams;
    funcDecl->funcType = funcType;
    funcDecl->body = body;
    funcDecl->isConst = isConst;
    funcDecl->type = funcType;
    
    LOG_PARSER_DETAIL("Parsed function: ", ctx.pool.lookup(name));
    return funcDecl;
}

// =============================================================================
// parseStructDecl
// =============================================================================

StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // 1. Parse 'struct' keyword
    if (!stream.check(TokenType::STRUCT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'struct', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    // 2. Parse struct name
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected struct name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // 3. Parse generic parameters
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // 4. Parse trait implementations
    std::vector<NamedTypeAST*> traitRefs;
    if (stream.match(TokenType::COLON)) {
        if (stream.consumeTrailing(TokenType::COMMA) > 0) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                    "unexpected trailing comma in trait list");
        }
        
        while (!stream.isAtEnd() && !stream.check(TokenType::LBRACE)) {
            NamedTypeAST* traitRef = parseNamedType(stream, ctx)->as<NamedTypeAST>();
            if (traitRef) {
                traitRefs.push_back(traitRef);
            } else {
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::LBRACE);
            }
            
            if (stream.consumeTrailing(TokenType::COMMA) > 1) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                        "unexpected trailing comma in trait list");
            }
        }
    }
    
    // 5. Parse struct body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for struct body");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::StructBody, stream.currentLoc());

    std::vector<FieldDeclPtr> fields;
    
    if (stream.check(TokenType::RBRACE)) {
        stream.consume();
        
        auto* structDecl = ctx.arena.make<StructDeclAST>();
        structDecl->loc = loc;
        structDecl->name = name;
        structDecl->genericParams = genericParams;
        structDecl->fields = ctx.arena.makeBuilder<FieldDeclPtr>().build();
        
        auto traitBuilder = ctx.arena.makeBuilder<NamedTypeAST*>();
        for (auto* tr : traitRefs) {
            traitBuilder.push_back(tr);
        }
        structDecl->traitRefs = traitBuilder.build();
        
        LOG_PARSER_DETAIL("Parsed empty struct: ", ctx.pool.lookup(name));
        return structDecl;
    }
    
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                "unexpected semicolon in struct body");
    }
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        FieldDeclPtr field = parseFieldDecl(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
        
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected duplicate semicolon in struct body");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close struct body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    auto* structDecl = ctx.arena.make<StructDeclAST>();
    structDecl->loc = loc;
    structDecl->name = name;
    structDecl->genericParams = genericParams;
    
    auto fieldBuilder = ctx.arena.makeBuilder<FieldDeclPtr>();
    for (auto* f : fields) {
        fieldBuilder.push_back(f);
    }
    structDecl->fields = fieldBuilder.build();
    
    auto traitBuilder = ctx.arena.makeBuilder<NamedTypeAST*>();
    for (auto* tr : traitRefs) {
        traitBuilder.push_back(tr);
    }
    structDecl->traitRefs = traitBuilder.build();
    
    LOG_PARSER_DETAIL("Parsed struct: ", ctx.pool.lookup(name));
    return structDecl;
}

// =============================================================================
// parseFieldDecl
// =============================================================================

FieldDeclPtr parseFieldDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse const modifier ────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    
    // ─── 3. Parse field name ────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected field name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // ─── 4. Parse field type ────────────────────────────────────────────────
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected field type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 5. Parse default value ─────────────────────────────────────────────
    ExprPtr defaultVal = nullptr;
    StmtPtr defaultBody = nullptr;
    
    if (stream.match(TokenType::ASSIGN)) {
        // ─── 5a. Check for block body ──────────────────────────────────────
        if (stream.check(TokenType::LBRACE)) {
            stream.consume();
            
            // Push struct field context for the body
            ScopedContext bodyGuard(ctx, SyntacticContext::FieldBody, stream.currentLoc());
            
            defaultBody = parseBlock(stream, ctx);
            if (!stream.check(TokenType::RBRACE)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected '}' to close block body");
                synchronizeTo(stream, ctx, TokenType::RBRACE);
                if (stream.check(TokenType::RBRACE)) {
                    stream.consume();
                }
            } else {
                stream.consume(); // Consume '}'
            }
            
        } else {
            // ─── 5b. Expression default ─────────────────────────────────────
            if (looksLikeAnonFunc(stream, ctx)) {
                // Anonymous function at declaration site
                ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                                        stream.currentLoc(),
                                        "anonymous function not allowed at declaration site");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "Use a block body instead: '{ ... }'");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "The block body borrows its signature from the field type");
                
                synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE, TokenType::COMMA);
                if (stream.checkAny(TokenType::SEMICOLON, TokenType::COMMA)) {
                    stream.consume();
                }
                return nullptr;
            }
            
            // Parse as an expression (function reference, etc.)
            defaultVal = parseExpr(stream, ctx);
            if (!defaultVal) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                        "expected default value expression");
                synchronizeToContext(stream, ctx);
                return nullptr;
            }
        }
    }
    
    // ─── 6. Build AST ──────────────────────────────────────────────────────
    auto* fieldDecl = ctx.arena.make<FieldDeclAST>();
    fieldDecl->loc = loc;
    fieldDecl->name = name;
    fieldDecl->type = type;
    fieldDecl->defaultVal = defaultVal;
    fieldDecl->defaultBody = defaultBody;
    fieldDecl->isConst = isConst;
    fieldDecl->attributes = attrs;
    
    if (doc.has_value()) {
        fieldDecl->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed field: ", ctx.pool.lookup(name));
    return fieldDecl;
}

// =============================================================================
// parseEnumDecl
// =============================================================================

EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::ENUM)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'enum', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected enum name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    PrimitiveTypeAST* backingType = nullptr;
    if (stream.match(TokenType::COLON)) {
        TypePtr type = parseType(stream, ctx);
        if (type && type->isa<PrimitiveTypeAST>()) {
            backingType = type->as<PrimitiveTypeAST>();
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected integer backing type, got '", stream.peekValue(), "'");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for enum body");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::EnumBody, stream.currentLoc());

    std::vector<EnumVariantPtr> variants;
    
    if (stream.check(TokenType::RBRACE)) {
        stream.consume();
        auto* enumDecl = ctx.arena.make<EnumDeclAST>();
        enumDecl->loc = loc;
        enumDecl->name = name;
        enumDecl->backingType = backingType;
        enumDecl->variants = ctx.arena.makeBuilder<EnumVariantPtr>().build();
        
        LOG_PARSER_DETAIL("Parsed empty enum: ", ctx.pool.lookup(name));
        return enumDecl;
    }
    
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                "unexpected semicolon in enum body");
    }
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        EnumVariantPtr variant = parseEnumVariant(stream, ctx);
        if (variant) {
            variants.push_back(variant);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
        
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected duplicate semicolon in enum body");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close enum body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    auto* enumDecl = ctx.arena.make<EnumDeclAST>();
    enumDecl->loc = loc;
    enumDecl->name = name;
    enumDecl->backingType = backingType;
    
    auto builder = ctx.arena.makeBuilder<EnumVariantPtr>();
    for (auto* v : variants) {
        builder.push_back(v);
    }
    enumDecl->variants = builder.build();
    
    LOG_PARSER_DETAIL("Parsed enum: ", ctx.pool.lookup(name));
    return enumDecl;
}

// =============================================================================
// parseEnumVariant
// =============================================================================

EnumVariantPtr parseEnumVariant(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variant name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    if (!stream.check(TokenType::INT_LITERAL) &&
        !stream.check(TokenType::HEX_LITERAL) &&
        !stream.check(TokenType::BINARY_LITERAL)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected integer literal, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token valueTok = stream.consume();
    int64_t value = std::stoll(valueTok.value);
    
    auto* variant = ctx.arena.make<EnumVariantAST>(name, value);
    variant->loc = loc;
    variant->attributes = attrs;
    if (doc.has_value()) {
        variant->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed enum variant: ", ctx.pool.lookup(name));
    return variant;
}

// =============================================================================
// parseTraitDecl
// =============================================================================

TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::TRAIT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'trait', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected trait name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for trait body");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::TraitBody, stream.currentLoc());

    std::vector<TraitFieldPtr> fields;
    
    if (stream.check(TokenType::RBRACE)) {
        stream.consume();
        auto* traitDecl = ctx.arena.make<TraitDeclAST>();
        traitDecl->loc = loc;
        traitDecl->name = name;
        traitDecl->genericParams = genericParams;
        traitDecl->fields = ctx.arena.makeBuilder<TraitFieldPtr>().build();
        
        LOG_PARSER_DETAIL("Parsed empty trait: ", ctx.pool.lookup(name));
        return traitDecl;
    }
    
    if (stream.consumeTrailing(TokenType::SEMICOLON) > 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                "unexpected semicolon in trait body");
    }
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        TraitFieldPtr field = parseTraitField(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.check(TokenType::SEMICOLON)) {
                stream.consume();
                continue;
            } else if (stream.check(TokenType::RBRACE)) {
                break;
            } else {
                break;
            }
        }
        
        if (stream.consumeTrailing(TokenType::SEMICOLON) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected duplicate semicolon in trait body");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close trait body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    auto* traitDecl = ctx.arena.make<TraitDeclAST>();
    traitDecl->loc = loc;
    traitDecl->name = name;
    traitDecl->genericParams = genericParams;
    
    auto builder = ctx.arena.makeBuilder<TraitFieldPtr>();
    for (auto* f : fields) {
        builder.push_back(f);
    }
    traitDecl->fields = builder.build();
    
    LOG_PARSER_DETAIL("Parsed trait: ", ctx.pool.lookup(name));
    return traitDecl;
}

// =============================================================================
// parseTraitField
// =============================================================================

TraitFieldPtr parseTraitField(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    bool isConst = stream.match(TokenType::CONST);
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected trait field name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    TypePtr type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected trait field type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>();
    traitField->loc = loc;
    traitField->name = name;
    traitField->type = type;
    traitField->isConst = isConst;
    traitField->attributes = attrs;
    if (doc.has_value()) {
        traitField->doc = doc;
    }
    
    LOG_PARSER_DETAIL("Parsed trait field: ", ctx.pool.lookup(name));
    return traitField;
}

} // namespace parser