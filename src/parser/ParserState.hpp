/**
 * @file ParserState.hpp
 * @brief Parser state for Lucid language parser with simplified error reporting.
 * 
 * This file defines the core state management for the Lucid parser, including:
 * - TokenStream: Safe token consumption with automatic comment skipping
 * - ParserState: Mutable parsing context with variadic error reporting
 * 
 * The error reporting system uses C++11 template metaprogramming to provide
 * a clean, type-safe API for reporting errors with automatic string formatting
 * and special handling for InternedString and SourceLocation types.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "parser/ModuleResolver.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <initializer_list>
#include <functional>
#include <sstream>
#include <type_traits>

namespace parser {

// ─────────────────────────────────────────────────────────────────────────────
// TokenStream – Safe token stream abstraction
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Wraps a vector of tokens with safe accessors and automatic comment skipping.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * TokenStream stream(tokens, filePath);
 * if (stream.check(TokenType::IDENTIFIER)) {
 *     Token tok = stream.advance();
 *     // use tok
 * }
 * stream.consume(TokenType::LBRACE);
 * ```
 * 
 * Comments (LINE_COMMENT, DOC_COMMENT, BLOCK_COMMENT) are transparently skipped
 * by all peek/advance methods. They are harvested separately via harvestDocComment().
 */
struct TokenStream {
    TokenStream() = default;
    TokenStream(std::vector<Token> tokens, InternedString filePath);
    
    const Token& peek() const;
    Token advance();
    bool check(TokenType type) const;
    bool checkAny(std::initializer_list<TokenType> types) const;
    bool match(TokenType type);
    Token consume(TokenType type);
    bool isAtEnd() const;
    SourceLocation currentLoc() const;
    InternedString getFilePath() const { return filePath_; }
    
    TokenType peekType() const { return peek().type; }
    TokenType peekNextType() const;
    const Token& peekNext() const;
    const Token& peekAt(size_t offset) const;
    bool isPrimitiveTypeToken(TokenType type) const;
    
    size_t getPos() const { return pos_; }
    void setPos(size_t pos) { pos_ = pos; }
    const std::vector<Token>& getTokens() const { return tokens_; }
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }
    size_t getTokenCount() const { return tokens_.size(); }
    size_t skipCommentsFrom(size_t start) const;
    SourceLocation locOf(const Token& tok) const;
    
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    InternedString filePath_;
    static const Token eofToken_;
};

// ─────────────────────────────────────────────────────────────────────────────
// ParserState – Mutable context
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Mutable state for a single file.
 * 
 * ParserState is the central context object passed to all parsing functions.
 * It holds the token stream, error tracking, context state, and module
 * import facilities.
 * 
 * ## Template-Based Error Reporting
 * 
 * The error reporting system uses C++11 template metaprogramming to provide
 * a clean, type-safe API with the following features:
 * 
 * ### 1. Variadic Arguments
 * Any number of arguments can be passed, and they are automatically
 * converted to a string using `operator<<`:
 * 
 * ```cpp
 * state.error("Expected '", expected, "' but found '", actual, "'");
 * ```
 * 
 * ### 2. Automatic SourceLocation Resolution
 * All errors automatically use the current token location from the stream.
 * No need to manually pass locations:
 * 
 * ```cpp
 * // Uses stream.currentLoc() automatically
 * state.error("Unexpected token");
 * ```
 * 
 * ### 3. InternedString Support
 * InternedString values are automatically resolved to their string values
 * using the StringPool. This is handled by the template specialization:
 * 
 * ```cpp
 * // usePath is InternedString - automatically resolved
 * state.error("Module '", usePath, "' not found");
 * ```
 * 
 * ### 4. Optional Diagnostic Code
 * A DiagCode can be passed as the first argument:
 * 
 * ```cpp
 * state.error(DiagCode::E0001, "Missing '", "}", "' after '", "if", "'");
 * ```
 * 
 * ## Template Implementation Details
 * 
 * The error reporting uses several template techniques:
 * 
 * ### `is_interned_string<T>` Trait
 * Detects if a type is InternedString or a reference to it:
 * 
 * ```cpp
 * template<typename T>
 * struct is_interned_string : std::false_type {};
 * 
 * template<>
 * struct is_interned_string<InternedString> : std::true_type {};
 * 
 * template<>
 * struct is_interned_string<InternedString&> : std::true_type {};
 * ```
 * 
 * ### SFINAE with `std::enable_if`
 * The generic `streamTo` overload is only enabled for types that are NOT
 * InternedString. This prevents ambiguity:
 * 
 * ```cpp
 * template<typename T>
 * typename std::enable_if<!is_interned_string<T>::value>::type
 * streamTo(std::ostringstream& oss, T&& value) const {
 *     oss << std::forward<T>(value);
 * }
 * ```
 * 
 * ### Specialized Overload for InternedString
 * The specialized overload uses the StringPool to resolve the string:
 * 
 * ```cpp
 * void streamTo(std::ostringstream& oss, InternedString s) const {
 *     oss << pool.lookup(s);
 * }
 * ```
 * 
 * ### `is_diag_code<T>` Trait
 * Detects if the first argument is a DiagCode:
 * 
 * ```cpp
 * template<typename T>
 * struct is_diag_code : std::false_type {};
 * 
 * template<>
 * struct is_diag_code<DiagCode> : std::true_type {};
 * ```
 * 
 * ### `if constexpr` (C++17)
 * Compile-time branching to check if the first argument is a DiagCode:
 * 
 * ```cpp
 * if constexpr (sizeof...(Args) > 0) {
 *     using FirstType = typename std::decay<...>::type;
 *     if constexpr (std::is_same<FirstType, DiagCode>::value) {
 *         // First arg is DiagCode
 *     }
 * }
 * ```
 * 
 * ## Thread Safety
 * 
 * Not thread-safe. Designed for single-threaded parsing.
 * 
 * ## Memory Ownership
 * 
 * - `stream`: Owns the token vector (moved in)
 * - `pool`: Reference to shared StringPool
 * - `arena`: Reference to shared ASTArena
 * - `errors`: Owns diagnostic messages for this file
 * 
 * ## Usage Example
 * 
 * ```cpp
 * auto tokens = lexer::tokenize(source, path);
 * TokenStream stream(std::move(tokens), pool.intern(path));
 * ParserState state(std::move(stream), pool.intern(path), pool, arena);
 * 
 * // Error reporting with automatic location
 * state.error("Unexpected token");
 * 
 * // With multiple parts
 * state.error("Expected '", "}", "' but found '", "(", "'");
 * 
 * // With InternedString (auto-resolved)
 * state.error("Module '", usePath, "' not found");
 * 
 * // With diagnostic code
 * state.error(DiagCode::E0001, "Missing '", "}", "' after '", "if", "'");
 * ```
 */
struct ParserState {
    // ─────────────────────────────────────────────────────────────────────────
    // Core State
    // ─────────────────────────────────────────────────────────────────────────

    /// The token stream being consumed (mutable)
    TokenStream stream;
    
    /// Source file path (for error reporting and module identity)
    InternedString filePath;
    
    /// String interner (shared across all files) - resolves InternedString to strings
    StringPool& pool;
    
    /// AST allocator (shared across all files)
    ASTArena& arena;

    // ─────────────────────────────────────────────────────────────────────────
    // Module Support
    // ─────────────────────────────────────────────────────────────────────────
    
    /// Module resolver for importing modules (optional, may be nullptr)
    ModuleResolver* moduleResolver = nullptr;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Error Tracking
    // ─────────────────────────────────────────────────────────────────────────
    
    /// True if any error has been reported during parsing
    bool hasErrors = false;
    
    /// Collected diagnostic messages for this file
    std::vector<Diagnostic> errors;
    
    /// Consecutive error count (used to prevent infinite loops in lists)
    int consecutiveErrors = 0;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Context Tracking
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Depth of spawn/join nesting (OS thread parallelism).
     * 
     * Tracks how deeply we're nested in spawn/join operations.
     * Used to enforce thread-safety rules.
     * 
     * Grammar reference: spawn/join (Parallelism, OS threads)
     */
    int spawnDepth = 0;
    
    /**
     * @brief True if currently parsing inside an async context.
     * 
     * Tracks that we're in a function that can use await.
     * Used to validate async/await pairing.
     * 
     * Grammar reference: async/await (Concurrency, event loop)
     */
    bool inAsyncContext = false;
    
    /// Current declaration context
    enum class Context {
        TopLevel,   // File-level declarations
        Local,      // Inside a block (function body, etc.)
        Function,   // Inside a function body (return allowed)
        Loop,       // Inside a loop body (break/continue allowed)
        Spawn,      // Inside a spawned thread (spawn restrictions)
        Async,      // Inside an async operation (await allowed)
    };
    Context context = Context::TopLevel;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Doc Comment Harvesting
    // ─────────────────────────────────────────────────────────────────────────
    
    /// Last harvested doc comment (stored between harvest and attachment)
    std::optional<DocComment> pendingDoc;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Create a new parser state for a single file.
     * 
     * @param s Token stream (ownership taken)
     * @param path File path (interned)
     * @param p String pool reference
     * @param a AST arena reference
     */
    ParserState(TokenStream&& s, 
                InternedString path, 
                StringPool& p, 
                ASTArena& a)
        : stream(std::move(s))
        , filePath(path)
        , pool(p)
        , arena(a)
    {}
    
    // ─────────────────────────────────────────────────────────────────────────
    // String Conversion Helper
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Convert InternedString to std::string using the pool.
     * 
     * @param s The InternedString to convert
     * @return std::string The resolved string value
     * 
     * This is used internally by the error reporting system, but is also
     * exposed for manual conversion when needed.
     */
    std::string toString(InternedString s) const {
        return std::string(pool.lookup(s));
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Internal: Build message with pool-aware streaming
    // ─────────────────────────────────────────────────────────────────────────
    
private:
    /**
     * @brief Trait to detect if a type is InternedString (or a reference to it).
     * 
     * This is used with SFINAE to prevent the generic streamTo overload
     * from matching InternedString, which would cause ambiguity with the
     * specialized overload.
     * 
     * @tparam T The type to check
     * 
     * ## How It Works
     * 
     * The primary template inherits from std::false_type.
     * Specializations for InternedString and its reference types
     * inherit from std::true_type.
     * 
     * ```cpp
     * is_interned_string<int>::value        // false
     * is_interned_string<InternedString>::value  // true
     * is_interned_string<InternedString&>::value // true
     * ```
     */
    template<typename T>
    struct is_interned_string : std::false_type {};
    
    template<>
    struct is_interned_string<InternedString> : std::true_type {};
    
    template<>
    struct is_interned_string<InternedString&> : std::true_type {};
    
    template<>
    struct is_interned_string<const InternedString&> : std::true_type {};
    
    /**
     * @brief Stream any value that is NOT InternedString.
     * 
     * This is the generic overload for all types except InternedString.
     * It uses SFINAE with std::enable_if to only be considered when the
     * type is not InternedString (or a reference to it).
     * 
     * @tparam T The type of the value (deduced)
     * @param oss The output stream
     * @param value The value to stream
     * 
     * ## SFINAE Explanation
     * 
     * `typename std::enable_if<!is_interned_string<T>::value>::type`
     * means: "This function only exists if T is NOT InternedString".
     * If T is InternedString, this overload is removed from consideration,
     * leaving only the specialized overload.
     * 
     * This prevents ambiguity between the generic overload and the
     * specialized InternedString overload.
     */
    template<typename T>
    typename std::enable_if<!is_interned_string<typename std::decay<T>::type>::value>::type
    streamTo(std::ostringstream& oss, T&& value) const {
        oss << std::forward<T>(value);
    }
    
    /**
     * @brief Stream InternedString using the StringPool.
     * 
     * This is the specialized overload for InternedString.
     * It resolves the InternedString to its actual string value using
     * the StringPool, making error messages readable.
     * 
     * @param oss The output stream
     * @param s The InternedString to resolve and stream
     */
    void streamTo(std::ostringstream& oss, InternedString s) const {
        oss << pool.lookup(s);
    }
    
    /**
     * @brief Build a message from a single argument.
     * 
     * @tparam T The type of the argument
     * @param oss The output stream
     * @param value The value to stream
     */
    template<typename T>
    void buildMessageImpl(std::ostringstream& oss, T&& value) const {
        streamTo(oss, std::forward<T>(value));
    }
    
    /**
     * @brief Build a message from multiple arguments (recursive).
     * 
     * This recursively streams all arguments into the output stream.
     * The recursion ends when only one argument remains.
     * 
     * @tparam T The type of the first argument
     * @tparam Rest The types of the remaining arguments
     * @param oss The output stream
     * @param first The first argument to stream
     * @param rest The remaining arguments to stream
     */
    template<typename T, typename... Rest>
    void buildMessageImpl(std::ostringstream& oss, T&& first, Rest&&... rest) const {
        streamTo(oss, std::forward<T>(first));
        buildMessageImpl(oss, std::forward<Rest>(rest)...);
    }
    
    /**
     * @brief Build a formatted message from variadic arguments.
     * 
     * This is the public-facing entry point for building messages.
     * It creates an ostringstream and streams all arguments into it.
     * 
     * @tparam Args The types of the arguments
     * @param args The arguments to format
     * @return std::string The formatted message
     * 
     * ## Example
     * 
     * ```cpp
     * std::string msg = buildMessage("Expected '", "}", "' but found '", "(", "'");
     * // msg = "Expected '}' but found '('"
     * ```
     */
    template<typename... Args>
    std::string buildMessage(Args&&... args) const {
        std::ostringstream oss;
        buildMessageImpl(oss, std::forward<Args>(args)...);
        return oss.str();
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Internal: Add a diagnostic
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Internal method to add a diagnostic to the error list.
     * 
     * This is the low-level method that actually pushes a Diagnostic
     * into the errors vector and updates error/warning counts.
     * 
     * @param severity The severity level (Error, Warning, Note, etc.)
     * @param category The diagnostic category (Syntax, Module, etc.)
     * @param loc The source location
     * @param code The diagnostic code
     * @param message The formatted error message
     */
    void addDiagnostic(DiagnosticSeverity severity, 
                       DiagnosticCategory category,
                       const SourceLocation& loc,
                       DiagCode code,
                       const std::string& message) {
        errors.push_back({
            severity,
            category,
            filePath,
            loc,
            code,
            {message}
        });
        if (severity == DiagnosticSeverity::Error || 
            severity == DiagnosticSeverity::Fatal) {
            hasErrors = true;
            consecutiveErrors++;
        } else if (severity == DiagnosticSeverity::Warning) {
            consecutiveErrors++;
        }
    }
    
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Public Error Reporting API
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Report an error at the current token location.
     * 
     * This is the primary error reporting function. It automatically:
     * 1. Uses the current token location (stream.currentLoc())
     * 2. Formats the message from variadic arguments
     * 3. Handles InternedString resolution
     * 4. Accepts an optional DiagCode as the first argument
     * 
     * @tparam Args The types of the arguments
     * @param args The message parts or (DiagCode, message parts...)
     * 
     * ## Usage Examples
     * 
     * ```cpp
     * // Simple error with no formatting
     * state.error("Unexpected token");
     * 
     * // With multiple string parts
     * state.error("Expected '", "}", "' but found '", "(", "'");
     * 
     * // With diagnostic code as first argument
     * state.error(DiagCode::E0001, "Missing '", "}", "' after '", "if", "'");
     * 
     * // With InternedString (automatically resolved)
     * state.error("Module '", usePath, "' not found");
     * 
     * // Mixed types
     * state.error("Expected ", 42, " but got ", 3.14);
     * ```
     * 
     * ## Type Resolution
     * 
     * - `const char*` and `std::string`: Streamed directly
     * - `InternedString`: Resolved via StringPool
     * - `SourceLocation`: Uses operator<< (prints line:column)
     * - `int`, `float`, etc.: Streamed as-is
     * - `DiagCode` (first arg only): Used as diagnostic code
     */
    template<typename... Args>
    void error(Args&&... args) {
        errorImpl(DiagCode::E0001, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Report a warning at the current token location.
     * 
     * Same as error(), but with Warning severity.
     * Warnings do not set hasErrors = true.
     * 
     * @tparam Args The types of the arguments
     * @param args The message parts or (DiagCode, message parts...)
     * 
     * @see error() for usage examples
     */
    template<typename... Args>
    void warning(Args&&... args) {
        warningImpl(DiagCode::W0001, std::forward<Args>(args)...);
    }
    
    /**
     * @brief Report a note at the current token location.
     * 
     * Notes are informational and do not affect error/warning counts.
     * Notes do NOT accept a DiagCode as the first argument.
     * 
     * @tparam Args The types of the arguments
     * @param args The message parts
     * 
     * ## Usage Examples
     * 
     * ```cpp
     * state.note("Consider using '", alternative, "' instead");
     * ```
     */
    template<typename... Args>
    void note(Args&&... args) {
        noteImpl(std::forward<Args>(args)...);
    }
    
private:
    // ─────────────────────────────────────────────────────────────────────────
    // Implementation Details for Error/Warning/Note
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Trait to detect if a type is DiagCode.
     * 
     * Used with if constexpr to check if the first argument is a DiagCode.
     */
    template<typename T>
    struct is_diag_code : std::false_type {};
    
    template<>
    struct is_diag_code<DiagCode> : std::true_type {};
    
    /**
     * @brief Implementation of error() with optional DiagCode detection.
     * 
     * This function uses compile-time branching to detect if the first
     * argument is a DiagCode. If it is, the DiagCode is extracted and
     * the remaining arguments are used for the message.
     * 
     * @tparam Args The types of the arguments
     * @param defaultCode The default DiagCode to use if no DiagCode is provided
     * @param args The arguments (DiagCode optional + message parts)
     * 
     * ## How It Works
     * 
     * 1. Check if there are any arguments
     * 2. If so, check if the first argument is a DiagCode
     * 3. If yes: extract the DiagCode, build message from remaining args
     * 4. If no: use the default DiagCode, build message from all args
     */
    template<typename... Args>
    void errorImpl(DiagCode defaultCode, Args&&... args) {
        if constexpr (sizeof...(Args) > 0) {
            using FirstType = typename std::decay<decltype(std::get<0>(std::forward_as_tuple(args...)))>::type;
            if constexpr (std::is_same<FirstType, DiagCode>::value) {
                // First arg is DiagCode - extract it
                DiagCode code = std::get<0>(std::forward_as_tuple(args...));
                // Build message from remaining args
                std::string message = buildRemaining(std::forward<Args>(args)...);
                addDiagnostic(DiagnosticSeverity::Error, 
                              DiagnosticCategory::Syntax,
                              stream.currentLoc(),
                              code,
                              message);
                return;
            }
        }
        // No DiagCode - use default
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Error, 
                      DiagnosticCategory::Syntax,
                      stream.currentLoc(),
                      defaultCode,
                      message);
    }
    
    /**
     * @brief Helper to build message from args, skipping the first argument.
     * 
     * This is used when the first argument is a DiagCode. It forwards
     * all arguments except the first to buildMessage().
     * 
     * @tparam First The type of the first argument (DiagCode)
     * @tparam Rest The types of the remaining arguments
     * @param first The first argument (DiagCode, skipped)
     * @param rest The remaining arguments (message parts)
     * @return std::string The formatted message
     */
    template<typename First, typename... Rest>
    std::string buildRemaining(First&& first, Rest&&... rest) {
        // Skip the first argument (DiagCode) and build from rest
        return buildMessage(std::forward<Rest>(rest)...);
    }
    
    /**
     * @brief Implementation of warning() with optional DiagCode detection.
     * 
     * Same logic as errorImpl(), but with Warning severity.
     * 
     * @see errorImpl() for detailed explanation
     */
    template<typename... Args>
    void warningImpl(DiagCode defaultCode, Args&&... args) {
        if constexpr (sizeof...(Args) > 0) {
            using FirstType = typename std::decay<decltype(std::get<0>(std::forward_as_tuple(args...)))>::type;
            if constexpr (std::is_same<FirstType, DiagCode>::value) {
                DiagCode code = std::get<0>(std::forward_as_tuple(args...));
                std::string message = buildRemaining(std::forward<Args>(args)...);
                addDiagnostic(DiagnosticSeverity::Warning, 
                              DiagnosticCategory::Syntax,
                              stream.currentLoc(),
                              code,
                              message);
                return;
            }
        }
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Warning, 
                      DiagnosticCategory::Syntax,
                      stream.currentLoc(),
                      defaultCode,
                      message);
    }
    
    /**
     * @brief Implementation of note() (no DiagCode support).
     * 
     * Notes are informational and do not accept a DiagCode.
     * All arguments are used for the message.
     * 
     * @tparam Args The types of the arguments
     * @param args The message parts
     */
    template<typename... Args>
    void noteImpl(Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Note, 
                      DiagnosticCategory::General,
                      stream.currentLoc(),
                      DiagCode::E0001,
                      message);
    }
    
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Context Queries
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if we can safely continue parsing.
     * 
     * @return true if we've had fewer than 10 consecutive errors
     * 
     * This is used to prevent infinite loops when the parser is stuck
     * in an error state. After 10 consecutive errors, parsing should
     * be aborted.
     */
    bool canContinue() const;
    
    /**
     * @brief Check if we're in a spawn context (parallelism).
     * 
     * @return true if spawnDepth > 0
     * 
     * Used to enforce thread-safety rules and restrict certain operations
     * inside spawned threads.
     */
    bool isSpawnContext() const { return spawnDepth > 0; }
    
    /**
     * @brief Check if we're in an async context (concurrency).
     * 
     * @return true if inAsyncContext is true
     * 
     * Used to validate that await is only used inside async contexts.
     */
    bool isAsyncContext() const { return inAsyncContext; }
    
    /**
     * @brief Get the current token location.
     * 
     * @return SourceLocation The location of the current token
     * 
     * This is automatically used by all error reporting functions.
     */
    SourceLocation currentLoc() const;

};

} // namespace parser