/**
 * @file Parser.cpp
 * @brief Implementation of TokenStream, ParserState, and core parsing functions.
 * 
 * This file implements the core parsing infrastructure:
 * - TokenStream: Safe token consumption with comment skipping
 * - ParserState: Mutable context for a parsing session
 * - Error Recovery: synchronize() and synchronizeTo()
 * - parse(): Entry point - parses the root file and all imports
 * - parseFile(): Helper - parses a single file
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
 * ## Usage
 * 
 * ```cpp
 * if (parseError) {
 *     state.error("Failed to parse expression");
 *     synchronize(state);
 * }
 * ```
 * 
 * @param state The parser state containing the token stream.
 * 
 * @see synchronizeTo() for synchronizing to specific tokens.
 */
void synchronize(ParserState& state) {
    LOG_PARSER("Synchronizing parser");
    
    while (!state.stream.isAtEnd()) {
        TokenType current = state.stream.peekType();
        
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
                LOG_PARSER_DETAIL("Synchronized at token: %s", 
                           debug::tokenTypeToString(current).c_str());
                return;
            default:
                break;
        }
        
        state.stream.advance();
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
 * ## Usage
 * 
 * ```cpp
 * // Synchronize to a closing brace or the next case
 * synchronizeTo(state, {TokenType::RBRACE, TokenType::CASE, TokenType::DEFAULT});
 * ```
 * 
 * @param state      The parser state containing the token stream.
 * @param stopTokens A list of token types to stop at.
 * 
 * @see synchronize() for general error recovery.
 */
void synchronizeTo(ParserState& state, std::initializer_list<TokenType> stopTokens) {
    LOG_PARSER("Synchronizing to stop tokens");
    
    while (!state.stream.isAtEnd()) {
        TokenType current = state.stream.peekType();
        
        for (TokenType stop : stopTokens) {
            if (current == stop) {
                LOG_PARSER_DETAIL("Synchronized at token: %s", 
                           debug::tokenTypeToString(current).c_str());
                return;
            }
        }
        
        state.stream.advance();
    }
    
    LOG_PARSER("Synchronization reached EOF");
}

// =============================================================================
// parse() - ENTRY POINT
// =============================================================================

/**
 * @brief Parse the root file and all imported files.
 * 
 * This is the ONE entry point for parsing. It:
 * 1. Parses the root file
 * 2. Recursively parses all imported files via parseUseDecl()
 * 3. Collects all declarations into a single ProgramAST
 * 
 * @param state Parser state for the root file
 * @return ProgramAST* The complete AST with all declarations from all files
 */
ProgramAST* parse(ParserState& state) {
    LOG_PARSER_MINIMAL("Starting parse of root file: %s", 
                debug::internedToString(state.pool, state.filePath).c_str());
    
    // ─── 1. Create the root Program AST Node ─────────────────────────────
    auto* program = state.arena.make<ProgramAST>();
    program->filePath = state.filePath;
    program->packageName = InternedString();
    
    // ─── 2. Parse Top-Level Declarations ─────────────────────────────────
    std::vector<DeclPtr> allDecls;
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = state.stream.getPos();
    
    while (!state.stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(state);
        size_t savedPos = state.stream.getPos();
        
        // Skip stray semicolons
        if (state.stream.check(TokenType::SEMICOLON)) {
            LOG_PARSER_DETAIL("Skipping stray semicolon at top level");
            state.stream.advance();
            continue;
        }
        
        // Parse a top-level declaration
        auto* decl = parseTopLevelDecl(state);
        
        // Check for progress
        if (state.stream.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("NO PROGRESS - stuck on token '%s' (type=%s), failures: %d",
                       state.stream.peek().value.c_str(),
                       debug::tokenTypeToString(state.stream.peekType()).c_str(),
                       consecutiveFailures);
            
            if (!state.stream.isAtEnd()) {
                state.stream.advance();
            }
            
            if (consecutiveFailures > 5) {
                LOG_PARSER("Too many consecutive failures, aggressive recovery");
                synchronize(state);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = state.stream.getPos();
            
            LOG_PARSER_DETAIL("Parsed declaration #%d (%s)", 
                             declCount, debug::kindToString(decl->kind).c_str());
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            
            // ─── 3. If this is a USE declaration, the import already happened
            //     inside parseUseDecl(). We just add the decl to the list.
            allDecls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            LOG_PARSER("parseTopLevelDecl returned nullptr but made progress");
        }
        
        // Critical stuck detection
        if (state.stream.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("CRITICAL - still no progress after %d attempts, forcing advance",
                      consecutiveFailures);
            if (!state.stream.isAtEnd()) {
                state.stream.advance();
            }
            lastPos = state.stream.getPos();
        }
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        state.error("Too many consecutive parsing errors (%d), aborting",
                   MAX_CONSECUTIVE_FAILURES);
        LOG_PARSER("ERROR: Too many consecutive failures (%d), aborting",
                  MAX_CONSECUTIVE_FAILURES);
    }
    
    // ─── 4. Build the AST ─────────────────────────────────────────────────
    auto builder = state.arena.makeBuilder<DeclPtr>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    program->decls = builder.build();
    
    LOG_PARSER_MINIMAL("Parse complete: %d total declarations across all files", 
                      allDecls.size());
    
    return program;
}

// =============================================================================
// parseFile() - Parse a SINGLE file
// =============================================================================

/**
 * @brief Parse a single source file into a ProgramAST.
 * 
 * This is a helper function that parses ONE file. It:
 * 1. Lexes the source
 * 2. Creates a ParserState with TokenStream
 * 3. Checks cyclic dependencies via ModuleResolver
 * 4. Parses the file's internal declarations
 * 
 * @param path The file path
 * @param source The source code
 * @param pool The string pool
 * @param arena The AST arena
 * @param resolver The module resolver (for cyclic detection)
 * @return ProgramAST* The AST for this file, or nullptr on error
 */
ProgramAST* parseFile(const std::string& path, 
                      const std::string& source,
                      StringPool& pool, 
                      ASTArena& arena,
                      ModuleResolver* resolver) {
    LOG_PARSER_MINIMAL("Parsing file: %s", path.c_str());
    
    // ─── 1. Intern the file path ────────────────────────────────────────
    InternedString filePath = pool.intern(path);
    
    // ─── 2. Check for Cyclic Dependencies ───────────────────────────────
    if (resolver) {
        if (resolver->isParsing(filePath)) {
            // Cyclic dependency detected
            LOG_PARSER("ERROR: Circular import detected: %s", path.c_str());
            return nullptr;
        }
        // Push to stack before parsing
        resolver->pushParsing(filePath);
        LOG_PARSER_DETAIL("Pushed to parsing stack: %s", path.c_str());
    }
    
    // ─── 3. Lex the Source ──────────────────────────────────────────────
    auto tokens = lexer::tokenize(source, path);
    if (tokens.empty()) {
        LOG_PARSER_MINIMAL("Lexer produced no tokens for: %s", path.c_str());
        if (resolver) resolver->popParsing();
        return nullptr;
    }
    
    // Check for lexer errors
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            LOG_PARSER_MINIMAL("Lexer error in: %s", path.c_str());
            if (resolver) resolver->popParsing();
            return nullptr;
        }
    }
    
    // ─── 4. Create TokenStream and ParserState ──────────────────────────
    TokenStream stream(std::move(tokens), filePath);
    ParserState state(std::move(stream), filePath, pool, arena);
    
    // ─── 5. Set up Module Resolution ────────────────────────────────────
    if (resolver) {
        state.moduleResolver = resolver;
    }
    

    // ================================================================================
    // [Entry Point] - Parse internal structure of a file
    // ================================================================================

    // ─── 6. Parse Internal Declarations ─────────────────────────────────
    ProgramAST* program = parseInternal(state);

    
    // ─── 7. Pop from Parsing Stack ──────────────────────────────────────
    if (resolver) {
        resolver->popParsing();
        LOG_PARSER_DETAIL("Popped from parsing stack: %s", path.c_str());
    }
    
    // ─── 8. Cache the result if successful ─────────────────────────────
    if (program && resolver && !state.hasErrors) {
        resolver->cacheModule(filePath, program);
        LOG_PARSER_DETAIL("Cached module: %s", path.c_str());
    }
    
    // ─── 9. Report Results ──────────────────────────────────────────────
    if (state.hasErrors) {
        LOG_PARSER_MINIMAL("Parse completed with errors in: %s", path.c_str());
    } else {
        LOG_PARSER_MINIMAL("Parse completed successfully: %s", path.c_str());
    }
    
    return program;
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
 * @param state The parser state
 * @return ProgramAST* The AST for this file's internal declarations
 */
ProgramAST* parseInternal(ParserState& state) {
    LOG_PARSER_MINIMAL("Parsing internal declarations of: %s", 
                debug::internedToString(state.pool, state.filePath).c_str());
    
    // ─── 1. Create the Program AST Node ──────────────────────────────────
    auto* program = state.arena.make<ProgramAST>();
    program->filePath = state.filePath;
    program->packageName = InternedString();
    
    // ─── 2. Parse Declarations ───────────────────────────────────────────
    std::vector<DeclPtr> decls;
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = state.stream.getPos();
    
    while (!state.stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(state);
        size_t savedPos = state.stream.getPos();
        
        // Skip stray semicolons
        if (state.stream.check(TokenType::SEMICOLON)) {
            LOG_PARSER_DETAIL("Skipping stray semicolon at top level");
            state.stream.advance();
            continue;
        }
        
        // Parse a top-level declaration
        auto* decl = parseTopLevelDecl(state);
        
        // Check for progress
        if (state.stream.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("NO PROGRESS - stuck on token '%s' (type=%s), failures: %d",
                       state.stream.peek().value.c_str(),
                       debug::tokenTypeToString(state.stream.peekType()).c_str(),
                       consecutiveFailures);
            
            if (!state.stream.isAtEnd()) {
                state.stream.advance();
            }
            
            if (consecutiveFailures > 5) {
                LOG_PARSER("Too many consecutive failures, aggressive recovery");
                synchronize(state);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = state.stream.getPos();
            
            LOG_PARSER_DETAIL("Parsed declaration #%d (%s)", 
                             declCount, debug::kindToString(decl->kind).c_str());
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            decls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            LOG_PARSER("parseTopLevelDecl returned nullptr but made progress");
        }
        
        // Critical stuck detection
        if (state.stream.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("CRITICAL - still no progress after %d attempts, forcing advance",
                      consecutiveFailures);
            if (!state.stream.isAtEnd()) {
                state.stream.advance();
            }
            lastPos = state.stream.getPos();
        }
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        state.error("Too many consecutive parsing errors (%d), aborting",
                   MAX_CONSECUTIVE_FAILURES);
        LOG_PARSER("ERROR: Too many consecutive failures (%d), aborting",
                  MAX_CONSECUTIVE_FAILURES);
    }
    
    // ─── 3. Build the AST ─────────────────────────────────────────────────
    auto builder = state.arena.makeBuilder<DeclPtr>();
    for (auto* d : decls) {
        builder.push_back(d);
    }
    program->decls = builder.build();
    
    LOG_PARSER_MINIMAL("Parsed %d internal declarations", declCount);
    
    return program;
}

// =============================================================================
// parseUseDecl() - Parse a use declaration and import the module
// =============================================================================

/**
 * @brief Parse a use declaration and import the referenced module.
 * 
 * Grammar: `use path [as alias]`
 * 
 * This function:
 * 1. Parses the use path (e.g., "std.io")
 * 2. Parses optional alias
 * 3. Resolves the path to a file
 * 4. Imports the module (parses if not cached)
 * 5. Creates the UseDeclAST node
 * 
 * @param state The parser state
 * @return UseDeclAST* The use declaration AST node, or nullptr on error
 */
UseDeclAST* parseUseDecl(ParserState& state) {
    SourceLocation loc = state.stream.currentLoc();
    state.stream.consume(TokenType::USE);
    
    // ─── 1. Parse the use path ───────────────────────────────────────────
    auto pathParts = parseUsePath(state);
    if (pathParts.empty()) {
        state.error(loc, "Expected module path after 'use'");
        synchronize(state);
        return nullptr;
    }
    
    // Build the full use path string
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(state.pool.lookup(pathParts[i]));
    }
    InternedString usePath = state.pool.intern(fullPath);
    
    // ─── 2. Parse optional alias ─────────────────────────────────────────
    InternedString alias;
    std::string aliasStr;  // For logging
    if (state.stream.match(TokenType::AS)) {
        Token aliasTok = state.stream.consume(TokenType::IDENTIFIER);
        if (aliasTok.type != TokenType::EOF_TOKEN) {
            alias = state.pool.intern(aliasTok.value);
            aliasStr = std::string(state.pool.lookup(alias));
        } else {
            state.error("Expected alias name after 'as'");
            synchronize(state);
            return nullptr;
        }
    }
    
    // ─── 3. Import the module ────────────────────────────────────────────
    ProgramAST* importedModule = nullptr;
    
    if (state.moduleResolver) {
        // Resolve the use path to a file path
        InternedString filePath = state.moduleResolver->resolveUsePath(usePath);
        if (!filePath.isValid()) {
            state.error("Module not found: '", usePath, "'");
            synchronize(state);
            return nullptr;
        }
        
        // Check if already parsed
        importedModule = state.moduleResolver->getParsedModule(filePath);
        
        if (!importedModule) {
            // Read the source file
            std::string source = state.moduleResolver->readModuleSource(filePath);
            if (source.empty()) {
                state.error("Failed to read module: '", usePath, "'");
                synchronize(state);
                return nullptr;
            }
            
            // Parse the module
            std::string pathStr = std::string(state.pool.lookup(filePath));

            // ================================================================================
            // [Entry Point] - Parse the file and repeat the loop until all files are parsed
            // ================================================================================
            importedModule = parseFile(pathStr, source, state.pool, state.arena, state.moduleResolver);
            
            if (!importedModule) {
                state.error("Failed to parse module: '", usePath, "'");
                synchronize(state);
                return nullptr;
            }
            
            // Cache the module
            state.moduleResolver->cacheModule(filePath, importedModule);
        }
        
    } else {
        state.error("No module resolver available for import: '", usePath, "'");
        synchronize(state);
        return nullptr;
    }
    
    // ─── 4. Create the UseDeclAST ────────────────────────────────────────
    auto* useDecl = state.arena.make<UseDeclAST>();
    useDecl->loc = loc;
    
    // Build the path span
    auto builder = state.arena.makeBuilder<InternedString>();
    for (const auto& part : pathParts) {
        builder.push_back(part);
    }
    useDecl->path = builder.build();
    
    if (alias.isValid()) {
        useDecl->alias = alias;
    }
    
    // ─── 5. Log the result (CLEAN - no printf-style!) ──────────────────
    if (alias.isValid()) {
        LOG_PARSER_DETAIL("Parsed use declaration: '", fullPath, "' as '", aliasStr, "'");
    } else {
        LOG_PARSER_DETAIL("Parsed use declaration: '", fullPath, "'");
    }
    
    return useDecl;
}

} // namespace parser