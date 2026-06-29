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
#include "core/ast/BaseAST.hpp"
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
 * @tparam StopTokens The token types to stop at (variadic)
 * @param stream The token stream for the current file
 * @param ctx The parsing context
 * @param stopTokens The token types to stop at
 */
template<typename... StopTokens>
void synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens) {
    LOG_PARSER("Synchronizing to stop tokens");
    
    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();
        
        // Check if current token matches any stop token
        if (((current == stopTokens) || ...)) {
            LOG_PARSER_DETAIL("Synchronized at token: ", debug::tokenTypeToString(current));
            return;
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
 * This function is the core of the parser's file-level processing. It consumes
 * tokens from the stream and produces a collection of top-level declarations.
 * 
 * ## Program Flow
 * 
 * ```
 * parseInternal()
 *     │
 *     ├── 1. Setup Phase
 *     │   ├── Log entry
 *     │   └── Initialize tracking variables
 *     │       ├── declCount = 0
 *     │       ├── consecutiveFailures = 0
 *     │       └── lastPos = stream.getPos()
 *     │
 *     ├── 2. Main Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && consecutiveFailures < MAX)
 *     │       │
 *     │       ├── 2.1 Harvest Doc Comment
 *     │       │   └── harvestDocComment() → scans backward for attached comments
 *     │       │
 *     │       ├── 2.2 Skip Stray Semicolons
 *     │       │   └── if (stream.check(SEMICOLON)) → advance() and continue
 *     │       │
 *     │       ├── 2.3 Parse Declaration
 *     │       │   └── parseDecl()
 *     │       │       │
 *     │       │       ├── USE → parseUseDecl()
 *     │       │       │   └── Imports module (recursive)
 *     │       │       │
 *     │       │       ├── STRUCT → parseStructDecl()
 *     │       │       │
 *     │       │       ├── ENUM → parseEnumDecl()
 *     │       │       │
 *     │       │       ├── TRAIT → parseTraitDecl()
 *     │       │       │
 *     │       │       └── LET/CONST → parseVarDecl() or parseFuncDecl()
 *     │       │
 *     │       ├── 2.4 Progress Check
 *     │       │   │
 *     │       │   ├── if (stream.getPos() == savedPos)
 *     │       │   │   ├── No progress → consecutiveFailures++
 *     │       │   │   ├── Log stuck token
 *     │       │   │   ├── Force consume token if not at EOF
 *     │       │   │   └── if (consecutiveFailures > 5)
 *     │       │   │       └── synchronize() → aggressive recovery
 *     │       │   │
 *     │       │   ├── else if (decl != nullptr)
 *     │       │   │   ├── declCount++
 *     │       │   │   ├── consecutiveFailures = 0
 *     │       │   │   ├── lastPos = stream.getPos()
 *     │       │   │   ├── Attach doc comment if present
 *     │       │   │   └── outDecls.push_back(decl)
 *     │       │   │
 *     │       │   └── else
 *     │       │       └── consecutiveFailures = 0
 *     │       │           (made progress but returned nullptr)
 *     │       │
 *     │       └── 2.5 Critical Stuck Detection
 *     │           └── if (stream.getPos() == lastPos && consecutiveFailures > 10)
 *     │               ├── Log critical stuck
 *     │               ├── Force consume token
 *     │               └── lastPos = stream.getPos()
 *     │
 *     ├── 3. Error Handling
 *     │   │
 *     │   ├── if (consecutiveFailures >= MAX)
 *     │   │   ├── ctx.error(stream, DiagCode::E0002, MAX)
 *     │   │   └── return false
 *     │   │
 *     │   └── if (stream.isAtEnd() && ctx.hasErrors)
 *     │       └── Log that errors occurred (safety net)
 *     │
 *     └── 4. Return
 *         ├── LOG_PARSER_MINIMAL("Parsed N declarations")
 *         └── return true
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * The parser uses a multi-level error recovery strategy:
 * 
 * ### Level 1: Per-Declaration Recovery
 * - Each declaration parser attempts to recover from local errors
 * - Returns nullptr on failure, allowing the loop to continue
 * 
 * ### Level 2: Progress Detection
 * - If the stream position doesn't advance, we force consume a token
 * - Prevents infinite loops on malformed input
 * 
 * ### Level 3: Panic Recovery
 * - After 5 consecutive failures, call `synchronize()`
 * - Skips tokens until a statement/declaration boundary is found
 * 
 * ### Level 4: Abort
 * - After 100 consecutive failures, abort parsing
 * - Prevents infinite loops in pathological cases
 * 
 * ## Declaration Collection
 * 
 * All successfully parsed declarations are collected into `outDecls`:
 * - Each declaration has its doc comment attached (if any)
 * - Declarations are stored in source order
 * - Imported modules are parsed recursively via `parseUseDecl()`
 * 
 * ## Token Stream State
 * 
 * After this function completes, the token stream will be at:
 * - EOF (normal case)
 * - A recovery point (if errors occurred)
 * - The position where parsing was aborted (fatal error)
 * 
 * ## Thread Safety
 * 
 * Not thread-safe. Requires exclusive access to the token stream.
 * 
 * @param stream The token stream for the file being parsed.
 *              This stream is consumed during parsing.
 * @param ctx The parsing context (shared across all files).
 *           Contains error tracking, allocators, and module resolver.
 * @param outDecls Output vector to collect successfully parsed declarations.
 *                Each declaration will have its doc comment attached.
 *                The vector is cleared by the caller before calling this function.
 * 
 * @return true if parsing succeeded (including with recoverable errors).
 *         false if a fatal error occurred and parsing was aborted.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * std::vector<DeclPtr> decls;
 * if (!parseInternal(stream, ctx, decls)) {
 *     // Fatal error - cannot continue
 *     return nullptr;
 * }
 * // All declarations are in decls
 * ```
 * 
 * @note This function does NOT handle imports directly. Imports are handled
 *       by parseUseDecl() which is called from parseDecl().
 *       The imported module's declarations are collected recursively
 *       when parseUseDecl() calls parse() on the imported file.
 * 
 * @warning If this function returns false, the token stream may be in an
 *          inconsistent state. The caller should not attempt to continue
 *          parsing the same file.
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
        
        // ─── Parse a top-level declaration ──────────────────────────────
        // The declaration parser will consume its own terminating ';'
        auto* decl = parseDecl(stream, ctx);
        
        // ─── Check if we're at EOF after parsing a declaration ──────────
        // If we parsed a declaration and then hit EOF, but the declaration
        // was incomplete (e.g., missing closing brace), we should have
        // already reported an error. But if we're at EOF and the declaration
        // is nullptr, we might have a malformed declaration.
        if (!decl && stream.isAtEnd()) {
            // We reached EOF while trying to parse a declaration.
            // This could be because we're at EOF after a comment, or
            // there's a malformed declaration.
            // If we have any errors, break out.
            if (ctx.hasErrors) {
                break;
            }
            // No error was reported, but we have no declaration and we're at EOF.
            // This could be valid (trailing whitespace after last declaration).
            // Let's break out of the loop gracefully.
            LOG_PARSER_DETAIL("Reached EOF after parsing declarations");
            break;
        }
        
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
            LOG_PARSER("parseDecl returned nullptr but made progress");
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
    
    // ─── Final EOF check ─────────────────────────────────────────────────
    // If we reached EOF while there were unclosed constructs, the helper
    // functions should have reported errors. This is just a safety net.
    if (stream.isAtEnd() && ctx.hasErrors) {
        // Errors already reported - nothing to do
        LOG_PARSER_DETAIL("Parse ended at EOF with errors");
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        ctx.error(stream, DiagCode::E0002, MAX_CONSECUTIVE_FAILURES);
        LOG_PARSER("ERROR: Too many consecutive failures (", MAX_CONSECUTIVE_FAILURES, "), aborting");
        return false;
    }
    
    LOG_PARSER_MINIMAL("Parsed ", declCount, " internal declarations");
    return true;
}

// =============================================================================
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
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

    // ─── Consume semicolon ────────────────────────────────────────────────
    if (!stream.match(TokenType::SEMICOLON)) {
        ctx.error(stream, DiagCode::E1007, ";", stream.peekValue());
        synchronize(stream, ctx);
        // Continue anyway - return the declaration
    }
    
    // Unknown declaration
    ctx.error(stream, DiagCode::E1008, stream.peekValue());
    synchronize(stream, ctx);
    return nullptr;
}

} // namespace parser