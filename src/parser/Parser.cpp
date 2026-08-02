/**
 * @file Parser.cpp
 * @brief Implementation of core parsing functions.
 * 
 * This file implements the core parsing infrastructure:
 * - parse(): Single entry point - parses a file and all its imports
 * - parseProgram(): Whole-program convenience wrapper
 * - parseInternal(): Parses internal declarations of a file
 * - parseDecl(): Dispatch to specific declaration parsers
 * 
 * Error recovery is in ErrorRecovery.cpp (synchronizeUntil, synchronizeTo,
 * synchronizeToContext).
 */

#include "Parser.hpp"
#include "lexer/Lexer.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace parser {

// =============================================================================
// parse() - ENTRY POINT
// =============================================================================

ModuleAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx) {
    LOG_PARSER_MINIMAL("Parsing file: ", path);
    
    InternedString filePath = ctx.pool.intern(path);

    // Check cache
    if (ctx.resolver) {
        if (auto* cached = ctx.resolver->getParsedModule(filePath)) {
            return cached;
        }
    }

    // Save and reset context stack for this file
    ScopedFileContext fileContext(ctx);
    
    // Check circular dependency
    if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
        LOG_PARSER("Circular import detected: ", path);
        auto* dummy = ctx.arena.make<ModuleAST>();
        dummy->filePath = filePath;
        dummy->hasErrors = true;
        return dummy;
    }
    
    ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
    
    // Lex the source
    std::vector<Token> tokens = lexer::tokenize(source, ctx.diagnostics);

    if (tokens.empty()) {
        auto* mod = ctx.arena.make<ModuleAST>();
        mod->filePath = filePath;
        mod->hasErrors = false;
        if (ctx.resolver) ctx.resolver->cacheModule(filePath, mod);
        return mod;
    }

    // Check for lexer errors
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            ctx.diagnostics.errorAt(DiagCode::Lex_UnknownCharacter,
                                    SourceLocation(tok.line, tok.column),
                                    "Unknown character in source");
            auto* mod = ctx.arena.make<ModuleAST>();
            mod->filePath = filePath;
            mod->hasErrors = true;
            if (ctx.resolver) ctx.resolver->cacheModule(filePath, mod);
            return mod;
        }
    }
    
    // Parse declarations
    TokenStream stream(std::move(tokens));
    std::vector<DeclPtr> allDecls;
    parseInternal(stream, ctx, allDecls);
    
    // Build module AST
    auto* thisModule = ctx.arena.make<ModuleAST>();
    thisModule->filePath = filePath;
    
    auto builder = ctx.arena.makeBuilder<DeclPtr>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    thisModule->decls = builder.build();
    thisModule->hasErrors = ctx.diagnostics.hasErrors();
    
    // Cache the result
    if (ctx.resolver) {
        ctx.resolver->cacheModule(filePath, thisModule);
        LOG_PARSER_DETAIL("Cached module: ", path);
    }
    
    LOG_PARSER_MINIMAL("Parse completed: ", allDecls.size(), " declarations");
    return thisModule;
}

// =============================================================================
// parseProgram() - Whole-program convenience wrapper
// =============================================================================

std::vector<ModuleAST*> parseProgram(const std::string& rootPath,
                                      const std::string& rootSource,
                                      ParserContext& ctx) {
    ModuleAST* root = parse(rootPath, rootSource, ctx);

    if (!ctx.resolver) {
        LOG_PARSER_MINIMAL("parseProgram: no resolver, returning root module only");
        return { root };
    }

    const auto& order = ctx.resolver->getModuleOrder();
    std::vector<ModuleAST*> modules;
    modules.reserve(order.size());
    for (InternedString path : order) {
        if (ModuleAST* mod = ctx.resolver->getParsedModule(path)) {
            modules.push_back(mod);
        }
    }

    LOG_PARSER_MINIMAL("parseProgram: ", modules.size(), " module(s)");
    return modules;
}

// =============================================================================
// parseInternal() - Parse a file's internal declarations
// =============================================================================

void parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls) {
    LOG_PARSER_MINIMAL("Parsing internal declarations");
    
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = stream.getPos();
    
    while (!stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(stream, ctx);
        size_t savedPos = stream.getPos();
        
        // Skip stray semicolons
        if (stream.check(TokenType::SEMICOLON)) {
            LOG_PARSER_DETAIL("Skipping stray semicolon");
            stream.consume();
            continue;
        }
        
        auto* decl = parseDecl(stream, ctx);
        
        if (!decl && stream.isAtEnd()) {
            if (ctx.diagnostics.hasErrors()) {
                break;
            }
            LOG_PARSER_DETAIL("Reached EOF after declarations");
            break;
        }
        
        if (stream.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("NO PROGRESS - stuck on: ", stream.peekValue());
            
            if (!stream.isAtEnd()) {
                stream.consume();
            }
            
            if (consecutiveFailures > 5) {
                LOG_PARSER("Aggressive recovery");
                synchronizeToContext(stream, ctx);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = stream.getPos();
            
            LOG_PARSER_DETAIL("Parsed declaration #", declCount);
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            outDecls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            LOG_PARSER("parseDecl returned nullptr but made progress");
        }
        
        if (stream.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("CRITICAL - no progress after ", consecutiveFailures, " attempts");
            if (!stream.isAtEnd()) {
                stream.consume();
            }
            lastPos = stream.getPos();
        }
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken,
                                stream.currentLoc(),
                                "Too many consecutive parse failures (", 
                                MAX_CONSECUTIVE_FAILURES, "), aborting");
        LOG_PARSER("ERROR: Too many consecutive failures, aborting");
        return;
    }
    
    LOG_PARSER_MINIMAL("Parsed ", declCount, " declarations");
}

// =============================================================================
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    stream.consumeTrailing(TokenType::SEMICOLON);
    
    if (stream.isAtEnd()) {
        return nullptr;
    }

    auto doc = harvestDocComment(stream, ctx);
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    DeclAST* decl = nullptr;
    
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
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);
    } else if (stream.check(TokenType::ENUM)) {
        decl = parseEnumDecl(stream, ctx);
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);
    } else if (stream.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(stream, ctx);
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);
    } else if (stream.check(TokenType::LET) || stream.check(TokenType::CONST)) {
        if (looksLikeFuncDecl(stream, ctx)) {
            decl = parseFuncDecl(stream, ctx);
            if (decl) stream.consumeTrailing(TokenType::SEMICOLON);
        } else {
            decl = parseVarDecl(stream, ctx);
        }
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken,
                                stream.currentLoc(),
                                "unexpected token '", stream.peekValue(), 
                                "' - expected declaration");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    if (decl) {
        decl->attributes = attrs;
        if (doc.has_value()) {
            decl->doc = doc;
        }
    }
    
    return decl;
}

} // namespace parser