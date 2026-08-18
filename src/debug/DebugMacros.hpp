/**
 * @file debugMacros.hpp
 * @brief Developer debug logging macros - Modern C++ streaming version.
 * 
 * These macros are for internal development tracing ONLY.
 * User-facing diagnostics are in Diagnostic.hpp.
 * 
 * ## Usage
 * 
 *   LOG_MINIMAL("Parser", "Starting parse");           // Always shown (level 0)
 *   LOG("Parser", "Parsing function: ", name);         // Normal detail (level 1)
 *   LOG_DETAIL("Parser", "Token: ", tok.value);        // Verbose detail (level 2)
 * 
 *   // Any type with operator<< works:
 *   LOG_PARSER("Module '", importPath, "' not found");
 *   LOG_PARSER("Expected ", expected, " but got ", actual);
 * 
 * ## Build Configuration
 * 
 *   -Ddebug_MASTER       → Enable all debug output (overrides all)
 *   -Ddebug_PARSER       → Enable PARSER component only
 *   -Ddebug_VERBOSITY=2  → Set detail level (0=minimal, 1=normal, 2=detail)
 *   -Ddebug_TO_FILE      → Write to debug.log instead of stdout
 * 
 * ## Verbosity Levels
 * 
 *   Level 0 (MINIMAL) : Major events (start/end parse, errors)
 *   Level 1 (NORMAL)  : Important steps (function entry/exit, declarations)
 *   Level 2 (DETAIL)  : Detailed trace (token stream, AST construction)
 */

#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <utility>

namespace debug {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration (set via build flags)
// ─────────────────────────────────────────────────────────────────────────────

/** @brief Check if debug is enabled for a component */
inline bool isEnabled(const char* component) {
    #ifdef debug_MASTER
        return true;
    #else
        #ifdef debug_PARSER
            if (std::string(component) == "PARSER") return true;
        #endif
        #ifdef debug_LEXER
            if (std::string(component) == "LEXER") return true;
        #endif
        #ifdef debug_SEMANTIC
            if (std::string(component) == "SEMANTIC") return true;
        #endif
        #ifdef debug_CODEGEN
            if (std::string(component) == "CODEGEN") return true;
        #endif
        #ifdef debug_TYPE
            if (std::string(component) == "TYPE") return true;
        #endif
        #ifdef debug_INTERPRETER
            if (std::string(component) == "INTERPRETER") return true;
        #endif
        return false;
    #endif
}

/** @brief Get the current verbosity level (0=minimal, 1=normal, 2=detail) */
inline int verbosity() {
    #ifdef debug_VERBOSITY
        return debug_VERBOSITY;
    #else
        return 1;  // Normal by default
    #endif
}

/** @brief Get the log file path (default: debug.log) */
inline std::string logPath() {
    #ifdef debug_FILE_PATH
        return debug_FILE_PATH;
    #else
        return "debug.log";
    #endif
}

/** @brief Get the output stream (stdout or file) */
inline std::ostream& stream() {
    #ifdef debug_TO_FILE
        static std::ofstream file(logPath(), std::ios::out | std::ios::trunc);
        static bool first = true;
        if (first) {
            first = false;
            std::cout << "[debug] Logging to: " << logPath() << std::endl;
        }
        return file;
    #else
        return std::cout;
    #endif
}

/** @brief Current timestamp for log entries */
inline std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count() % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms;
    return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Modern C++ Streaming Logger
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Build a message from variadic arguments using stream syntax.
 * 
 * This is the modern C++ replacement for printf-style formatting.
 * It handles any type that has an operator<< defined.
 * 
 * @tparam Args The types of the arguments
 * @param args The arguments to format
 * @return std::string The formatted message
 * 
 * ## How It Works
 * 
 * The function uses a parameter pack expansion with a dummy array
 * to evaluate each argument in order, streaming them into the
 * ostringstream. The dummy array trick ensures left-to-right
 * evaluation order (guaranteed in C++11 for braced initializers).
 */
template<typename... Args>
std::string buildMessage(Args&&... args) {
    std::ostringstream oss;
    // Use a dummy array to expand the parameter pack
    // The comma operator ensures each argument is streamed in order
    int dummy[] = {0, (oss << std::forward<Args>(args), 0)...};
    (void)dummy;  // Suppress unused variable warning
    return oss.str();
}

/**
 * @brief Log a message with the given component and level.
 * 
 * @param component The subsystem name (PARSER, LEXER, etc.)
 * @param level The verbosity level required (0, 1, 2)
 * @param message The message to log
 */
inline void logMessage(const char* component, int level, const std::string& message) {
    if (isEnabled(component) && verbosity() >= level) {
        stream() << "[" << timestamp() << "] "
                 << "[" << component << "] "
                 << message << std::endl;
    }
}

} // namespace debug

// ─────────────────────────────────────────────────────────────────────────────
// Core Logging Macros (Modern C++ Version)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Internal macro that does the actual logging with streaming.
 * 
 * This macro uses the buildMessage template to format the log message
 * with full type safety.
 * 
 * @param COMPONENT  The subsystem name (PARSER, LEXER, etc.)
 * @param LEVEL      The verbosity level required (0, 1, 2)
 * @param ...        The message parts (any number of arguments)
 * 
 * ## Performance Note
 * 
 * The macro checks isEnabled() and verbosity() before building the
 * message, so if logging is disabled, the buildMessage function
 * is never called. This means zero overhead when debug is off.
 */
#define LOG_CORE(COMPONENT, LEVEL, ...) \
    do { \
        if (debug::isEnabled(COMPONENT) && debug::verbosity() >= (LEVEL)) { \
            debug::stream() << "[" << debug::timestamp() << "] " \
                            << "[" << COMPONENT << "] " \
                            << debug::buildMessage(__VA_ARGS__) << std::endl; \
        } \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Public Logging Macros (3 levels)
// ─────────────────────────────────────────────────────────────────────────────

// ─── Level 0: Minimal (major events only) ───────────────────────────────────

#define LOG_MINIMAL(COMPONENT, ...) \
    LOG_CORE(COMPONENT, 0, __VA_ARGS__)

// ─── Level 1: Normal (important steps) ─────────────────────────────────────

#define LOG(COMPONENT, ...) \
    LOG_CORE(COMPONENT, 1, __VA_ARGS__)

// ─── Level 2: Detail (verbose trace) ──────────────────────────────────────

#define LOG_DETAIL(COMPONENT, ...) \
    LOG_CORE(COMPONENT, 2, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Component-Specific Aliases
// ─────────────────────────────────────────────────────────────────────────────

// Parser
#define LOG_PARSER_MINIMAL(...)        LOG_MINIMAL("PARSER", __VA_ARGS__)
#define LOG_PARSER(...)                LOG("PARSER", __VA_ARGS__)
#define LOG_PARSER_DETAIL(...)         LOG_DETAIL("PARSER", __VA_ARGS__)

// Lexer
#define LOG_LEXER_MINIMAL(...)         LOG_MINIMAL("LEXER", __VA_ARGS__)
#define LOG_LEXER(...)                 LOG("LEXER", __VA_ARGS__)
#define LOG_LEXER_DETAIL(...)          LOG_DETAIL("LEXER", __VA_ARGS__)

// Semantic
#define LOG_SEMA_MINIMAL(...)      LOG_MINIMAL("SEMANTIC", __VA_ARGS__)
#define LOG_SEMA(...)              LOG("SEMANTIC", __VA_ARGS__)
#define LOG_SEMA_DETAIL(...)       LOG_DETAIL("SEMANTIC", __VA_ARGS__)

// CodeGen
#define LOG_CODEGEN_MINIMAL(...)       LOG_MINIMAL("CODEGEN", __VA_ARGS__)
#define LOG_CODEGEN(...)               LOG("CODEGEN", __VA_ARGS__)
#define LOG_CODEGEN_DETAIL(...)        LOG_DETAIL("CODEGEN", __VA_ARGS__)

// Type
#define LOG_TYPE_MINIMAL(...)          LOG_MINIMAL("TYPE", __VA_ARGS__)
#define LOG_TYPE(...)                  LOG("TYPE", __VA_ARGS__)
#define LOG_TYPE_DETAIL(...)           LOG_DETAIL("TYPE", __VA_ARGS__)

// Interpreter
#define LOG_INTERPRETER_MINIMAL(...)   LOG_MINIMAL("INTERPRETER", __VA_ARGS__)
#define LOG_INTERPRETER(...)           LOG("INTERPRETER", __VA_ARGS__)
#define LOG_INTERPRETER_DETAIL(...)    LOG_DETAIL("INTERPRETER", __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Usage Examples in Comments
// ─────────────────────────────────────────────────────────────────────────────

/*
 * === Usage Examples ===
 * 
 * // Simple logging with just a string
 * LOG_PARSER("Starting parse of: ", filePath);
 * 
 * // Logging with multiple arguments (any types)
 * LOG_PARSER("Parsed declaration #", declCount, " (", kindName, ")");
 * 
 * // Logging with InternedString (resolved to string)
 * LOG_PARSER("Module '", importPath, "' not found");
 * 
 * // Logging with mixed types
 * LOG_PARSER("Expected ", expected, " but got ", actual);
 * 
 * // Using different verbosity levels
 * LOG_PARSER_MINIMAL("Parsing complete: ", filePath);
 * LOG_PARSER("Parsed ", declCount, " declarations");
 * LOG_PARSER_DETAIL("Token: ", token.value, " at ", token.line, ":", token.column);
 * 
 * // With SourceLocation
 * LOG_PARSER("Error at ", loc.line(), ":", loc.column());
 * 
 * // With any custom type that has operator<<
 * LOG_PARSER("Value: ", myCustomType);
 */