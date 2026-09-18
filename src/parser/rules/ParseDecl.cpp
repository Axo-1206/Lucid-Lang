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
#include "parser/support/ErrorRecovery.hpp"

#include <vector>

namespace parser {

// ─────────────────────────────────────────────────────────────────────────────
// Shared helper: wrap a block body in an AnonFuncExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Wrap a parsed block body in an AnonFuncExprAST with the given
///        signature.
///
/// Both function declarations (with block bodies) and function-typed field
/// defaults (with block bodies) produce an AnonFuncExprAST whose `funcType`
/// is borrowed from the declaration site rather than written inline. This
/// helper performs that wrapping, so both parse paths share a single
/// construction point.
///
/// The resulting node is the declaration's initializer (for a function) or
/// the field's default value (for a field). It is NOT a standalone value;
/// its `funcType` is the declaration site's signature, which Sema will
/// check for assignability when it resolves the initializer against the
/// declared type.
///
/// @param funcType The signature to attach. Must not be null.
/// @param body     The parsed block body. Must not be null.
/// @param loc      Source location for the wrapped node.
/// @param ctx      The parsing context.
/// @return An AnonFuncExprAST whose funcType is `funcType` and whose body
///         is `body`.
static AnonFuncExprAST* wrapBlockBodyInAnonFunc(
    FuncTypeAST* funcType,
    StmtAST* body,
    SourceLocation loc,
    ParserContext& ctx)
{
    auto* anon = ctx.arena.make<AnonFuncExprAST>(funcType, body);
    anon->loc = loc;
    return anon;
}

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
            synchronizeToBoundary(stream, ctx, {TokenType::ASSIGN, TokenType::SEMICOLON});
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
                synchronizeToBoundary(stream, ctx, {TokenType::ASSIGN, TokenType::SEMICOLON});

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
            synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
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
    // 1. Keyword
    SourceLocation keywordLoc = stream.currentLoc();
    bool isConst = stream.match(TokenType::CONST);
    if (!isConst && !stream.match(TokenType::LET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'let' or 'const', got '", stream.peekValue(), "'");
        return nullptr;
    }
    DeclKeyword keyword = isConst ? DeclKeyword::Const : DeclKeyword::Let;

    // 2. Name
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        name = ctx.pool.intern(stream.consume().value);
    } else if (is_function_type_keyword(stream.peekType()) ||
               stream.check(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                stream.currentLoc(), "expected function name");
        name = ctx.pool.intern("");
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                                stream.currentLoc(),
                                "expected function name, got '", stream.peekValue(), "'");
        return nullptr;
    }

    // 3. Generics
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }
    bool genericLetError = (!isConst && !genericParams.empty());
    if (genericLetError) {
        ctx.diagnostics.errorAt(DiagCode::Sem_GenericRequiresConst, keywordLoc,
                                "a generic function must be declared 'const'");
    }

    // 4. Header: marked bound cluster + return type after first '->'
    //
    // We collect the bound cluster here and stop at the first '->'.
    // Everything after is parsed by parseType (which recurses into
    // parseFuncType for arrow-separated stages).
    SourceLocation headerLoc = stream.currentLoc();
    std::vector<std::vector<ParamAST*>> groups;
    std::vector<FuncShape>              shapes;

    while (is_function_type_keyword(stream.peekType())) {
        Token markerTok = stream.consume();
        FuncShape shape = (markerTok.type == TokenType::TYPE_FN)
                          ? FuncShape::Fn : FuncShape::Cls;
        shapes.push_back(shape);

        if (!stream.check(TokenType::LPAREN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken,
                                    stream.currentLoc(),
                                    "expected '(' after '", markerTok.value, "'");
            groups.push_back({});
            break;
        }
        std::vector<ParamAST*> group = parseParamList(stream, ctx, /*allowNames=*/true);
        groups.push_back(std::move(group));

        if (stream.check(TokenType::ARROW)) break;
        if (!is_function_type_keyword(stream.peekType())) break;
    }

    if (groups.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, headerLoc,
                                "expected 'fn' or 'cls' before parameter group, got '",
                                stream.peekValue(), "'");
        groups.push_back({});
        shapes.push_back(FuncShape::Fn);
    }

    TypeAST* restType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        restType = parseType(stream, ctx);
        if (!restType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType,
                                    stream.currentLoc(),
                                    "expected return type after '->'");
            restType = ctx.arena.make<UnknownTypeAST>();
            restType->hasSyntaxError = true;
        }
    }

    // Build the declared FuncTypeAST chain.
    TypeAST* cur = restType;
    std::vector<FuncTypeAST*> stageTypes(groups.size());
    for (int i = static_cast<int>(groups.size()) - 1; i >= 0; --i) {
        auto* ft = ctx.arena.make<FuncTypeAST>();
        auto pb = ctx.arena.makeBuilder<ParamAST*>();
        for (ParamAST* p : groups[i]) pb.push_back(p);
        ft->params = pb.build();
        ft->shape = shapes[i];
        ft->returnType = cur;
        ft->loc = headerLoc;
        stageTypes[i] = ft;
        cur = ft;
    }
    FuncTypeAST* declaredType = stageTypes[0];

    // 5. '=' (optional — foreign functions don't have one)
    bool hasExplicitAssign = stream.match(TokenType::ASSIGN);
    if (!hasExplicitAssign &&
        !stream.check(TokenType::LBRACE) &&
        !stream.check(TokenType::SEMICOLON)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken,
                                stream.currentLoc(),
                                "expected '=', '{', or ';', got '",
                                stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx,
            {TokenType::LBRACE, TokenType::ASSIGN, TokenType::SEMICOLON});
        hasExplicitAssign = stream.match(TokenType::ASSIGN);
    }

    // 6. Body
    ExprAST* init = nullptr;
    bool bodyError = false;

    if (stream.check(TokenType::LBRACE)) {
        // Block body: build an anon chain with declaredType's nodes.
        if (!hasExplicitAssign) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken,
                                    stream.previousLoc(),
                                    "expected '=' before function body");
        }
        StmtAST* block = parseBlock(stream, ctx);
        if (!block) {
            block = ctx.arena.make<UnknownStmtAST>();
            block->hasSyntaxError = true;
            bodyError = true;
        }

        // Build the anon chain bottom-up. Innermost stage gets the block.
        StmtAST* currentBody = block;
        AnonFuncExprAST* currentAnon = nullptr;
        for (int i = static_cast<int>(stageTypes.size()) - 1; i >= 0; --i) {
            currentAnon = ctx.arena.make<AnonFuncExprAST>(stageTypes[i], currentBody);
            currentAnon->loc = headerLoc;

            if (i == 0) break;
            auto* ret = ctx.arena.make<ReturnStmtAST>();
            ret->loc = headerLoc;
            ret->value = currentAnon;
            currentBody = ret;
        }
        init = currentAnon;
    } else if (hasExplicitAssign) {
        // Expression body: reference or call.
        if (looksLikeAnonFunc(stream, ctx)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                                    stream.currentLoc(),
                                    "anonymous function not allowed at declaration site");
            ctx.diagnostics.noteAt(stream.currentLoc(),
                                   "Use a block body instead: '{ ... }'");
            init = ctx.arena.make<UnknownExprAST>();
            init->hasSyntaxError = true;
            bodyError = true;
            synchronizeTo(stream, ctx, TokenType::SEMICOLON);
        } else {
            ExprAST* expr = parseRequiredExpr(stream, ctx, "function body expression");
            if (!expr) {
                init = ctx.arena.make<UnknownExprAST>();
                init->hasSyntaxError = true;
                bodyError = true;
            } else if (expr->isa<IdentifierExprAST>() ||
                       expr->isa<ModuleAccessExprAST>() ||
                       expr->isa<FieldAccessExprAST>() ||
                       expr->isa<CallExprAST>()) {
                init = expr;
            } else {
                // Plain expression — no signature to wrap it in here.
                // Wrap in a ReturnStmt for the innermost stage.
                auto* ret = ctx.arena.make<ReturnStmtAST>();
                ret->loc = expr->loc;
                ret->value = expr;

                StmtAST* currentBody = ret;
                AnonFuncExprAST* currentAnon = nullptr;
                for (int i = static_cast<int>(stageTypes.size()) - 1; i >= 0; --i) {
                    currentAnon = ctx.arena.make<AnonFuncExprAST>(stageTypes[i], currentBody);
                    currentAnon->loc = headerLoc;

                    if (i == 0) break;
                    auto* innerRet = ctx.arena.make<ReturnStmtAST>();
                    innerRet->loc = headerLoc;
                    innerRet->value = currentAnon;
                    currentBody = innerRet;
                }
                init = currentAnon;
            }
        }
    }
    // else: foreign function, init stays nullptr.

    // 7. Build FuncDeclAST
    auto* funcDecl = ctx.arena.make<FuncDeclAST>(
        name, keyword, genericParams, declaredType, init);

    if (bodyError || genericLetError || name.isEmpty() ||
        (init && init->hasSyntaxError)) {
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
        synchronizeToBoundary(stream, ctx);
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

        FieldDeclAST* field = parseFieldDecl(stream, ctx, name);
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
FieldDeclAST* parseFieldDecl(TokenStream& stream, ParserContext& ctx, InternedString structName) {
    SourceLocation loc = stream.currentLoc();
    auto doc = harvestDocComment(stream, ctx);

    // ─── 1. Attributes ─────────────────────────────────────────────────────
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    // ─── 2. Const modifier ─────────────────────────────────────────────────
    bool isConst = stream.match(TokenType::CONST);
    bool hasSyntaxError = false;

    // ─── 3. Field name ─────────────────────────────────────────────────────
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected field name, got '", stream.peekValue(), "'");
        name = ctx.pool.intern("");
        hasSyntaxError = true;
    }

    // ─── 4. Field type ─────────────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        type = ctx.arena.make<UnknownTypeAST>();
        type->hasSyntaxError = true;
        hasSyntaxError = true;

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

        auto* fieldDecl = ctx.arena.make<FieldDeclAST>(name, type, nullptr, isConst);
        fieldDecl->loc = loc;
        fieldDecl->attributes = attrs;
        if (doc.has_value()) fieldDecl->doc = doc;
        fieldDecl->hasSyntaxError = true;

        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        return fieldDecl;
    }

    // ─── 5. Default value ──────────────────────────────────────────────────
    //
    // The field's effective type may change if this is a function-typed
    // field with a block default. The block default becomes an
    // AnonFuncExprAST whose funcType is the field's type with a
    // synthesized `self: &StructName` prepended.
    ExprAST* defaultVal = nullptr;
    TypeAST* finalType = type;

    if (stream.match(TokenType::ASSIGN)) {
        if (stream.check(TokenType::LBRACE)) {
            // ─── 5a. Block default ─────────────────────────────────────────
            //
            // If this is a function-typed field, prepend `self: &StructName`
            // to the field's type so the AnonFuncExprAST carries the
            // self-inclusive signature. For non-function fields with a
            // block default, this is a syntax error — but we let Sema
            // reject it later, since the parser's job is to build AST.
            FuncTypeAST* effectiveFuncType = nullptr;

            if (type->isa<FuncTypeAST>() && structName.isValid()) {
                FuncTypeAST* funcType = type->as<FuncTypeAST>();

                // Synthesize self: &StructName
                NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(structName);
                RefTypeAST* selfType = ctx.arena.make<RefTypeAST>(namedType);

                InternedString selfName = ctx.pool.intern("self");
                ParamAST* selfParam = ctx.arena.make<ParamAST>(
                    selfName, selfType, false, false);
                selfParam->loc = loc;

                // Build a new funcType with self prepended.
                FuncTypeAST* newFuncType = ctx.arena.make<FuncTypeAST>();
                auto paramsBuilder = ctx.arena.makeBuilder<ParamAST*>();
                paramsBuilder.push_back(selfParam);
                for (ParamAST* origParam : funcType->params) {
                    paramsBuilder.push_back(origParam);
                }
                newFuncType->params = paramsBuilder.build();
                newFuncType->returnType = funcType->returnType;
                newFuncType->loc = funcType->loc;

                effectiveFuncType = newFuncType;
                finalType = newFuncType;
            } else {
                // Non-function field with a block default: parser can't
                // reject cleanly here, but we still need a funcType for
                // the AnonFuncExprAST. Sema will reject this when it
                // resolves the default against the field type.
                effectiveFuncType = ctx.arena.make<FuncTypeAST>();
                effectiveFuncType->params = ctx.arena.makeBuilder<ParamAST*>().build();
                effectiveFuncType->returnType = nullptr;
                effectiveFuncType->loc = loc;
            }

            // Parse the block body.
            StmtAST* blockBody = parseBlock(stream, ctx);
            if (!blockBody) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                        "expected block body");
                blockBody = ctx.arena.make<UnknownStmtAST>();
                blockBody->hasSyntaxError = true;
                hasSyntaxError = true;
            }

            // Wrap the block body in an AnonFuncExprAST — same helper as
            // the function declaration path.
            defaultVal = wrapBlockBodyInAnonFunc(effectiveFuncType, blockBody, loc, ctx);
        } else {
            // ─── 5b. Expression default ────────────────────────────────────
            if (looksLikeAnonFunc(stream, ctx)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                                        stream.currentLoc(),
                                        "anonymous function not allowed at declaration site");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "Use a block body instead: '{ ... }'");
                ctx.diagnostics.noteAt(stream.currentLoc(),
                                       "The block body borrows its signature from the field type");
                hasSyntaxError = true;
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

    // ─── 6. Build AST ──────────────────────────────────────────────────────
    auto* fieldDecl = ctx.arena.make<FieldDeclAST>(name, finalType, defaultVal, isConst);
    fieldDecl->loc = loc;
    fieldDecl->attributes = attrs;
    if (doc.has_value()) fieldDecl->doc = doc;

    if (hasSyntaxError || name.isEmpty() ||
        (finalType && finalType->hasSyntaxError) ||
        (defaultVal && defaultVal->hasSyntaxError)) {
        fieldDecl->hasSyntaxError = true;
    }

    // ─── 7. Semicolon ──────────────────────────────────────────────────────
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
            
            synchronizeToBoundary(stream, ctx, {TokenType::LBRACE, TokenType::SEMICOLON});
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
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
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
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
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