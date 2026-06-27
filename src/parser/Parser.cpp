/**
 * @file Parser.cpp
 * @brief Implementation of core parsing functions.
 * 
 * This file implements the core parsing infrastructure:
 * - TokenStream: Safe token consumption with comment skipping
 * - ParserContext: Shared context across all files
 * - Error Recovery: synchronize() and synchronizeTo()
 * - parse(): Single entry point - parses the root file and all imports
 * - parseInternal(): Parses internal declarations of a file
 * - parseUseDecl(): Parses use declaration and imports the module
 */

#include "Parser.hpp"
#include "Lexer.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace parser {

// =============================================================================
// Error Recovery Functions
// =============================================================================

/**
 * @brief Synchronize the parser to the next statement or declaration boundary.
 * 
 * This function implements panic-mode error recovery. When a parsing error
 * occurs, the parser skips tokens until it finds a token that could start a
 * new statement or declaration. This prevents cascading errors and allows
 * the parser to continue parsing the rest of the file.
 * 
 * ## Synchronization Tokens
 * 
 * The parser synchronizes to the following tokens:
 * - Control flow: `if`, `switch`, `for`, `while`, `do`, `return`, `break`,
 *   `continue`
 * - Declarations: `let`, `const`, `struct`, `enum`, `trait`, `use`
 * - Blocks: `{`
 * - Special: `;` (statement terminator)
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 */
void synchronize(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("Synchronizing parser");
    
    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();
        
        switch (current) {
            case TokenType::IF:
            case TokenType::SWITCH:
            case TokenType::FOR:
            case TokenType::WHILE:
            case TokenType::DO:
            case TokenType::RETURN:
            case TokenType::BREAK:
            case TokenType::CONTINUE:
            case TokenType::LET:
            case TokenType::CONST:
            case TokenType::STRUCT:
            case TokenType::ENUM:
            case TokenType::TRAIT:
            case TokenType::USE:
            case TokenType::LBRACE:
            case TokenType::SEMICOLON:
                LOG_PARSER_DETAIL("Synchronized at token: ", debug::tokenTypeToString(current));
                return;
            default:
                break;
        }
        
        stream.advance();
    }
    
    LOG_PARSER("Synchronization reached EOF");
}

/**
 * @brief Synchronize the parser to one of a specific set of tokens.
 * 
 * This function skips tokens until it finds a token that matches any of the
 * specified stop tokens. This is useful for more targeted error recovery,
 * such as synchronizing to a closing brace or a specific keyword.
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @param stopTokens A list of token types to stop at
 */
void synchronizeTo(TokenStream& stream, ParserContext& ctx, std::initializer_list<TokenType> stopTokens) {
    LOG_PARSER("Synchronizing to stop tokens");
    
    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();
        
        for (TokenType stop : stopTokens) {
            if (current == stop) {
                LOG_PARSER_DETAIL("Synchronized at token: ", debug::tokenTypeToString(current));
                return;
            }
        }
        
        stream.advance();
    }
    
    LOG_PARSER("Synchronization reached EOF");
}

// =============================================================================
// parse() - ENTRY POINT
// =============================================================================

/**
 * @brief Parse a single source file and all its imports.
 * 
 * This is the ONE and ONLY entry point for parsing. It:
 * 1. Lexes the source
 * 2. Creates a TokenStream for the file
 * 3. Checks cyclic dependencies via ModuleResolver
 * 4. Parses the file's internal declarations
 * 5. Recursively parses all imported files via parseUseDecl()
 * 6. Returns the complete AST with all declarations
 * 
 * ## Visual Flow
 * 
 * parse("main.lucid")
 *     │
 *     ├── parseInternal(stream, ctx) → collects main.lucid decls
 *     │
 *     ├── parseUseDecl() → "use std.math"
 *     │   │
 *     │   ├── parse("std.math") → collects std.math decls
 *     │   │   └── resolver->cacheModule("std/math.lucid", ast)
 *     │   │
 *     │   └── ctx.importedDecls += std.math->decls
 *     │
 *     └── rootProgram = merge(fileDecls + importedDecls)
 *         │
 *         └── All declarations from main.lucid + std.math
 * 
 * @param path The file path
 * @param source The source code
 * @param ctx The parsing context (shared across all files)
 * @return ProgramAST* The complete AST, or nullptr on error
 */
ProgramAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx) {
    LOG_PARSER_MINIMAL("Parsing file: ", path);
    
    // ─── 1. Intern the file path ────────────────────────────────────────
    InternedString filePath = ctx.pool.intern(path);
    ctx.currentFilePath = filePath;
    ctx.clearErrors();
    
    // ─── 2. Check for Cyclic Dependencies ───────────────────────────────
    // We put the file that currently parsing into a stack, if the next
    // file is already in the stack then it's a Cyclic Dependencies
    if (ctx.resolver) {
        if (ctx.resolver->isParsing(filePath)) {
            LOG_PARSER("ERROR: Circular import detected: ", path);
            return nullptr;
        }
        ctx.resolver->pushParsing(filePath);
        LOG_PARSER_DETAIL("Pushed to parsing stack: ", path);
    }
    
    // ─── 3. Lex the Source ──────────────────────────────────────────────
    auto tokens = lexer::tokenize(source, path);
    if (tokens.empty()) {
        LOG_PARSER_MINIMAL("Lexer produced no tokens for: ", path);
        if (ctx.resolver) ctx.resolver->popParsing();
        return nullptr;
    }
    
    // Check for lexer errors
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            LOG_PARSER_MINIMAL("Lexer error in: ", path);
            if (ctx.resolver) ctx.resolver->popParsing();
            return nullptr;
        }
    }
    
    // ─── 4. Create TokenStream ──────────────────────────────────────────
    TokenStream stream(std::move(tokens), filePath);
    
    // ─── 5. Parse Internal Declarations ─────────────────────────────────
    std::vector<DeclPtr> allDecls;
    
    if (!parseInternal(stream, ctx, allDecls)) {
        if (ctx.resolver) ctx.resolver->popParsing();
        return nullptr;
    }
    
    // ─── 6. Build the root ProgramAST ────────────────────────────────────
    auto* rootProgram = ctx.arena.make<ProgramAST>();
    rootProgram->filePath = filePath;
    
    auto builder = ctx.arena.makeBuilder<DeclPtr>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    rootProgram->decls = builder.build();
    
    // ─── 7. Pop from Parsing Stack ──────────────────────────────────────
    if (ctx.resolver) {
        ctx.resolver->popParsing();
        LOG_PARSER_DETAIL("Popped from parsing stack: ", path);
    }
    
    // ─── 8. Cache the result ─────────────────────────────────────────────
    if (ctx.resolver && !ctx.hasErrors) {
        ctx.resolver->cacheModule(filePath, rootProgram);
        LOG_PARSER_DETAIL("Cached module: ", path);
    }
    
    LOG_PARSER_MINIMAL("Parse completed: ", allDecls.size(), " total declarations");
    
    return rootProgram;
}

// =============================================================================
// parseInternal() - Parse a file's internal declarations
// =============================================================================

/**
 * @brief Parse the internal declarations of a single file.
 * 
 * This does NOT handle imports - it only parses the declarations within
 * the current file. Imports are handled by parseUseDecl().
 * 
 * @param stream The token stream for the file
 * @param ctx The parsing context
 * @param outDecls Output vector to collect declarations
 * @return true if parsing succeeded, false on fatal error
 */
bool parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls) {
    LOG_PARSER_MINIMAL("Parsing internal declarations of: ", 
                       debug::internedToString(ctx.pool, ctx.currentFilePath));
    
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = stream.getPos();
    
    while (!stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(stream, ctx);
        size_t savedPos = stream.getPos();
        
        // Skip stray semicolons
        if (stream.check(TokenType::SEMICOLON)) {
            LOG_PARSER_DETAIL("Skipping stray semicolon at top level");
            stream.advance();
            continue;
        }
        
        // Parse a top-level declaration
        auto* decl = parseTopLevelDecl(stream, ctx);
        
        // Check for progress
        if (stream.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("NO PROGRESS - stuck on token: ", stream.peek().value,
                       " type: ", debug::tokenTypeToString(stream.peekType()));
            
            if (!stream.isAtEnd()) {
                stream.advance();
            }
            
            if (consecutiveFailures > 5) {
                LOG_PARSER("Too many consecutive failures, aggressive recovery");
                synchronize(stream, ctx);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = stream.getPos();
            
            LOG_PARSER_DETAIL("Parsed declaration #", declCount, " (",
                              debug::kindToString(decl->kind), ")");
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            outDecls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            LOG_PARSER("parseTopLevelDecl returned nullptr but made progress");
        }
        
        // Critical stuck detection
        if (stream.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("CRITICAL - still no progress after ", consecutiveFailures, " attempts");
            if (!stream.isAtEnd()) {
                stream.advance();
            }
            lastPos = stream.getPos();
        }
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        ctx.error(stream.currentLoc(), 
                  "Too many consecutive parsing errors (", MAX_CONSECUTIVE_FAILURES, "), aborting");
        LOG_PARSER("ERROR: Too many consecutive failures (", MAX_CONSECUTIVE_FAILURES, "), aborting");
        return false;
    }
    
    LOG_PARSER_MINIMAL("Parsed ", declCount, " internal declarations");
    return true;
}

// =============================================================================
// parseTopLevelDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseTopLevelDecl(TokenStream& stream, ParserContext& ctx) {
    // Check for USE declaration first (imports module)
    if (stream.check(TokenType::USE)) {
        return parseUseDecl(stream, ctx);
    }
    
    // Other declarations...
    if (stream.check(TokenType::STRUCT)) {
        return parseStructDecl(stream, ctx);
    }
    
    if (stream.check(TokenType::ENUM)) {
        return parseEnumDecl(stream, ctx);
    }
    
    if (stream.check(TokenType::TRAIT)) {
        return parseTraitDecl(stream, ctx);
    }
    
    if (stream.check(TokenType::LET) || stream.check(TokenType::CONST)) {
        // Need lookahead to distinguish var from func
        // For now, try var first
        if (looksLikeFuncDecl(stream, ctx)) {
            return parseFuncDecl(stream, ctx);
        }
        return parseVarDecl(stream, ctx);
    }
    
    // Unknown declaration
    ctx.error(stream.currentLoc(), "Unexpected token: ", stream.peek().value);
    synchronize(stream, ctx);
    return nullptr;
}

// =============================================================================
// parseUseDecl() - Parse a use declaration and import the module
// =============================================================================

/**
 * @brief Parse a use declaration and import the referenced module.
 * 
 * Grammar: `use path [as alias]`
 * 
 * Alias rules:
 * 1. If `as alias` is present, use the specified alias
 * 2. Otherwise, use the last component of the path as the alias
 *    - `std.math` → alias = "math"
 *    - `graphics.gl` → alias = "gl"
 * 
 * ## How It Works
 * 
 * This function:
 * 1. Parses the use path (e.g., "std.math")
 * 2. Determines the alias
 * 3. Creates the UseDeclAST node
 * 4. Resolves the path to a file
 * 5. Recursively parses the imported module (if not cached)
 * 6. Collects the imported declarations for merging
 * 
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @return UseDeclAST* The use declaration AST node, or nullptr on error
 */
UseDeclAST* parseUseDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    stream.consume(TokenType::USE);
    
    // ─── 1. Parse the use path ───────────────────────────────────────────
    auto pathParts = parseUsePath(stream, ctx);
    if (pathParts.empty()) {
        ctx.error(loc, "Expected module path after 'use'");
        synchronize(stream, ctx);
        return nullptr;
    }
    
    // Build the full use path string (dot-separated)
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    InternedString usePath = ctx.pool.intern(fullPath);
    
    // ─── 2. Determine the alias ──────────────────────────────────────────
    InternedString alias;
    std::string aliasStr;
    
    if (stream.match(TokenType::AS)) {
        // Explicit alias: `use path as alias`
        Token aliasTok = stream.consume(TokenType::IDENTIFIER);
        if (aliasTok.type != TokenType::EOF_TOKEN) {
            alias = ctx.pool.intern(aliasTok.value);
            aliasStr = std::string(ctx.pool.lookup(alias));
        } else {
            ctx.error(loc, "Expected alias name after 'as'");
            synchronize(stream, ctx);
            return nullptr;
        }
    } else {
        // Implicit alias: use the last component of the path
        // e.g., "std.math" → alias = "math"
        InternedString lastPart = pathParts.back();
        alias = lastPart;
        aliasStr = std::string(ctx.pool.lookup(alias));
    }
    
    // ─── 3. Create the UseDeclAST ────────────────────────────────────────
    auto* useDecl = ctx.arena.make<UseDeclAST>();
    useDecl->loc = loc;
    useDecl->path = usePath;
    useDecl->alias = alias;
    
    // ─── 4. Import the module ────────────────────────────────────────────
    if (ctx.resolver) {
        // Resolve the use path to a file path
        InternedString filePath = ctx.resolver->resolveUsePath(usePath);
        if (!filePath.isValid()) {
            ctx.error(loc, "Module not found: '", usePath, "'");
            synchronize(stream, ctx);
            return useDecl;
        }
        
        // Check if already parsed
        ProgramAST* importedModule = ctx.resolver->getParsedModule(filePath);
        
        if (!importedModule) {
            // Read the source file
            std::string source = ctx.resolver->readModuleSource(filePath);
            if (source.empty()) {
                // ============================================================
                // NOTE: if the module is empty then we simply don't care and
                // move on
                // ============================================================
                ctx.error(loc, "Failed to read module: '", usePath, "'");
                synchronize(stream, ctx);
                return useDecl;
            }
            
            // ============================================================
            // Parse the module (recursive call!)
            // ============================================================
            std::string pathStr = std::string(ctx.pool.lookup(filePath));
            importedModule = parse(pathStr, source, ctx);
            
            if (!importedModule) {
                ctx.error(loc, "Failed to parse module: '", usePath, "'");
                synchronize(stream, ctx);
                return useDecl;
            }
            
            // Cache the module
            ctx.resolver->cacheModule(filePath, importedModule);
        }
        
        // ─── 5. NOTE: Imported declarations are automatically collected
        //     by parse() when it parses the imported module.
        //     No need to manually collect them here.
        
    } else {
        ctx.error(loc, "No module resolver available for import: '", usePath, "'");
        synchronize(stream, ctx);
        return useDecl;
    }
    
    LOG_PARSER_DETAIL("Parsed use declaration: '", fullPath, "' as '", aliasStr, "'");
    
    return useDecl;
}


} // namespace parser