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
 */

#include "Parser.hpp"
#include "core/Tokens.hpp"
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
    
    // ─── Parse declarations until EOF ──────────────────────────────────────
    while (!stream.isAtEnd() && ctx.canContinue()) {
        // ─── Filter invalid tokens in this context ──────────────────────
        // A top-level declaration starts with a declaration keyword
        // We also skip stray semicolons
        if (!stream.check(TokenType::SEMICOLON) &&
            !stream.check(TokenType::AT_SIGN) &&  // Attributes are part of declarations
            !is_declaration_keyword(stream.peekType())) {
            
            // Check if it's a control flow or concurrency keyword (invalid at top level)
            if (is_control_flow_keyword(stream.peekType()) || 
                is_concurrency_keyword(stream.peekType())) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "statement keyword '", stream.peekValue(), 
                                        "' cannot appear at top level - expected a declaration");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "unexpected token '", stream.peekValue(), 
                                        "' - expected declaration");
            }
            
            // Synchronize to nearest valid declaration to recover
            synchronizeToDeclBoundary(stream, ctx, {});
            
            if (stream.isAtEnd()) {
                break;
            }
        }

        // ─── Skip stray semicolons ──────────────────────────────────────
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        // ─── Progress guard ──────────────────────────────────────────────
        // Save position before parsing to detect zero-progress
        size_t savedPos = stream.getPos();

        // ─── Parse declaration ──────────────────────────────────────────
        DeclAST* decl = parseDecl(stream, ctx);
        if (decl) {
            outDecls.push_back(decl);
            declCount++;
        } else {
            // parseDecl reported the error. If no progress was made, consume
            // one token to avoid infinite loop.
            if (stream.getPos() == savedPos && !stream.isAtEnd()) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "skipping unexpected token '", stream.peekValue(), "'");
                stream.consume();
            }
        }
    }
    
    Trace::info("Parsed ", declCount, " declarations");
}

} // namespace parser