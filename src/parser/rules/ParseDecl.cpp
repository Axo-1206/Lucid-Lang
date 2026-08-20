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
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    // Don't consume semicolons here - let each declaration parser handle it
    // or the caller handle it based on the declaration type.
    
    if (stream.isAtEnd()) {
        return nullptr;
    }

    auto doc = harvestDocComment(stream, ctx);
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    DeclAST* decl = nullptr;
    bool isFuncDecl = false;
    bool isVarDecl = false;
    
    if (stream.check(TokenType::IMPORT)) {
        if (ctx.currentContext() == SyntacticContext::FuncBody) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_InvalidAttributeTarget,
                                    stream.currentLoc(),
                                    "import cannot appear inside function body");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
        decl = parseImportDecl(stream, ctx);
    } else if (stream.check(TokenType::STRUCT)) {
        decl = parseStructDecl(stream, ctx);
    } else if (stream.check(TokenType::ENUM)) {
        decl = parseEnumDecl(stream, ctx);
    } else if (stream.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(stream, ctx);
    } else if (stream.check(TokenType::LET) || stream.check(TokenType::CONST)) {
        if (looksLikeFuncDecl(stream, ctx)) {
            decl = parseFuncDecl(stream, ctx);
            isFuncDecl = true;
        } else {
            decl = parseVarDecl(stream, ctx);
            isVarDecl = true;
        }
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken,
                                stream.currentLoc(),
                                "unexpected token '", stream.peekValue(), 
                                "' - expected declaration");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }

    if (stream.consumeTrailing(TokenType::COMMA) == 0) {
        if (isFuncDecl) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                                "expected ';' after function declaration");
        } else if (isVarDecl) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected ';' after variable declaration");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ';' after declaration");
        }
    }
    
    if (decl) {
        decl->attributes = attrs;
        if (doc.has_value()) {
            decl->doc = doc;
        }
    }
    
    return decl;
}

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
    
    // ─── Build full import path ──────────────────────────────────────────
    // This is the user‑written logical path (e.g., "io.math")
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    InternedString importPath = ctx.pool.intern(fullPath);
    
    // 3. Determine the alias
    InternedString alias;
    std::string aliasStr;
    
    if (stream.match(TokenType::AS)) {
        Token aliasTok = stream.peek();
        stream.consume();
        if (aliasTok.type == TokenType::IDENTIFIER) {
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
    auto* importDecl = ctx.arena.make<ImportDeclAST>(importPath, alias);
    importDecl->loc = loc;
    
    // 5. Resolve the import path to a file path ──────────────────────────
    // This converts "io.math" → "io/math.luc"
    // The resolved path is the canonical key used by the CLI, Interpreter, and LSP.
    if (!ctx.resolver) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedModule, loc,
                                "no module resolver available for '", fullPath, "'");
        return importDecl;
    }

    InternedString resolvedPath = ctx.resolver->resolveImportPath(importPath);
    if (!resolvedPath.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedModule, loc,
                                "module '", fullPath, "' not found");
        return importDecl;
    }

    // ─── Store resolved path on current module ──────────────────────
    // This is NOT the user‑written path ("io.math").
    // This is the resolved filesystem path ("io/math.luc").
    // Used by: CLI (DependencyGraph), Interpreter (ModuleLoader), LSP
    if (ctx.currentModule) {
        ctx.currentModule->imports.push_back(resolvedPath);
    } else {
        // Should never happen — parse() always sets ctx.currentModule
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedModule, loc,
                                "internal error: no current module for import '", fullPath, "'");
    }

    // 6. Parse the imported module ────────────────────────────────────────
    // parse() will handle:
    //   - Cache checking
    //   - Circular import detection (via ScopedParsingGuard + isParsing())
    //   - Lexing
    //   - Actual parsing
    //   - Registering the module in ModuleResolver::parsedModules_
    if (!ctx.resolver->getParsedModule(resolvedPath)) {
        std::string pathStr = std::string(ctx.pool.lookup(resolvedPath));
        std::string source = ctx.resolver->readModuleSource(resolvedPath);
        
        // parse() will detect cycles via isParsing() + ScopedParsingGuard
        parse(pathStr, source, ctx);
    }
    // If already parsed, we just use the cached version (already loaded)
    
    LOG_PARSER_MINIMAL("Parsed import: '", fullPath, "' as '", aliasStr, "'");
    return importDecl;
}

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
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
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
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Parse initializer
    ExprAST* init = nullptr;
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
    
    // Create VarDeclAST using constructor (all parser fields immutable)
    auto* varDecl = ctx.arena.make<VarDeclAST>(name, keyword, type, init);
    varDecl->loc = loc;
    
    LOG_PARSER_DETAIL("Parsed variable: ", ctx.pool.lookup(name));
    return varDecl;
}

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
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
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
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse function type ─────────────────────────────────────────────
    // For function declarations, the leading cluster (before first `->`) 
    // has parameter names. parseFuncType() always uses allowNames=false,
    // so we need to parse the leading cluster separately.
    
    std::vector<ParamAST*> leadingParams;
    
    // Parse the leading cluster - this one has names
    while (stream.check(TokenType::LPAREN)) {
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx, true);  // allowNames = true
        for (auto* p : groupParams) {
            leadingParams.push_back(p);
        }
        if (stream.check(TokenType::ARROW)) {
            break;
        }
        // No '->' means more adjacent groups
    }
    
    // Now parse the rest of the function type (after the first `->`)
    // This will use parseFuncType which always uses allowNames=false
    TypeAST* restType = nullptr;
    
    if (stream.check(TokenType::ARROW)) {
        // There's a `->`, so parse the rest as a function type
        // But we need to parse it carefully - parseFuncType would parse the
        // entire function type, but we already have the leading cluster.
        // So we need to parse just the return type part.
        stream.consume(); // Consume '->'
        restType = parseType(stream, ctx);  // This could be another FuncTypeAST
        if (!restType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected return type");
            synchronizeToContext(stream, ctx);
            return nullptr;
        }
    }
    
    // ─── 5. Build the FuncTypeAST ──────────────────────────────────────────
    // The FuncTypeAST has the leading params as its params,
    // and the rest type as its return type.
    auto* funcType = ctx.arena.make<FuncTypeAST>();
    funcType->loc = loc;
    
    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : leadingParams) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();
    funcType->returnType = restType;
    funcType->hasArrow = (restType != nullptr);
    
    // ─── 6. Parse '=' and body ──────────────────────────────────────────────
    StmtAST* body = nullptr;
    
    if (stream.match(TokenType::ASSIGN)) {
        // ─── Has body ──────────────────────────────────────────────────────
        if (stream.check(TokenType::LBRACE)) {
            // ─── Block body ──────────────────────────────────────────────────
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
            // ─── Expression body ──────────────────────────────────────────
            if (looksLikeAnonFunc(stream, ctx)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration, 
                                        stream.currentLoc(),
                                        "anonymous function not allowed at declaration site");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "Use a block body instead: '{ ... }'");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "The block body borrows its signature from the function declaration");
                
                synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
                return nullptr;
            }
            
            ExprAST* exprBody = parseExpr(stream, ctx);
            if (!exprBody) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                        "expected function body expression");
                synchronizeToContext(stream, ctx);
                return nullptr;
            }
            
            // ─── Determine if this is a pure function reference ──────────
            bool isPureFunctionRef = false;
            
            if (exprBody->isa<IdentifierExprAST>() ||
                exprBody->isa<ModuleAccessExprAST>() ||
                exprBody->isa<FieldAccessExprAST>() ||
                exprBody->isa<CallExprAST>()) {
                isPureFunctionRef = true;
            }
            
            if (isPureFunctionRef) {
                auto* refStmt = ctx.arena.make<FuncRefStmtAST>();
                refStmt->loc = exprBody->loc;
                refStmt->target = exprBody;
                body = refStmt;
                
                LOG_PARSER_DETAIL("Parsed function reference: ", ctx.pool.lookup(name));
            } else {
                auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
                returnStmt->loc = exprBody->loc;
                returnStmt->value = exprBody;
                body = returnStmt;
                
                LOG_PARSER_DETAIL("Parsed expression body for: ", ctx.pool.lookup(name));
            }
        }
        
    } else {
        // ─── No '=' - foreign function ─────────────────────────────────────
        body = nullptr;
    }
    
    // ─── 7. Build FuncDeclAST ────────────────────────────────────────────────
    auto* funcDecl = ctx.arena.make<FuncDeclAST>(name, keyword, genericParams, funcType, body);
    funcDecl->loc = loc;
    
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
    ArenaSpan<GenericParamDeclAST*> genericParams;
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

    std::vector<FieldDeclAST*> fields;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        FieldDeclAST* field = parseFieldDecl(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close struct body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    // Build field span
    auto fieldBuilder = ctx.arena.makeBuilder<FieldDeclAST*>();
    for (auto* f : fields) {
        fieldBuilder.push_back(f);
    }
    
    // Build trait refs span
    auto traitBuilder = ctx.arena.makeBuilder<NamedTypeAST*>();
    for (auto* tr : traitRefs) {
        traitBuilder.push_back(tr);
    }
    
    // Create StructDeclAST using constructor
    auto* structDecl = ctx.arena.make<StructDeclAST>(
        name,
        genericParams,
        fieldBuilder.build(),
        traitBuilder.build()
    );
    structDecl->loc = loc;
    
    LOG_PARSER_DETAIL("Parsed struct: ", ctx.pool.lookup(name));
    return structDecl;
}

FieldDeclAST* parseFieldDecl(TokenStream& stream, ParserContext& ctx) {
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
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected field type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 5. Parse default value ─────────────────────────────────────────────
    ExprAST* defaultVal = nullptr;
    StmtAST* defaultBody = nullptr;
    
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
    
    // ─── 6. Build AST using constructor ──────────────────────────────────
    auto* fieldDecl = ctx.arena.make<FieldDeclAST>(name, type, defaultVal, defaultBody, isConst);
    fieldDecl->loc = loc;
    fieldDecl->attributes = attrs;
    
    if (doc.has_value()) {
        fieldDecl->doc = doc;
    }

    if (stream.consumeTrailing(TokenType::COMMA) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ';' after field declaration");
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
        TypeAST* type = parseType(stream, ctx);
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

    std::vector<EnumVariantAST*> variants;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        EnumVariantAST* variant = parseEnumVariant(stream, ctx);
        if (variant) {
            variants.push_back(variant);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close enum body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    // Build variant span
    auto builder = ctx.arena.makeBuilder<EnumVariantAST*>();
    for (auto* v : variants) {
        builder.push_back(v);
    }
    
    // Create EnumDeclAST using constructor
    auto* enumDecl = ctx.arena.make<EnumDeclAST>(name, builder.build(), backingType);
    enumDecl->loc = loc;
    
    LOG_PARSER_DETAIL("Parsed enum: ", ctx.pool.lookup(name));
    return enumDecl;
}

EnumVariantAST* parseEnumVariant(TokenStream& stream, ParserContext& ctx) {
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
    
    // Create EnumVariantAST using constructor
    auto* variant = ctx.arena.make<EnumVariantAST>(name, value);
    variant->loc = loc;
    variant->attributes = attrs;
    if (doc.has_value()) {
        variant->doc = doc;
    }
        
    if (stream.consumeTrailing(TokenType::COMMA) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ';' after enum variant declaration");
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
    
    ArenaSpan<GenericParamDeclAST*> genericParams;
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

    std::vector<TraitFieldDeclAST*> fields;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        TraitFieldDeclAST* field = parseTraitField(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "unexpected EOF - expected '}' to close trait body");
    } else {
        stream.consume(); // Consume '}'
    }
    
    // Build field span
    auto builder = ctx.arena.makeBuilder<TraitFieldDeclAST*>();
    for (auto* f : fields) {
        builder.push_back(f);
    }
    
    // Create TraitDeclAST using constructor
    auto* traitDecl = ctx.arena.make<TraitDeclAST>(name, genericParams, builder.build());
    traitDecl->loc = loc;
    
    LOG_PARSER_DETAIL("Parsed trait: ", ctx.pool.lookup(name));
    return traitDecl;
}

TraitFieldDeclAST* parseTraitField(TokenStream& stream, ParserContext& ctx) {
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
    
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected trait field type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Create TraitFieldDeclAST using constructor
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>(name, type, isConst);
    traitField->loc = loc;
    traitField->attributes = attrs;
    if (doc.has_value()) {
        traitField->doc = doc;
    }

    if (stream.consumeTrailing(TokenType::COMMA) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ';' after trait field declaration");
    }
    
    LOG_PARSER_DETAIL("Parsed trait field: ", ctx.pool.lookup(name));
    return traitField;
}

} // namespace parser