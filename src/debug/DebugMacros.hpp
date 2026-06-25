/**
 * @file DebugMacros.hpp
 * @brief Developer debug logging macros.
 * 
 * These macros are for internal development tracing ONLY.
 * User-facing diagnostics are in Diagnostic.hpp.
 * 
 * ## Usage
 * 
 *   LOG_MINIMAL("Parser", "Starting parse");    // Always shown (level 0)
 *   LOG("Parser", "Parsing function");          // Normal detail (level 1)
 *   LOG_DETAIL("Parser", "Token: %s", tok);     // Verbose detail (level 2)
 * 
 * ## Build Configuration
 * 
 *   -DDEBUG_MASTER       → Enable all debug output (overrides all)
 *   -DDEBUG_PARSER       → Enable PARSER component only
 *   -DDEBUG_VERBOSITY=2  → Set detail level (0=minimal, 1=normal, 2=detail)
 *   -DDEBUG_TO_FILE      → Write to debug.log instead of stdout
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
#include <cstdarg>

namespace Debug {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration (set via build flags)
// ─────────────────────────────────────────────────────────────────────────────

/** @brief Check if debug is enabled for a component */
inline bool isEnabled(const char* component) {
    #ifdef DEBUG_MASTER
        return true;
    #else
        #ifdef DEBUG_PARSER
            if (std::string(component) == "PARSER") return true;
        #endif
        #ifdef DEBUG_LEXER
            if (std::string(component) == "LEXER") return true;
        #endif
        #ifdef DEBUG_SEMANTIC
            if (std::string(component) == "SEMANTIC") return true;
        #endif
        #ifdef DEBUG_CODEGEN
            if (std::string(component) == "CODEGEN") return true;
        #endif
        #ifdef DEBUG_TYPE
            if (std::string(component) == "TYPE") return true;
        #endif
        #ifdef DEBUG_INTERPRETER
            if (std::string(component) == "INTERPRETER") return true;
        #endif
        return false;
    #endif
}

/** @brief Get the current verbosity level (0=minimal, 1=normal, 2=detail) */
inline int verbosity() {
    #ifdef DEBUG_VERBOSITY
        return DEBUG_VERBOSITY;
    #else
        return 1;  // Normal by default
    #endif
}

/** @brief Get the log file path (default: debug.log) */
inline std::string logPath() {
    #ifdef DEBUG_FILE_PATH
        return DEBUG_FILE_PATH;
    #else
        return "debug.log";
    #endif
}

/** @brief Get the output stream (stdout or file) */
inline std::ostream& stream() {
    #ifdef DEBUG_TO_FILE
        static std::ofstream file(logPath(), std::ios::out | std::ios::trunc);
        static bool first = true;
        if (first) {
            first = false;
            std::cout << "[DEBUG] Logging to: " << logPath() << std::endl;
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

} // namespace Debug

// ─────────────────────────────────────────────────────────────────────────────
// Core Logging Macros
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Internal macro that does the actual logging.
 * 
 * @param COMPONENT  The subsystem name (PARSER, LEXER, etc.)
 * @param LEVEL      The verbosity level required (0, 1, 2)
 * @param FORMAT     printf-style format string
 * @param ...        Format arguments
 */
#define LOG_CORE(COMPONENT, LEVEL, FORMAT, ...) \
    do { \
        if (Debug::isEnabled(COMPONENT) && Debug::verbosity() >= (LEVEL)) { \
            Debug::stream() << "[" << Debug::timestamp() << "] " \
                            << "[" << COMPONENT << "] " \
                            << Debug::format(FORMAT, ##__VA_ARGS__) << std::endl; \
        } \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Public Logging Macros (3 levels, 7 components)
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
// Component-Specific Aliases (optional, for convenience)
// ─────────────────────────────────────────────────────────────────────────────

#define LOG_PARSER(...)        LOG("PARSER", __VA_ARGS__)
#define LOG_PARSER_DETAIL(...) LOG_DETAIL("PARSER", __VA_ARGS__)

#define LOG_LEXER(...)         LOG("LEXER", __VA_ARGS__)
#define LOG_LEXER_DETAIL(...)  LOG_DETAIL("LEXER", __VA_ARGS__)

#define LOG_SEMANTIC(...)      LOG("SEMANTIC", __VA_ARGS__)
#define LOG_SEMANTIC_DETAIL(...) LOG_DETAIL("SEMANTIC", __VA_ARGS__)

#define LOG_CODEGEN(...)       LOG("CODEGEN", __VA_ARGS__)
#define LOG_CODEGEN_DETAIL(...) LOG_DETAIL("CODEGEN", __VA_ARGS__)

#define LOG_TYPE(...)          LOG("TYPE", __VA_ARGS__)
#define LOG_TYPE_DETAIL(...)   LOG_DETAIL("TYPE", __VA_ARGS__)

#define LOG_INTERPRETER(...)   LOG("INTERPRETER", __VA_ARGS__)
#define LOG_INTERPRETER_DETAIL(...) LOG_DETAIL("INTERPRETER", __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// Helper: printf-style formatting (safe, no heap allocation)
// ─────────────────────────────────────────────────────────────────────────────

namespace Debug {
    inline std::string format(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        
        // Determine required size
        va_list args_copy;
        va_copy(args_copy, args);
        int size = vsnprintf(nullptr, 0, fmt, args_copy);
        va_end(args_copy);
        
        if (size <= 0) {
            va_end(args);
            return std::string(fmt);  // Fallback to raw format string
        }
        
        // Format the string
        std::string result(size + 1, '\0');
        vsnprintf(&result[0], result.size(), fmt, args);
        result.pop_back();  // Remove null terminator
        va_end(args);
        return result;
    }
}