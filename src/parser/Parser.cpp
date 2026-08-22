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
#include "debug/DebugUtils.hpp"
#include "core/trace/Trace.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace parser {

// =============================================================================
// parseProgram() - ENTRY POINT (ALL FILE)
// =============================================================================

std::vector<ModuleAST*> parseProgram(const std::string& rootPath,
                                      const std::string& rootSource,
                                      ParserContext& ctx) {
    ModuleAST* root = parse(rootPath, rootSource, ctx);

    if (!ctx.resolver) {
        Trace::detail("parseProgram: no resolver, returning root module only");
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

    Trace::info("Total modules in program: ", modules.size());
    return modules;
}

// =============================================================================
// parse() - SINGLE FILE
// =============================================================================

ModuleAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx) {
    Trace::info("Parsing file: ", path);
    
    InternedString filePath = ctx.pool.intern(path);

    // ─── Check cache ──────────────────────────────────────────────────
    if (ctx.resolver) {
        if (auto* cached = ctx.resolver->getParsedModule(filePath)) {
            Trace::detail("Using cached module: ", path);
            return cached;
        }
    }

    // ─── File context ──────────────────────────────────────────────────
    ScopedFileContext fileContext(ctx);
    
    // ─── Circular import detection ────────────────────────────────────
    if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
        // Infrastructure error - compiler can't resolve the cycle
        Trace::error("Circular import detected: ", path);
        
        // User-facing diagnostic with proper error code
        ctx.diagnostics.errorAt(DiagCode::Sem_ModuleCycle,
                                SourceLocation(1, 1),  // Start of file
                                "Circular import detected: ", path);
        
        auto* dummy = ctx.arena.make<ModuleAST>();
        dummy->filePath = filePath;
        dummy->hasErrors = true;
        return dummy;
    }
    
    ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
    
    // ─── Lex the source ────────────────────────────────────────────────
    Trace::detail("Lexing: ", path);
    std::vector<Token> tokens = lexer::tokenize(source, ctx.diagnostics);

    if (tokens.empty()) {
        Trace::detail("Empty file: ", path);
        auto* mod = ctx.arena.make<ModuleAST>();
        mod->filePath = filePath;
        mod->hasErrors = false;
        if (ctx.resolver) ctx.resolver->cacheModule(filePath, mod);
        return mod;
    }

    // ─── Check for lexer errors ────────────────────────────────────────
    // User code errors - use Diagnostic only
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
    
    // ─── Create ModuleAST BEFORE parsing imports ──────────────────────
    // This allows parseImportDecl() to populate module->imports.
    auto* module = ctx.arena.make<ModuleAST>();
    module->filePath = filePath;
    module->imports.clear();  // Will be populated during parsing
    
    // ─── Set current module in context ──────────────────────────────
    ctx.currentModule = module;
    
    // ─── Parse declarations ────────────────────────────────────────────
    Trace::detail("Parsing declarations in: ", path);
    TokenStream stream(std::move(tokens));
    std::vector<DeclAST*> allDecls;
    parseInternal(stream, ctx, allDecls);
    
    // ─── Reset context ──────────────────────────────────────────────────
    ctx.currentModule = nullptr;
    
    // ─── Build module AST ──────────────────────────────────────────────
    auto builder = ctx.arena.makeBuilder<DeclAST*>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    module->decls = builder.build();
    module->hasErrors = ctx.diagnostics.hasErrors();
    
    // ─── Cache the result ──────────────────────────────────────────────
    if (ctx.resolver) {
        ctx.resolver->cacheModule(filePath, module);
        Trace::detail("Cached module: ", path, " with ", 
                     module->imports.size(), " imports");
    }
    
    Trace::info("Parsed ", allDecls.size(), " declarations in ", path);
    return module;
}

// =============================================================================
// parseInternal() - Parse a file's internal declarations
// =============================================================================

void parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclAST*>& outDecls) {
    Trace::detail("Parsing internal declarations");
    
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = stream.getPos();
    
    while (!stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(stream, ctx);
        size_t savedPos = stream.getPos();
        
        // Skip stray semicolons
        if (stream.check(TokenType::SEMICOLON)) {
            stream.consume();
            continue;
        }
        
        auto* decl = parseDecl(stream, ctx);
        
        if (!decl && stream.isAtEnd()) {
            if (ctx.diagnostics.hasErrors()) {
                break;
            }
            Trace::detail("Reached EOF after declarations");
            break;
        }
        
        if (stream.getPos() == savedPos) {
            consecutiveFailures++;
            Trace::detail("No progress, stuck on: ", stream.peekValue());
            
            if (!stream.isAtEnd()) {
                stream.consume();
            }
            
            if (consecutiveFailures > 5) {
                Trace::detail("Aggressive recovery");
                synchronizeToContext(stream, ctx);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = stream.getPos();
            
            Trace::detail("Parsed declaration: ", declCount);
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            outDecls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            Trace::detail("parseDecl returned nullptr but made progress");
        }
        
        // Critical protection - prevent infinite loops
        if (stream.getPos() == lastPos && consecutiveFailures > 10) {
            Trace::detail("CRITICAL - no progress after ", consecutiveFailures, " attempts");
            if (!stream.isAtEnd()) {
                stream.consume();
            }
            lastPos = stream.getPos();
        }
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        // User code error - too many syntax errors to recover
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken,
                                stream.currentLoc(),
                                "Too many consecutive parse failures (", 
                                MAX_CONSECUTIVE_FAILURES, "), aborting");
        return;
    }
    
    Trace::info("Parsed ", declCount, " declarations");
}

} // namespace parser