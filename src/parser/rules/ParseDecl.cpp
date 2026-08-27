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
    bool hasDeclError = false;

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
        hasDeclError = true;

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
            type = ctx.arena.make<UnknownTypeAST>();
            type->hasSyntaxError = true;
            hasDeclError = true;

            if (stream.check(TokenType::ASSIGN)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected type for variable declaration '", ctx.pool.lookup(name), "'");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected type for variable declaration '", ctx.pool.lookup(name), "', but got '", stream.peekValue(), "'");
                synchronizeToDeclBoundary(stream, ctx, {TokenType::ASSIGN, TokenType::SEMICOLON});

                if (!stream.check(TokenType::ASSIGN)) {
                    if (!stream.check(TokenType::SEMICOLON)) {
                        ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                                "incomplete variable declaration '", ctx.pool.lookup(name), "'");
                    }
                    auto* varDecl = ctx.arena.make<VarDeclAST>(name, keyword, type, nullptr);
                    varDecl->hasSyntaxError = true;
                    return varDecl;
                }
            }
        }
    }
    
    // ─── Parse initializer ─────────────────────────────────────────────
    ExprAST* init = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        init = parseRequiredExpr(stream, ctx, "initializer expression");
        if (init && init->hasSyntaxError) {
            hasDeclError = true;
            synchronizeToDeclBoundary(stream, ctx, {TokenType::SEMICOLON});
        }
    } 
    if (isConst && !init) {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingInitializer, stream.currentLoc(),
                                "const variable '", ctx.pool.lookup(name), "' requires an initializer");
        init = ctx.arena.make<UnknownExprAST>();
        init->hasSyntaxError = true;
        hasDeclError = true;
    }
    
    // Create VarDeclAST using constructor (all parser fields immutable)
    auto* varDecl = ctx.arena.make<VarDeclAST>(name, keyword, type, init);
    if (hasDeclError || (type && type->hasSyntaxError) || (init && init->hasSyntaxError) || name.isEmpty()) {
        varDecl->hasSyntaxError = true;
    }
    
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
    
    // ─── 4. Parse parameter groups ─────────────────────────────────────────
    SourceLocation funcTypeLoc = stream.currentLoc();
    std::vector<std::vector<ParamAST*>> groups;
    
    // Function MUST have at least one parameter group
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(' for function parameters, got '", stream.peekValue(), "'");
        // Create an empty group as a placeholder for recovery
        std::vector<ParamAST*> emptyGroup;
        groups.push_back(emptyGroup);
    }
    
    while (stream.check(TokenType::LPAREN)) {
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx, true);
        groups.push_back(groupParams);
        // Stop if we see '->' - this group has an explicit arrow
        if (stream.check(TokenType::ARROW)) {
            break;
        }
    }
    
    // ─── 5. Parse return type ──────────────────────────────────────────────
    TypeAST* restType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        restType = parseType(stream, ctx);
        if (!restType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected return type after '->'");
            // Error recovery: create UnknownTypeAST and synchronize
            restType = ctx.arena.make<UnknownTypeAST>();
            restType->hasSyntaxError = true;
            synchronizeToDeclBoundary(stream, ctx,
                {TokenType::LBRACE, TokenType::ASSIGN, TokenType::SEMICOLON});
        }
    }
    
    // ─── 6. Parse body ──────────────────────────────────────────────────────
    StmtAST* body = nullptr;
    bool hasExplicitAssign = false;
    bool hasBodyError = false;
    
    // ─── 6a. Check for '=' ──────────────────────────────────────────────────
    if (stream.match(TokenType::ASSIGN)) {
        hasExplicitAssign = true;
    } else if (!stream.check(TokenType::LBRACE) && !stream.check(TokenType::SEMICOLON)) {
        // If we're not at '=', '{', or ';', something is wrong - synchronize
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', '{', or ';', got '", stream.peekValue(), "'");
        synchronizeToDeclBoundary(stream, ctx,
            {TokenType::LBRACE, TokenType::ASSIGN, TokenType::SEMICOLON});
        
        // After synchronization, check again
        if (stream.match(TokenType::ASSIGN)) {
            hasExplicitAssign = true;
        } else if (stream.check(TokenType::LBRACE)) {
            // Continue to body parsing
        } else if (stream.check(TokenType::SEMICOLON)) {
            // Foreign function - body remains nullptr
            body = nullptr;
        } else {
            // Failed to recover - create error body
            body = ctx.arena.make<UnknownStmtAST>();
            body->hasSyntaxError = true;
            hasBodyError = true;
        }
    }
    
    // ─── 6b. Parse body if we have an explicit '=' or just a block ──────
    if (!body && stream.check(TokenType::LBRACE)) {
        if (!hasExplicitAssign) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.previousLoc(),
                                    "expected '=' before function body");
            // Continue parsing anyway for better recovery
        }
        
        body = parseBlock(stream, ctx);
        if (!body) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                    "expected block body");
            body = ctx.arena.make<UnknownStmtAST>();
            body->hasSyntaxError = true;
            hasBodyError = true;
        }
    } else if (!body && hasExplicitAssign) {
        // ─── Expression body ──────────────────────────────────────────────
        if (looksLikeAnonFunc(stream, ctx)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration, 
                                    stream.currentLoc(),
                                    "anonymous function not allowed at declaration site");
            ctx.diagnostics.noteAt(stream.currentLoc(),
                                   "Use a block body instead: '{ ... }'");
            
            // Error recovery
            auto* unknownExpr = ctx.arena.make<UnknownExprAST>();
            unknownExpr->hasSyntaxError = true;
            auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
            returnStmt->value = unknownExpr;
            returnStmt->hasSyntaxError = true;
            body = returnStmt;
            hasBodyError = true;
            
            // Synchronize to semicolon
            synchronizeTo(stream, ctx, TokenType::SEMICOLON);
        } else {
            ExprAST* exprBody = parseRequiredExpr(stream, ctx, "function body expression");
            if (exprBody && exprBody->hasSyntaxError) {
                hasBodyError = true;
            }
            
            // ─── Determine if this is a pure function reference ──────────
            bool isPureFunctionRef = exprBody && (exprBody->isa<IdentifierExprAST>() ||
                exprBody->isa<ModuleAccessExprAST>() ||
                exprBody->isa<FieldAccessExprAST>() ||
                exprBody->isa<ComposeExprAST>() ||
                exprBody->isa<CallExprAST>());
            
            if (isPureFunctionRef) {
                auto* refStmt = ctx.arena.make<FuncRefStmtAST>();
                refStmt->loc = exprBody->loc;
                refStmt->target = exprBody;
                body = refStmt;
            } else {
                auto* returnStmt = ctx.arena.make<ReturnStmtAST>();
                returnStmt->loc = exprBody ? exprBody->loc : stream.currentLoc();
                returnStmt->value = exprBody;
                body = returnStmt;
            }
        }
    } else if (!body && !stream.check(TokenType::SEMICOLON)) {
        // ─── No body found - foreign function or error ──────────────────
        // If we're at ';', it's a foreign function
        // Otherwise, something is wrong
        if (!stream.check(TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '{', '=', or ';' for function body, got '", 
                                    stream.peekValue(), "'");
            body = ctx.arena.make<UnknownStmtAST>();
            body->hasSyntaxError = true;
            hasBodyError = true;
        }
        // body remains nullptr for foreign functions
    }
    
    // ─── 7. Build nested function types and wrapper bodies ──────────────────
    FuncTypeAST* funcType = nullptr;
    StmtAST* finalBody = body;
    bool hasError = false;
    
    if (groups.empty() || (groups.size() == 1 && groups[0].empty())) {
        // ─── No valid parameter groups ──────────────────────────────────────
        // This should only happen if parsing failed earlier.
        // Create a default FuncTypeAST with empty params.
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, funcTypeLoc,
                                "function must have at least one parameter group '()'");
        
        funcType = ctx.arena.make<FuncTypeAST>();
        funcType->params = ctx.arena.makeBuilder<ParamAST*>().build();
        funcType->returnType = restType;
        funcType->loc = funcTypeLoc;
        hasError = true;
    } else {
        // ─── 7a. Build nested function types from the bottom up ─────────────
        // Store each group's FuncTypeAST in a vector where index 0 = outermost
        std::vector<FuncTypeAST*> groupTypes;
        groupTypes.resize(groups.size());
        
        TypeAST* currentReturnType = restType;  // nullptr for void
        for (int i = static_cast<int>(groups.size()) - 1; i >= 0; --i) {
            auto* ft = ctx.arena.make<FuncTypeAST>();
            auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
            for (ParamAST* param : groups[i]) {
                paramBuilder.push_back(param);
            }
            ft->params = paramBuilder.build();
            ft->returnType = currentReturnType;
            ft->loc = funcTypeLoc;
            groupTypes[i] = ft;
            currentReturnType = ft;
        }
        funcType = groupTypes[0];
        
        // ─── 7b. Build wrapper bodies from the bottom up ────────────────────
        // The innermost group uses the user's original body
        StmtAST* innerBody = body;
        
        // Only wrap if there are at least 2 groups AND we have a body
        if (groups.size() >= 2 && body) {
            // Wrap from the second-last group down to the first group.
            for (int i = static_cast<int>(groups.size()) - 2; i >= 0; --i) {
                // The inner function type is the one for groups[i+1]
                FuncTypeAST* innerType = groupTypes[i + 1];
                
                // Create anonymous function
                auto* anon = ctx.arena.make<AnonFuncExprAST>(innerType, innerBody);
                // Set location to the first parameter of the inner group, or the group's location
                if (!groups[i + 1].empty() && groups[i + 1][0]) {
                    anon->loc = groups[i + 1][0]->loc;
                } else {
                    anon->loc = funcTypeLoc;
                }
                
                // Wrap in return statement
                auto* ret = ctx.arena.make<ReturnStmtAST>();
                ret->loc = anon->loc;
                ret->value = anon;
                
                // This becomes the body for the next outer group
                innerBody = ret;
            }
        }
        
        finalBody = innerBody;
    }
    
    // ─── 8. Build FuncDeclAST ────────────────────────────────────────────────
    auto* funcDecl = ctx.arena.make<FuncDeclAST>(name, keyword, genericParams, funcType, finalBody);
    if (hasBodyError || hasError || (restType && restType->hasSyntaxError) || name.isEmpty()) {
        funcDecl->hasSyntaxError = true;
    }
    
    return funcDecl;
}

// =============================================================================
// parseStructDecl
// =============================================================================

StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'struct' keyword
    if (!stream.match(TokenType::STRUCT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'struct', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // 2. Parse struct name
    bool hasSyntaxError = false;
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else if (stream.check(TokenType::LESS) || stream.check(TokenType::LBRACE) || stream.check(TokenType::COLON)) {
        // The name is missing, but generics, trait list, or body still follow
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected struct name");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected struct name, got '", stream.peekValue(), "'");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    }
    
    // 3. Parse generic parameters
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    
    // 4. Parse trait implementations
    std::vector<NamedTypeAST*> traitRefs;
    if (stream.match(TokenType::COLON)) {
        while (!stream.isAtEnd() && !stream.check(TokenType::LBRACE)) {
            TypeAST* parsed = parseNamedType(stream, ctx);
            NamedTypeAST* traitRef = parsed ? parsed->as<NamedTypeAST>() : nullptr;
            if (traitRef) {
                traitRefs.push_back(traitRef);

                if (!stream.match(TokenType::COMMA)) {
                    if (stream.check(TokenType::LBRACE)) {
                        break;
                    }
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected ',' to separate traits");
                    hasSyntaxError = true;

                    synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::LBRACE);
                    if (stream.match(TokenType::COMMA)) {
                        continue;
                    }
                    break;
                }
            } else {
                auto* placeholder = ctx.arena.make<NamedTypeAST>(ctx.pool.intern(""));
                placeholder->hasSyntaxError = true;
                traitRefs.push_back(placeholder);
                hasSyntaxError = true;

                if (stream.match(TokenType::COMMA)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.previousLoc(),
                                            "expected a type after ',' in trait list");
                    continue;
                }

                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                        "failed to parse trait, got '", stream.peekValue(), "'");

                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::LBRACE);
                if (stream.match(TokenType::COMMA)) {
                    continue;
                }
                break;
            }
        }
    }
    
    // 5. Parse struct body
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for struct body");
        synchronizeToDeclBoundary(stream, ctx);
        auto traitBuilder = ctx.arena.makeBuilder<NamedTypeAST*>();
        for (auto* tr : traitRefs) {
            traitBuilder.push_back(tr);
        }
        auto* structDecl = ctx.arena.make<StructDeclAST>(
            name,
            genericParams,
            ctx.arena.makeBuilder<FieldDeclAST*>().build(),
            traitBuilder.build()
        );
        structDecl->hasSyntaxError = true;
        return structDecl;
    }
    stream.consume();

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
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        FieldDeclAST* field = parseFieldDecl(stream, ctx);
        fields.push_back(field);
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
    structDecl->hasSyntaxError = hasSyntaxError;
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
    bool hasSyntaxError = false;
    
    // ─── 3. Parse field name ──────────────────────────────────────────────
    InternedString name;
    
    // Field name is always provided (filtered by parseStructDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected field name, got '", stream.peekValue(), "'");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    }

    // ─── 4. Parse field type ──────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        // Create an UnknownTypeAST as a placeholder
        type = ctx.arena.make<UnknownTypeAST>();
        type->hasSyntaxError = true;
        hasSyntaxError = true;
        
        // Check what went wrong for better diagnostics
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
        
        // ─── Build broken node with UnknownTypeAST ──────────────────────
        auto* fieldDecl = ctx.arena.make<FieldDeclAST>(name, type, nullptr, nullptr, isConst);
        fieldDecl->loc = loc;
        fieldDecl->attributes = attrs;
        if (doc.has_value()) {
            fieldDecl->doc = doc;
        }
        fieldDecl->hasSyntaxError = true;
        
        // Synchronize to a valid recovery point (field separator or closing brace)
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        return fieldDecl;
    }
    
    // ─── 5. Parse default value ─────────────────────────────────────────────
    ExprAST* defaultVal = nullptr;
    StmtAST* defaultBody = nullptr;
    
    if (stream.match(TokenType::ASSIGN)) {
        // ─── 5a. Check for block body ──────────────────────────────────────
        if (stream.check(TokenType::LBRACE)) {
            defaultBody = parseBlock(stream, ctx);
            
            /// Check if parseBlock actually got a valid block
            if (!defaultBody) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected block body");
                defaultBody = ctx.arena.make<UnknownStmtAST>();
                defaultBody->hasSyntaxError = true;
                hasSyntaxError = true;
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
                hasSyntaxError = true;
                // Create a placeholder and continue
                defaultVal = ctx.arena.make<UnknownExprAST>();
                defaultVal->hasSyntaxError = true;
            } else {
                defaultVal = parseExpr(stream, ctx);
                if (!defaultVal) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                            "expected default value expression");
                    defaultVal = ctx.arena.make<UnknownExprAST>();
                    defaultVal->hasSyntaxError = true;
                    hasSyntaxError = true;
                }
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
    
    // ─── 7. Mark as broken if any component is broken ──────────────────────
    if (hasSyntaxError || name.isEmpty() || (type && type->hasSyntaxError) ||
        (defaultVal && defaultVal->hasSyntaxError) || 
        (defaultBody && defaultBody->hasSyntaxError)) {
        fieldDecl->hasSyntaxError = true;
    }

    // ─── 8. Check for semicolon ─────────────────────────────────────────────
    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after field declaration");
        fieldDecl->hasSyntaxError = true;
    }
    
    return fieldDecl;
}

// =============================================================================
// parseEnumDecl
// =============================================================================

EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'enum' keyword
    if (!stream.match(TokenType::ENUM)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'enum', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
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
            
            synchronizeToDeclBoundary(stream, ctx, {TokenType::LBRACE, TokenType::SEMICOLON});
            if (!stream.check(TokenType::LBRACE)) {
                auto builder = ctx.arena.makeBuilder<EnumVariantAST*>();
                auto* enumDecl = ctx.arena.make<EnumDeclAST>(name, builder.build(), backingType);
                enumDecl->hasSyntaxError = true;
                return enumDecl;
            }
        }
    }
    
    // 4. Parse enum body
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for enum body");
        synchronizeToDeclBoundary(stream, ctx, {TokenType::SEMICOLON});
        auto* enumDecl = ctx.arena.make<EnumDeclAST>(name, ctx.arena.makeBuilder<EnumVariantAST*>().build(), backingType);
        enumDecl->hasSyntaxError = true;
        return enumDecl;
    }

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
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        EnumVariantAST* variant = parseEnumVariant(stream, ctx);
        variants.push_back(variant);
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
    bool hasSyntaxError = false;
    
    // Variant name is always provided (filtered by parseEnumDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected variant name, got '", stream.peekValue(), "'");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    }
    
    // ─── 3. Parse '=' ─────────────────────────────────────────────────────
    int64_t value = 0;
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '=', got '", stream.peekValue(), "'");
        hasSyntaxError = true;
        // ─── Create broken enum variant and return ──────────────────────
        auto* variant = ctx.arena.make<EnumVariantAST>(name, 0);
        variant->loc = loc;
        variant->attributes = attrs;
        if (doc.has_value()) {
            variant->doc = doc;
        }
        variant->hasSyntaxError = true;
        // Synchronize to a valid recovery point (variant separator or closing brace)
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::COMMA, TokenType::RBRACE);
        return variant;
    }
    
    // ─── 4. Parse variant value ──────────────────────────────────────────────
    if (stream.check(TokenType::INT_LITERAL) ||
        stream.check(TokenType::HEX_LITERAL) ||
        stream.check(TokenType::BINARY_LITERAL)) {
        Token valueTok = stream.consume();
        try {
            value = std::stoll(valueTok.value);
        } catch (const std::exception&) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "invalid integer literal '", valueTok.value, "'");
            value = 0;
            hasSyntaxError = true;
        }
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected integer literal, got '", stream.peekValue(), "'");
        hasSyntaxError = true;
    }
    
    // ─── 5. Build AST using constructor ──────────────────────────────────
    auto* variant = ctx.arena.make<EnumVariantAST>(name, value);
    variant->loc = loc;
    variant->attributes = attrs;
    if (doc.has_value()) {
        variant->doc = doc;
    }
    if (hasSyntaxError || name.isEmpty()) {
        variant->hasSyntaxError = true;
    }

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after enum variant declaration");
        variant->hasSyntaxError = true;
    }
    
    return variant;
}

// =============================================================================
// parseTraitDecl
// =============================================================================

TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx) {
    // 1. Parse 'trait' keyword
    if (!stream.match(TokenType::TRAIT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'trait', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
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
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected '{' for trait body");
        synchronizeToDeclBoundary(stream, ctx, {TokenType::SEMICOLON});
        auto* traitDecl = ctx.arena.make<TraitDeclAST>(name, genericParams, ctx.arena.makeBuilder<TraitFieldDeclAST*>().build());
        traitDecl->hasSyntaxError = true;
        return traitDecl;
    }

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
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        TraitFieldDeclAST* field = parseTraitField(stream, ctx);
        fields.push_back(field);
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
    bool hasSyntaxError = false;
    
    // ─── 3. Parse trait field name ──────────────────────────────────────────
    InternedString name;
    
    // Field name is always provided (filtered by parseTraitDecl)
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected trait field name, got '", stream.peekValue(), "'");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    }

    // ─── 4. Parse trait field type ──────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        // Create an UnknownTypeAST as a placeholder
        type = ctx.arena.make<UnknownTypeAST>();
        type->hasSyntaxError = true;
        hasSyntaxError = true;
        
        // Check what went wrong for better diagnostics
        if (stream.check(TokenType::RBRACE) || stream.match(TokenType::SEMICOLON)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                    "incomplete trait field declaration '", ctx.pool.lookup(name), "'");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected trait field type, got '", stream.peekValue(), "'");
        }
        
        // ─── Build broken node and return ──────────────────────────────────
        auto* traitField = ctx.arena.make<TraitFieldDeclAST>(name, type, isConst);
        traitField->loc = loc;
        traitField->attributes = attrs;
        if (doc.has_value()) {
            traitField->doc = doc;
        }
        traitField->hasSyntaxError = true;
        
        // Synchronize to a valid recovery point (field separator or closing brace)
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        return traitField;
    }
    
    // ─── 5. Build AST using constructor ──────────────────────────────────
    auto* traitField = ctx.arena.make<TraitFieldDeclAST>(name, type, isConst);
    traitField->loc = loc;
    traitField->attributes = attrs;
    if (doc.has_value()) {
        traitField->doc = doc;
    }
    if (hasSyntaxError || name.isEmpty() || (type && type->hasSyntaxError)) {
        traitField->hasSyntaxError = true;
    }

    if (stream.consumeTrailing(TokenType::SEMICOLON) == 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ';' after trait field declaration");
        traitField->hasSyntaxError = true;
    }
    
    return traitField;
}

} // namespace parser