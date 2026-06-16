/**
 * @file Parser.cpp
 * @brief Core parsing infrastructure and top-level dispatch.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the foundational components of the Luc parser:
 *   - TokenStream: Safe token consumption with comment skipping
 *   - Error handling: Panic-mode recovery and per-function error reporting
 *   - List helpers: Temporary vector builders for expression/type/stmt/param lists
 *   - Attribute parsing: @inline, @deprecated, @extern, etc.
 *   - Top-level dispatch: parse() and parseDeclaration()
 * 
 * ## TokenStream Purpose
 * 
 * TokenStream wraps the raw token vector and provides:
 *   - Automatic comment skipping (LINE_COMMENT, DOC_COMMENT are invisible to grammar)
 *   - Position manipulation (getPos/setPos) for lookahead and error recovery
 *   - Safe token consumption with consume() that reports errors on mismatch
 * 
 * ## Error Recovery Strategy
 * 
 *   - **Panic mode**: synchronize() skips tokens until a statement or declaration
 *     boundary (IF, FOR, WHILE, LET, CONST, RBRACE, etc.)
 *   - **Custom recovery**: synchronizeTo() stops at caller-specified token types
 *   - **Per-function recovery**: On parse failure, functions may return nullptr
 *     and the caller calls synchronize()
 * 
 * ## List Helpers
 * 
 * These functions return std::vector (temporary) for collecting sequences:
 *   - parseExprList()   : comma-separated expressions until end token
 *   - parseTypeList()   : comma-separated types until end token
 *   - parseStmtList()   : statements until end token
 *   - parseParamList()  : function parameters until ')'
 * 
 * The caller is responsible for converting to ArenaSpan using SpanBuilder.
 * 
 * ## Attribute Parsing
 * 
 * Attributes use the syntax @name or @name(args). They are collected as
 * temporary vectors and attached to declarations via attachMetadata().
 * 
 * ## Top-Level Parsing Flow
 * 
 *   parse()
 *     ├── parsePackageDecl()     (mandatory, first non‑comment)
 *     └── while (!ts_.isAtEnd())
 *           └── parseTopLevelDecl()
 *                 └── parseDeclaration()
 *                       └── dispatches to specific parser (use, struct, etc.)
 * 
 * @see ParserDecl.cpp for declaration parsers
 * @see ParserExpr.cpp for expression parsers
 * @see ParserStmt.cpp for statement parsers
 * @see ParserType.cpp for type parsers
 */

#include "Parser.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// ============================================================================
// TokenStream Implementation
// ============================================================================

/**
 * @brief Constructs a TokenStream from a vector of tokens.
 * 
 * @param tokens The token vector from the lexer (takes ownership via move)
 * @param filePath The source file path (used for error reporting locations)
 * 
 * The stream starts at position 0. Comments are NOT skipped during construction;
 * they are skipped on-the-fly by peek() and advance() methods.
 */
TokenStream::TokenStream(std::vector<Token> tokens, InternedString filePath)
    : tokens_(std::move(tokens)), filePath_(filePath) {}

/**
 * @brief Finds the next non-comment token position starting from `start`.
 * 
 * @param start The index to begin scanning from
 * @return The index of the first token that is not LINE_COMMENT or DOC_COMMENT
 * 
 * This is the core helper for comment filtering. It scans forward until it hits
 * a non-comment token or the end of the token vector.
 * 
 * @note Called by peek(), advance(), and peekNext() to skip comments transparently.
 */
size_t TokenStream::skipCommentsFrom(size_t start) const {
    size_t i = start;
    while (i < tokens_.size() &&
           (tokens_[i].type == TokenType::LINE_COMMENT ||
            tokens_[i].type == TokenType::DOC_COMMENT)) {
        ++i;
    }
    return i;
}

/**
 * @brief Checks if the stream has consumed all non-comment tokens.
 * 
 * @return true if no more non-comment tokens remain, false otherwise
 * 
 * This handles comment skipping: if only comments remain at the current position,
 * they are considered "end" because the grammar never sees them.
 */
bool TokenStream::isAtEnd() const {
    return skipCommentsFrom(pos_) >= tokens_.size();
}

/**
 * @brief Returns the next non-comment token without consuming it.
 * 
 * @return const reference to the next token, or eofToken if at end
 * 
 * Comment tokens are transparently skipped. If the stream is exhausted,
 * returns a sentinel EOF token (not from the original token vector).
 */
const Token& TokenStream::peek() const {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) return eofToken;
    return tokens_[idx];
}

/**
 * @brief Consumes and returns the next non-comment token.
 * 
 * @return The consumed token, or eofToken if at end
 * 
 * Advances the stream position past the next non-comment token.
 * Any comments between the current position and that token are also skipped.
 * This guarantees forward progress on every call.
 */
Token TokenStream::advance() {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) {
        ++pos_;
        return eofToken;
    }
    const Token& tok = tokens_[idx];
    pos_ = idx + 1;
    return tok;
}

/**
 * @brief Checks if the next non-comment token has the given type.
 * 
 * @param type The token type to check for
 * @return true if the next token matches (and stream is not at end)
 * 
 * Non-consuming; comments are automatically skipped.
 */
bool TokenStream::check(TokenType type) const {
    return !isAtEnd() && peek().type == type;
}

/**
 * @brief Checks if the next non-comment token matches any of the given types.
 * 
 * @param types List of token types to check against
 * @return true if the next token matches any type in the list
 * 
 * Useful for parsing ambiguous constructs (e.g., checking if next token
 * could start a type, expression, or statement).
 */
bool TokenStream::checkAny(std::initializer_list<TokenType> types) const {
    for (TokenType t : types)
        if (check(t)) return true;
    return false;
}

/**
 * @brief If the next token matches `type`, consumes it and returns true.
 * 
 * @param type The token type to match
 * @return true if the token was consumed, false otherwise
 * 
 * This is a "try-consume" operation: it only consumes if the token matches,
 * otherwise it leaves the stream unchanged. Used for optional syntax elements.
 */
bool TokenStream::match(TokenType type) {
    if (!check(type)) return false;
    LOG_LEXER_EXTREME("TokenStream::match: consumed " << LucDebug::tokenTypeToString(type));
    advance();
    return true;
}

/**
 * @brief Tries to match any token type from a list; consumes the first match.
 * 
 * @param types List of token types to try in order
 * @return true if any token type matched and was consumed
 * 
 * Checks each type in the given order; consumes the first match.
 * Useful for keywords that can appear in the same syntactic position
 * (e.g., `let` vs `const` for variable declarations).
 */
bool TokenStream::matchAny(std::initializer_list<TokenType> types) {
    for (TokenType t : types)
        if (match(t)) return true;
    return false;
}

/**
 * @brief If the next token matches `type`, consumes and returns it.
 * 
 * @param type The token type to check for
 * @return The consumed token if matched, or std::nullopt otherwise
 * 
 * Unlike match() which returns bool, this returns the actual token
 * when callers need its value (e.g., identifier name, number literal).
 */
std::optional<Token> TokenStream::consumeIf(TokenType type) {
    if (check(type)) return advance();
    return std::nullopt;
}

/**
 * @brief Requires the next token to be `type`; consumes it or reports error.
 * 
 * @param type The required token type
 * @return The consumed token (or a dummy token on error)
 * 
 * This is the workhorse for parsing mandatory syntax elements.
 * On mismatch, it reports an error using the diagnostic system and returns
 * a dummy token to allow parsing to continue (error recovery).
 * 
 * @note The dummy token has the expected type but empty value, line 0, column 0.
 */
Token TokenStream::consume(TokenType type) {
    if (check(type)) return advance();
    SourceLocation loc = currentLoc();
    // Return a dummy token for recovery
    return {type, "", 0, 0};
}

/**
 * @brief Returns the source location of the next non-comment token.
 * 
 * @return SourceLocation with line and column (1-based)
 * 
 * Used for attaching source locations to AST nodes and error reporting.
 * The location is derived from the token's stored line/column values.
 */
SourceLocation TokenStream::currentLoc() const {
    return locOf(peek());
}

/**
 * @brief Converts a token to a SourceLocation.
 * 
 * @param tok The token to convert
 * @return SourceLocation with the token's line and column
 * 
 * Extracts line and column from the token (stored as ints during lexing)
 * and packages them into a SourceLocation (uint32_t for memory efficiency).
 */
SourceLocation TokenStream::locOf(const Token& tok) const {
    return SourceLocation(static_cast<uint32_t>(tok.line),
                          static_cast<uint32_t>(tok.column));
}

/**
 * @brief Returns the type of the token two steps ahead (skipping comments).
 * 
 * @return TokenType of the token after the next non-comment token
 * 
 * Useful for lookahead in ambiguous grammars, e.g., checking if after
 * an identifier comes `::` (namespace access) vs `(` (function call).
 * Comments are automatically skipped in both position calculations.
 */
TokenType TokenStream::peekNextType() const {
    return peekNext().type;
}

/**
 * @brief Returns the token two steps ahead (skipping comments).
 * 
 * @return const reference to the token after the next non-comment token
 * 
 * First skips comments from current position to find the next token,
 * then skips again to find the token after that. Returns eofToken if
 * the stream doesn't have enough tokens.
 */
const Token& TokenStream::peekNext() const {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) return eofToken;
    // Move one step forward (skip the current non‑comment token)
    size_t nextIdx = skipCommentsFrom(idx + 1);
    if (nextIdx >= tokens_.size()) return eofToken;
    return tokens_[nextIdx];
}

/**
 * @brief Raw token access at a specific index (does NOT skip comments).
 * 
 * @param offset Index into the token vector (0-based)
 * @return const reference to the token at that position, or eofToken if out of bounds
 * 
 * @warning This bypasses comment skipping! Only use for lookahead helpers
 *          that manually manage comment filtering, or for doc comment harvesting.
 * 
 * Used by harvestDocComment() to scan backward through comments, and by
 * lookahead helpers that need to inspect raw token types.
 */
const Token& TokenStream::peekAt(size_t offset) const {
    // Raw access – caller must handle comments manually if needed.
    // This is used only in lookahead that already uses skipCommentsFrom.
    if (offset >= tokens_.size()) return eofToken;
    return tokens_[offset];
}

bool isPrimitiveTypeToken(TokenType type) {
    switch (type) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
        case TokenType::TYPE_ANY:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// Parser construction
// ============================================================================

/**
 * @brief Constructs the parser with tokens and compilation resources.
 * 
 * @param tokens The token vector from the lexer (moved into TokenStream)
 * @param filePath Interned source file path (for error messages)
 * @param pool String pool for interning identifiers
 * @param arena Arena allocator for AST nodes
 * 
 * Initializes the TokenStream and stores references to resources needed
 * throughout parsing (pool for interning, arena for allocation).
 */
Parser::Parser(std::vector<Token> tokens, InternedString filePath,
               StringPool& pool, ASTArena& arena)
    : ts_(std::move(tokens), filePath),
      filePath_(std::move(filePath)),
      pool_(pool), arena_(arena) {}

// ============================================================================
// Error handling
// ============================================================================

/**
 * @brief Reports a syntax error at the given location.
 * 
 * @param loc Source location where the error occurred
 * @param code Diagnostic code (e.g., E2001 for "expected token")
 * @param args List of string arguments for error message formatting
 * 
 * Forwards to the global diagnostic system. The error is recorded but
 * does NOT stop parsing; the caller is responsible for recovery.
 * 
 * @note The variadic template overload in the header forwards to this function.
 */
void Parser::error(const SourceLocation& loc, DiagCode code, std::initializer_list<std::string> args) {
    diagnostic::error(DiagnosticCategory::Syntax, filePath_, loc, code, args);
}

/**
 * @brief Reports an error at the current token position.
 * 
 * @param code Diagnostic code
 * @param args List of string arguments for error message formatting
 * 
 * Convenience overload that uses ts_.currentLoc() as the error location.
 * Most parser errors are reported at the current token because that's
 * where the unexpected input was found.
 */
void Parser::errorAt(DiagCode code, std::initializer_list<std::string> args) {
    error(ts_.currentLoc(), code, args);
}

/**
 * @brief Panic-mode error recovery: skip to a statement or declaration boundary.
 * 
 * This is the primary recovery mechanism for unexpected tokens at the
 * top level or in statement contexts. It consumes tokens until it finds
 * a token that can safely start a new declaration or statement.
 * 
 * ## Recovery Tokens (from GRAMMAR.md)
 * 
 * - Declaration starts: AT_SIGN, PACKAGE, USE, PUB, EXPORT, STRUCT, ENUM,
 *   TRAIT, IMPL, TYPE, FROM, LET, CONST
 * - Statement starts: IF, FOR, WHILE, DO, RETURN, BREAK, CONTINUE,
 *   MATCH, SWITCH
 * - Block boundary: RBRACE (to exit the current block)
 * 
 * The stop token itself is consumed to prevent infinite loops where
 * the same error is reported repeatedly.
 * 
 * @see synchronizeTo() for the implementation
 */
void Parser::synchronize() {
    LOG_PARSER_VERBOSE("Parser::synchronize: entering panic mode recovery");
    synchronizeTo({
        TokenType::AT_SIGN, TokenType::PACKAGE, TokenType::USE,
        TokenType::PUB, TokenType::EXPORT, TokenType::STRUCT,
        TokenType::ENUM, TokenType::TRAIT, TokenType::IMPL,
        TokenType::TYPE, TokenType::FROM, TokenType::LET,
        TokenType::CONST, TokenType::IF, TokenType::FOR,
        TokenType::WHILE, TokenType::DO, TokenType::RETURN,
        TokenType::BREAK, TokenType::CONTINUE, TokenType::MATCH,
        TokenType::SWITCH, TokenType::RBRACE
    });
}

/**
 * @brief Skips tokens until a stop token is found, then consumes it.
 * 
 * @param stopTokens List of token types that are considered safe recovery points
 * 
 * ## Algorithm
 * 
 * 1. If already at a stop token: consume it and return (fast path)
 * 2. Otherwise, scan forward, consuming tokens one by one
 * 3. When a stop token is found: consume it and return
 * 4. If EOF is reached before finding a stop token: return silently
 * 
 * ## Safety
 * 
 * Includes a hard limit of 10,000 skipped tokens to prevent infinite loops
 * in pathological cases (e.g., file with no stop tokens and millions of tokens).
 * 
 * @note The stop token IS consumed. This is intentional: after recovery,
 *       the parser should start parsing the next construct at the token AFTER
 *       the recovery point, not AT the recovery point (which would cause
 *       re-parsing the same token that triggered recovery).
 * 
 * @see synchronize() for the default stop token set
 */
void Parser::synchronizeTo(std::initializer_list<TokenType> stopTokens) {
    int skipped = 0;
    
    // Don't skip if we're already at a stop token - just consume it
    if (ts_.checkAny(stopTokens)) {
        LOG_PARSER_VERBOSE("Parser::synchronizeTo: already at stop token, consuming: " 
                               << LucDebug::tokenTypeToString(ts_.peekType()));
        ts_.advance();
        return;
    }
    
    while (!ts_.isAtEnd()) {
        if (ts_.checkAny(stopTokens)) {
            // Consume the stop token to make progress
            LOG_PARSER_VERBOSE("Parser::synchronizeTo: found stop token, consuming: " 
                                   << LucDebug::tokenTypeToString(ts_.peekType()));
            ts_.advance();
            if (skipped > 0) {
                LOG_PARSER_VERBOSE("Parser::synchronizeTo: skipped " << skipped << " tokens");
            }
            return;
        }
        ts_.advance();
        skipped++;
        
        // Safety: prevent infinite loops in synchronizeTo itself
        if (skipped > 10000) {
            LOG_PARSER("Parser::synchronizeTo: ERROR - skipped too many tokens (" 
                           << skipped << "), aborting recovery");
            break;
        }
    }
    
    if (skipped > 0) {
        LOG_PARSER_VERBOSE("Parser::synchronizeTo: reached EOF, skipped " << skipped << " tokens");
    }
}

// ============================================================================
// Visibility
// ============================================================================

/**
 * @brief Parses a visibility modifier (pub, export, or none).
 * 
 * @return Visibility enum value
 * 
 * ## Grammar
 * 
 *   visibility = 'pub' | 'export' | ε
 * 
 * 'pub' means package-private (visible within the same package).
 * 'export' means public (visible to all packages).
 * No keyword means private (visible only within the current file).
 * 
 * This function is called at the start of parsing any declaration that
 * can have a visibility modifier (structs, enums, functions, variables, etc.).
 * 
 * @note The function consumes the visibility token if present.
 *       If neither token is present, returns Visibility::Private and
 *       does NOT consume any token (the stream position is unchanged).
 */
Visibility Parser::parseVisibility() {
    if (ts_.match(TokenType::PUB)) return Visibility::Package;
    if (ts_.match(TokenType::EXPORT)) return Visibility::Export;
    return Visibility::Private;
}

// ============================================================================
// Doc Comment Harvesting
// ============================================================================
// 
// harvestDocComment() scans backward from the current position to collect
// documentation comments attached to the next declaration.
// 
// Attachment priority (from GRAMMAR.md):
//   1. Block doc comment   : /-- ... --/ immediately above declaration
//   2. Stacked line comments: consecutive -- lines above declaration
//   3. Trailing comment    : -- comment on the same line as declaration
// 
// ─── Scanning Logic ────────────────────────────────────────────────────────
//   - Starts from pos-1 and moves backward
//   - Stops at first non-comment token
//   - Collects LINE_COMMENT tokens that are contiguous and end on declLine-1
//   - DOC_COMMENT token immediately above (line difference ≤ 1) becomes block
//   - LINE_COMMENT on the same line becomes trailing
// 
// ─── Priority Resolution ────────────────────────────────────────────────────
//   Block doc > Stacked lines > Trailing comment
// ============================================================================

/**
 * @brief Harvests documentation comments attached to the upcoming declaration.
 * 
 * @return A DocComment if found, std::nullopt otherwise
 * 
 * ## How It Works
 * 
 * The function scans backward from the current parser position (which points
 * to the first token of the declaration, like `struct` or `func`) to find
 * comments that should be attached to that declaration.
 * 
 * ### Scanning Algorithm
 * 
 * ```
 *   Position before call:    pos → token of declaration (e.g., "struct")
 *   Scan backward:          [-- comment] [-- another] [doc block] [previous decl]
 *                              ↑            ↑            ↑
 *                            collected   collected    collected (block takes priority)
 * ```
 * 
 * ### Three Types of Doc Comments
 * 
 * 1. **Block Doc Comment** (`/-- ... --/`)
 *    - A single DOC_COMMENT token on the line immediately above the declaration
 *    - Takes highest priority
 * 
 * 2. **Stacked Line Comments** (consecutive `--` lines)
 *    - Multiple LINE_COMMENT tokens, each on its own line, directly above declaration
 *    - They are combined with newlines between them
 * 
 * 3. **Trailing Comment** (`-- comment` on same line as declaration)
 *    - A LINE_COMMENT on the same line as the declaration's first token
 *    - Lowest priority (only used if no block or stacked comments exist)
 * 
 * ### Important Details
 * 
 * - Only comments immediately adjacent to the declaration are collected
 *   (no gaps of non-comment tokens between comment and declaration)
 * - Comments are NOT consumed from the token stream (they are already skipped
 *   by TokenStream's peek/advance, so they remain in the vector but invisible
 *   to the grammar)
 * - The harvested doc comment is stored in the AST node via attachMetadata()
 * 
 * @warning This function assumes ts_.getPos() points to the FIRST token of the
 *          declaration. If called at the wrong position, comments may be
 *          incorrectly attached or missed entirely.
 * 
 * @see DocCommentForm for the three comment forms
 */
std::optional<DocComment> Parser::harvestDocComment() {
    LOG_PARSER_EXTREME("harvestDocComment: checking for doc comment");
    
    const auto& tokens = ts_.getTokens();
    size_t pos = ts_.getPos();
    
    if (pos == 0) return std::nullopt;
    
    int declLine = ts_.peek().line;
    std::optional<std::string> trailingText;
    std::vector<std::string> stackedLines;
    int stackedTopLine = -1;
    std::optional<std::string> blockText;
    
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];
        
        if (t.type == TokenType::LINE_COMMENT) {
            if (t.line <= 0) continue;
            if (t.line == declLine) {
                if (!trailingText.has_value()) {
                    trailingText = t.value;
                }
                continue;
            }
            if (stackedLines.empty()) {
                if (declLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            } else {
                if (stackedTopLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            }
        }
        
        if (t.type == TokenType::DOC_COMMENT) {
            if (t.line <= 0) continue;
            if (declLine - t.line <= 1) {
                blockText = t.value;
            }
            break;
        }
        
        break;
    }
    
    if (blockText.has_value()) {
        LOG_PARSER_EXTREME("harvestDocComment: found block doc comment");
        return DocComment{pool_.intern(*blockText), DocCommentForm::Block};
    }
    if (!stackedLines.empty()) {
        LOG_PARSER_EXTREME("harvestDocComment: found " << stackedLines.size() << " stacked line comments");
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{pool_.intern(combined), DocCommentForm::Stacked};
    }
    if (trailingText.has_value()) {
        LOG_PARSER_EXTREME("harvestDocComment: found trailing comment");
        return DocComment{pool_.intern(*trailingText), DocCommentForm::Trailing};
    }
    
    return std::nullopt;
}

// ============================================================================
// Top-Level Parsing
// ============================================================================

/**
 * @brief Parses the entire source file into a ProgramAST.
 * 
 * @return ProgramASTPtr (raw pointer to arena-allocated ProgramAST)
 * 
 * ## Parsing Steps
 * 
 * 1. **Create ProgramAST node** – holds file path and list of declarations
 * 2. **Parse package declaration** – mandatory, must be first non-comment token
 * 3. **Parse top-level declarations** – zero or more declarations (structs,
 *    enums, functions, etc.) until EOF
 * 4. **Build ArenaSpan** – convert declaration vector to arena span
 * 
 * ## Package Declaration Handling
 * 
 * The package declaration is REQUIRED in Luc. If missing or invalid:
 * - Reports an error
 * - Sets program->packageName to invalid InternedString (0)
 * - Does NOT add a dummy PackageDeclAST to decls list
 * - Calls synchronize() to skip to the next valid declaration
 * - Continues parsing (allows recovery from missing package)
 * 
 * ## Invalid Package Name Semantics
 * 
 * An invalid InternedString (value == 0) serves as a sentinel indicating
 * that no valid package declaration was parsed. This is preferable to using
 * sentinel strings like "<error>" because:
 *   - Invalid (0) is a natural "absent" value for InternedString
 *   - Doesn't pollute the string pool with artificial identifiers
 *   - Semantic analysis can check `program->packageName.isValid()`
 * 
 * ## Infinite Loop Protection
 * 
 * The parsing loop uses three safety mechanisms:
 * 
 * ### 1. Progress Detection
 * ```cpp
 * if (ts_.getPos() == savedPos) {
 *     // No progress - force consume a token
 *     ts_.advance();
 *     consecutiveFailures++;
 * }
 * ```
 * 
 * ### 2. Consecutive Failure Counter
 * - Tracks how many times in a row a declaration failed to make progress
 * - After 5 failures: calls synchronize() for aggressive recovery
 * - After 100 failures: aborts parsing entirely
 * 
 * ### 3. Stuck Token Detection
 * - If position hasn't changed AND consecutiveFailures > 10: forces advance
 * - Prevents infinite loops where synchronize() fails to consume tokens
 * 
 * ## Error Recovery Philosophy
 * 
 * parse() NEVER returns nullptr. Even on catastrophic errors, it returns
 * a ProgramAST with whatever was successfully parsed. Errors are communicated
 * via the diagnostic system (diagnostic::hasErrors()).
 * 
 * This design allows IDEs and batch compilers to get partial ASTs for:
 * - Syntax highlighting in the presence of errors
 * - Code completion after an error
 * - Continuing to parse the rest of the file
 * 
 * ## Return Value Guarantees
 * 
 * - Always returns a valid ProgramAST* (arena-allocated, never freed)
 * - program->filePath is always set
 * - program->packageName is always set (to valid name or invalid InternedString)
 * - program->decls is always a valid ArenaSpan (possibly empty)
 * 
 * @note After parsing, call hasErrors() to check if any errors were reported.
 */
ProgramASTPtr Parser::parse() {
    LOG_PARSER("Parser::parse: starting parsing of file " 
                   << std::string(pool_.lookup(filePath_)));
    
    auto* program = arena_.make<ProgramAST>();
    program->filePath = filePath_;
    program->loc = ts_.currentLoc();
    program->packageName = InternedString();  // Initialize to invalid (0)

    std::vector<DeclAST*> decls;

    // ========================================================================
    // Package declaration (mandatory)
    // ========================================================================
    auto* pkg = parsePackageDecl();
    if (pkg) {
        program->packageName = pkg->name;
        decls.push_back(pkg);
        LOG_PARSER_VERBOSE("Parser::parse: package name = " 
                               << std::string(pool_.lookup(program->packageName)));
    } else {
        // parsePackageDecl failed (error already reported)
        LOG_PARSER("Parser::parse: ERROR - failed to parse package declaration");
        synchronize();  // Skip to next safe point
        // packageName remains invalid
    }

    // ========================================================================
    // Top-level declarations with infinite loop protection
    // ========================================================================
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = ts_.getPos();
    
    while (!ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment();
        size_t savedPos = ts_.getPos();
        
        DeclAST* decl = parseTopLevelDecl();
        
        // Check for progress
        if (ts_.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("Parser::parse: NO PROGRESS - stuck on token '" 
                           << ts_.peek().value << "' (type=" 
                           << LucDebug::tokenTypeToString(ts_.peekType())
                           << "), consecutive failures: " << consecutiveFailures);
            
            // Force consumption of stuck token
            if (!ts_.isAtEnd()) {
                LOG_PARSER("Parser::parse: forcing consumption of stuck token");
                ts_.advance();
            }
            
            // Aggressive recovery after 5 failures
            if (consecutiveFailures > 5) {
                LOG_PARSER("Parser::parse: too many consecutive failures, aggressive recovery");
                synchronize();
            }
        } else if (decl) {
            // Successfully parsed a declaration
            declCount++;
            consecutiveFailures = 0;
            lastPos = ts_.getPos();
            
            LOG_PARSER_EXTREME("Parser::parse: parsed declaration #" << declCount 
                                   << " (" << LucDebug::kindToString(decl->kind) << ")");
            
            // Attach documentation if available
            if (doc) {
                decl->doc = std::move(doc);
            }
            decls.push_back(decl);
        } else {
            // Made progress but parseTopLevelDecl returned nullptr
            consecutiveFailures = 0;
            LOG_PARSER("Parser::parse: parseTopLevelDecl returned nullptr but made progress, continuing");
        }
        
        // Critical stuck detection
        if (ts_.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("Parser::parse: CRITICAL - still no progress after " 
                           << consecutiveFailures << " attempts, forcing advance");
            if (!ts_.isAtEnd()) {
                ts_.advance();
            }
            lastPos = ts_.getPos();
        }
    }

    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        LOG_PARSER("Parser::parse: ERROR - too many consecutive failures (" 
                       << MAX_CONSECUTIVE_FAILURES << "), aborting parsing");
        errorAt(DiagCode::E2001, "Too many parsing errors, compilation aborted");
    }

    LOG_PARSER("Parser::parse: parsed " << declCount << " top-level declarations");

    // Build the ArenaSpan for program->decls
    auto builder = arena_.makeBuilder<DeclAST*>();
    for (auto* d : decls) builder.push_back(d);
    program->decls = builder.build();

    LOG_PARSER("Parser::parse: parsing complete");
    return program;
}