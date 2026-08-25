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
#include "core/SourceLocation.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "parser/Parser.hpp"

#include <vector>

namespace parser {

// =============================================================================
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    if (stream.isAtEnd()) {
        return nullptr;
    }

    auto doc = harvestDocComment(stream, ctx);
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);
    
    SourceLocation loc = stream.currentLoc();

    DeclAST* decl = nullptr;
    bool isFuncDecl = false;
    bool isVarDecl = false;
    
    // ─── Pure dispatcher: caller guarantees current token is a declaration keyword ───
    if (stream.check(TokenType::IMPORT)) {
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
        // Should never happen - caller filters before calling parseDecl
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken,
                                stream.currentLoc(),
                                "internal error: parseDecl called with non-declaration token '", 
                                stream.peekValue(), "'");
        // Consume the token to avoid infinite loops
        stream.consume();
        return nullptr;
    }

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) {
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
        decl->loc = loc;
    }
    
    return decl;
}

ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();

    // 1. Parse 'import' keyword
    if (!stream.check(TokenType::IMPORT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'import', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume 'import'
    
    // 2. Parse the import path
    auto pathParts = parseImportPath(stream, ctx);
    if (pathParts.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedModulePath, stream.currentLoc(),
                                "expected module path, got '", stream.peekValue(), "'");
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
            return nullptr;
        }
    } else {
        // Implicit alias: last component of path
        alias = pathParts.back();
        aliasStr = std::string(ctx.pool.lookup(alias));
    }
    
    // 4. Create the ImportDeclAST
    auto* importDecl = ctx.arena.make<ImportDeclAST>(importPath, alias);
    
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
    
    Trace::detail("Parsed import: '", fullPath, "' as '", aliasStr, "'");
    return importDecl;
}

VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx) {
    // Parse keyword
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const', got '", stream.peekValue(), "'");
        return nullptr;
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    InternedString name;
    TypeAST* type = nullptr;

    // ─── Parse name ────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        // The name specifically may just be missing rather than the whole
        // declaration being garbage - try the type directly at this
        // position. If it parses cleanly, we know exactly what happened
        // and can recover without throwing away any tokens.
        type = parseType(stream, ctx);
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected variable name, got '", stream.peekValue(), "'");
            return nullptr;
        }

        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variable name before type");
        // No symbol to record - parse has diagnostics, so semantic analysis
        // never runs on this AST. Still a defined, lookup-safe value.
        name = ctx.pool.intern("");

        // A stray token between the recovered type and '=' / ';' means the
        // type and name were most likely written in the wrong order
        // (e.g. `const int x = 5;`). Report it and resync past it without
        // wandering into the next declaration/statement.
        if (!stream.check(TokenType::ASSIGN) && !stream.check(TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token '", stream.peekValue(), "'");
            synchronizeToDeclBoundary(stream, ctx, {TokenType::ASSIGN, TokenType::SEMICOLON});
        }
    } else {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);

        // ─── Parse type (required) ─────────────────────────────────────
        type = parseType(stream, ctx);
        if (!type) {
            if (stream.check(TokenType::ASSIGN)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected type for variable declaration '", ctx.pool.lookup(name), "'");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected type for variable declaration '", ctx.pool.lookup(name), "', but got '", stream.peekValue(), "'");
                // Only stop at '=' / ';' for this construct, plus - always -
                // the start of the next declaration/statement. A fixed
                // token set here would happily swallow an entire following
                // `struct { ... }` looking for one of its targets.
                synchronizeToDeclBoundary(stream, ctx, {TokenType::ASSIGN, TokenType::SEMICOLON});

                if (!stream.check(TokenType::ASSIGN)) {
                    if (!stream.check(TokenType::SEMICOLON)) {
                        // Stopped at a new declaration/statement keyword,
                        // not at '=' or ';' - this declaration was
                        // abandoned mid-way, which is more specific and
                        // more useful than repeating "expected type".
                        ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                                "incomplete variable declaration '", ctx.pool.lookup(name), "'");
                    }
                    return nullptr;
                }
            }
        }
    }
    
    // ─── Parse initializer ─────────────────────────────────────────────
    ExprAST* init = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        init = parseExpr(stream, ctx);
        if (!init) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected initializer expression");
            return nullptr;
        }
    } 
    if (isConst && !init) {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingInitializer, stream.currentLoc(),
                                "const variable '", ctx.pool.lookup(name), "' requires an initializer");
        return nullptr;
    }
    
    // Create VarDeclAST using constructor (all parser fields immutable)
    auto* varDecl = ctx.arena.make<VarDeclAST>(name, keyword, type, init);
    
    return varDecl;
}

FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx) {
    // ─── 1. Parse keyword ──────────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const', got '", stream.peekValue(), "'");
        return nullptr;
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;
    
    // ─── 2. Parse function name ─────────────────────────────────────────────
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else if (stream.check(TokenType::LESS) || stream.check(TokenType::LPAREN)) {
        // The name is missing, but generics/a parameter list still follow -
        // exactly like parseVarDecl continuing when a type parses even
        // though the name didn't, this is decisive enough to keep going
        // instead of throwing away the rest of the signature.
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected function name");
        name = ctx.pool.intern("");
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected function name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 3. Parse generic parameters ────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // ─── 4. Parse function type ─────────────────────────────────────────────
    SourceLocation funcTypeLoc = stream.currentLoc();
    std::vector<ParamAST*> leadingParams;
    
    // Parse the leading cluster - this one has names
    while (stream.check(TokenType::LPAREN)) {
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx, true);
        for (auto* p : groupParams) {
            leadingParams.push_back(p);
        }
        if (stream.check(TokenType::ARROW)) {
            break;
        }
    }
    
    // ─── Parse the rest of the function type ──────────────────────────────
    TypeAST* restType = nullptr;
    bool sawArrow = false;
    
    if (stream.check(TokenType::ARROW)) {
        stream.consume(); // Consume '->'
        sawArrow = true;
        restType = parseType(stream, ctx);
        if (!restType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected return type");

            // A broken return type shouldn't cost us the rest of the
            // declaration. Look for '{' (block body), '=' (expression
            // body), or ';' (foreign fn) - and never wander past the start
            // of the next declaration/statement while looking for them.
            synchronizeToDeclBoundary(stream, ctx,
                {TokenType::LBRACE, TokenType::ASSIGN, TokenType::SEMICOLON});

            if (!stream.check(TokenType::LBRACE) &&
                !stream.check(TokenType::ASSIGN) &&
                !stream.check(TokenType::SEMICOLON)) {
                // Nothing recoverable found before the next declaration or
                // statement - the declaration was abandoned mid-way.
                ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                        "incomplete function declaration '", ctx.pool.lookup(name), "'");
                return nullptr;
            }
            // restType stays nullptr (error type) - fall through and still
            // parse/diagnose the body below instead of losing it.
        }
    }
    
    // ─── 5. Build the FuncTypeAST ──────────────────────────────────────────
    auto* funcType = ctx.arena.make<FuncTypeAST>();
    
    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : leadingParams) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();
    funcType->returnType = restType;
    funcType->hasArrow = sawArrow;
    funcType->loc = funcTypeLoc;
    
    // ─── 6. Parse body ──────────────────────────────────────────────────────
    StmtAST* body = nullptr;
    bool hasExplicitAssign = false;
    
    // ─── 6a. Check for '=' ──────────────────────────────────────────────────
    if (stream.match(TokenType::ASSIGN)) {
        hasExplicitAssign = true;
    }
    
    // ─── 6b. Parse body if we see '{' ─────────────────────────────────────
    if (stream.check(TokenType::LBRACE)) {
        // If there was no '=', report the error but still parse the body
        if (!hasExplicitAssign) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.previousLoc(),
                                    "expected '=' before function body");
            // Continue parsing the body anyway to provide better error recovery
            // We'll still report the error, but we'll have a partial AST
        }
        
        // ─── Parse block body ──────────────────────────────────────────────
        // parseBlock handles consuming '{' and matching '}'
        ScopedContext bodyGuard(ctx, SyntacticContext::FuncBody, stream.currentLoc());
        body = parseBlock(stream, ctx);
        if (!body) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                    "expected block body");
            return nullptr;
        }
        
    } else if (hasExplicitAssign) {
        // ─── Has '=' but no '{' - expression body ─────────────────────────
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
            return nullptr;
        }
        
        // ─── Determine if this is a pure function reference ──────────────
        bool isPureFunctionRef = false;
        
        if (exprBody->isa<IdentifierExprAST>() ||
            exprBody->isa<ModuleAccessExprAST>() ||
            exprBody->isa<FieldAccessExprAST>() ||
            exprBody->isa<ComposeExprAST>() ||
            exprBody->isa<CallExprAST>()) {
            isPureFunctionRef = true;
        }
        
        if (isPureFunctionRef) {
            auto* refStmt = ctx.arena.make<FuncRefStmtAST>();
            refStmt->loc = exprBody->loc;
            refStmt->target = exprBody;
            body = refStmt;
        } else {
            auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
            returnStmt->loc = exprBody->loc;
            returnStmt->value = exprBody;
            body = returnStmt;
        }
        
    } else {
        // ─── No '=' and no '{' - foreign function ──────────────────────────
        // This is a valid foreign function declaration.
        // The body remains nullptr, and the caller will handle the ';'
        body = nullptr;
    }
    
    // ─── 7. Build FuncDeclAST ────────────────────────────────────────────────
    auto* funcDecl = ctx.arena.make<FuncDeclAST>(name, keyword, genericParams, funcType, body);
    
    return funcDecl;
}

// =============================================================================
// parseStructDecl
// =============================================================================

StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'struct' keyword
    if (!stream.check(TokenType::STRUCT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'struct', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    // 2. Parse struct name
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else if (stream.check(TokenType::LESS) || stream.check(TokenType::LBRACE) || stream.check(TokenType::COLON)) {
        // The name is missing, but generics, trait list, or body still follow
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected struct name");
        name = ctx.pool.intern("");
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected struct name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
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
            
            int count = stream.consumeTrailing(TokenType::COMMA);
            if (count == 0) {
                if (!stream.check(TokenType::LBRACE)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                        "expected a type after ',' in trait list");
                } else {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ',' to separate traits");
                }
            } else if (count == 2) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.previousLoc(),
                                        "expected a type after ',' in trait list");
            } else if (count > 3) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                        "unexpected trailing comma in trait list");
            }
        }
    }
    
    // 5. Parse struct body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for struct body");
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::StructBody, stream.currentLoc());

    std::vector<FieldDeclAST*> fields;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // filter all invalid token in this context
        // a field starts with IDENTIFIER, CONST, or AT_SIGN (attributes), ends with SEMICOLON
        if (!stream.checkAny(TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                            "unexpected token(s) '", stream.peekValue(), "' inside struct body");
            
            // Synchronize to nearest valid field to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // consume stray ';'
        if (stream.check(TokenType::SEMICOLON)) {
            stream.consume();
            continue;
        }

        FieldDeclAST* field = parseFieldDecl(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Synchronize to nearest valid field to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::RBRACE);
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
    
    return structDecl;
}

/// NOTE: parseStructDecl already filter the context for us, we will
///  start with tokens IDENTIFIER or CONST in this function
FieldDeclAST* parseFieldDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse const modifier ────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    
    // ─── 3. Parse field name and type ─────────────────────────────────────
    InternedString name;
    
    // Field name is always provided (filtered by parseStructDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    }

    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        /// Check the next token
        if (stream.check(TokenType::ASSIGN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                            "expected field type before '='");
        } else if (stream.check(TokenType::RBRACE) || stream.match(TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                    "incomplete field declaration '", ctx.pool.lookup(name), "'");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected field type, got '", stream.peekValue(), "'");
        }
        
        return nullptr;
    }
    
    // ─── 5. Parse default value ─────────────────────────────────────────────
    ExprAST* defaultVal = nullptr;
    StmtAST* defaultBody = nullptr;
    
    if (stream.match(TokenType::ASSIGN)) {
        // ─── 5a. Check for block body ──────────────────────────────────────
        if (stream.check(TokenType::LBRACE)) {
            
            // Push struct field context for the body
            ScopedContext bodyGuard(ctx, SyntacticContext::FieldBody, stream.currentLoc());
            
            defaultBody = parseBlock(stream, ctx);

            /// Check if parseBlock actually got a valid block
            /// NOTE: this should not happen (fatal error)
            if (!defaultBody) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected block body");
                return nullptr;
            }
        } else {
            // ─── 5b. Expression default ─────────────────────────────────────
            if (looksLikeAnonFunc(stream, ctx)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                                        stream.currentLoc(),
                                        "anonymous function not allowed at declaration site");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "Use a block body instead: '{ ... }'");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "The block body borrows its signature from the field type");
                return nullptr;
            }
            
            defaultVal = parseExpr(stream, ctx);
            if (!defaultVal) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                        "expected default value expression");
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

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after field declaration");
    }
    
    return fieldDecl;
}

// =============================================================================
// parseEnumDecl
// =============================================================================

EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'enum' keyword
    if (!stream.check(TokenType::ENUM)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'enum', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    // 2. Parse enum name
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else if (stream.check(TokenType::COLON) || stream.check(TokenType::LBRACE)) {
        // The name is missing, but backing type or body still follow
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected enum name");
        name = ctx.pool.intern("");
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected enum name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 3. Parse backing type
    PrimitiveTypeAST* backingType = nullptr;
    if (stream.match(TokenType::COLON)) {
        TypeAST* type = parseType(stream, ctx);
        if (type && type->isa<PrimitiveTypeAST>()) {
            backingType = type->as<PrimitiveTypeAST>();
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected integer backing type, got '", stream.peekValue(), "'");
            return nullptr;
        }
    }
    
    // 4. Parse enum body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for enum body");
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::EnumBody, stream.currentLoc());

    std::vector<EnumVariantAST*> variants;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Filter all invalid token in this context
        // An enum variant starts with IDENTIFIER or AT_SIGN (attributes), ends with SEMICOLON
        if (!stream.checkAny(TokenType::IDENTIFIER, TokenType::AT_SIGN, TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token(s) '", stream.peekValue(), "' inside enum body");
            
            // Synchronize to nearest valid variant to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::AT_SIGN, TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // Consume stray ';'
        if (stream.check(TokenType::SEMICOLON)) {
            stream.consume();
            continue;
        }

        EnumVariantAST* variant = parseEnumVariant(stream, ctx);
        if (variant) {
            variants.push_back(variant);
        } else {
            // Synchronize to nearest valid variant to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::AT_SIGN, TokenType::RBRACE);
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
    
    return enumDecl;
}

/// NOTE: parseEnumDecl already filters the context for us, we will
///  start with token IDENTIFIER in this function
EnumVariantAST* parseEnumVariant(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse variant name ──────────────────────────────────────────────
    InternedString name;
    
    // Variant name is always provided (filtered by parseEnumDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variant name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 3. Parse '=' ─────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 4. Parse variant value ──────────────────────────────────────────────
    int64_t value = 0;
    if (stream.check(TokenType::INT_LITERAL) ||
        stream.check(TokenType::HEX_LITERAL) ||
        stream.check(TokenType::BINARY_LITERAL)) {
        Token valueTok = stream.consume();
        value = std::stoll(valueTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected integer literal, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 5. Build AST using constructor ──────────────────────────────────
    auto* variant = ctx.arena.make<EnumVariantAST>(name, value);
    variant->loc = loc;
    variant->attributes = attrs;
    if (doc.has_value()) {
        variant->doc = doc;
    }

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after enum variant declaration");
    }
    
    return variant;
}

// =============================================================================
// parseTraitDecl
// =============================================================================

TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'trait' keyword
    if (!stream.check(TokenType::TRAIT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'trait', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    // 2. Parse trait name
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else if (stream.check(TokenType::LESS) || stream.check(TokenType::LBRACE)) {
        // The name is missing, but generics or body still follow
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected trait name");
        name = ctx.pool.intern("");
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected trait name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 3. Parse generic parameters
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // 4. Parse trait body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for trait body");
        return nullptr;
    }
    stream.consume();

    ScopedContext bodyGuard(ctx, SyntacticContext::TraitBody, stream.currentLoc());

    std::vector<TraitFieldDeclAST*> fields;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // Filter all invalid token in this context
        // A trait field starts with IDENTIFIER, CONST, or AT_SIGN (attributes), ends with SEMICOLON
        if (!stream.checkAny(TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token(s) '", stream.peekValue(), "' inside trait body");
            
            // Synchronize to nearest valid field to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // Consume stray ';'
        if (stream.check(TokenType::SEMICOLON)) {
            stream.consume();
            continue;
        }

        TraitFieldDeclAST* field = parseTraitField(stream, ctx);
        if (field) {
            fields.push_back(field);
        } else {
            // Synchronize to nearest valid field to recover
            synchronizeTo(stream, ctx, TokenType::IDENTIFIER, TokenType::CONST, TokenType::AT_SIGN, TokenType::RBRACE);
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

    return traitDecl;
}

/// NOTE: parseTraitDecl already filters the context for us, we will
///  start with tokens IDENTIFIER or CONST in this function
TraitFieldDeclAST* parseTraitField(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 1. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);
    
    // ─── 2. Parse const modifier ────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    
    // ─── 3. Parse trait field name and type ──────────────────────────────
    InternedString name;
    
    // Field name is always provided (filtered by parseTraitDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    }

    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        /// Check the next token
        if (stream.check(TokenType::RBRACE) || stream.match(TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                    "incomplete trait field declaration '", ctx.pool.lookup(name), "'");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected trait field type, got '", stream.peekValue(), "'");
        }
        
        return nullptr;
    }
    
    // ─── 4. Build AST using constructor ──────────────────────────────────
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>(name, type, isConst);
    traitField->loc = loc;
    traitField->attributes = attrs;
    if (doc.has_value()) {
        traitField->doc = doc;
    }

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) { 
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after trait field declaration");
    }
    
    return traitField;
}

} // namespace parser