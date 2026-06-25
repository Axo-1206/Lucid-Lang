#pragma once

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <sstream> 
#include <iomanip>
#include <string>

// =============================================================================
// Debug logging configuration
// =============================================================================

// Helper to check if debug is enabled for a component
namespace Debug {
    inline bool isDebugEnabled(const char* component) {
        #ifdef DEBUG_MASTER
            return true;
        #else
            #ifdef DEBUG_LEXER
                if (std::string(component) == "LEXER") return true;
            #endif
            #ifdef DEBUG_LEXER_TOKENS 
                if (std::string(component) == "LEXER_TOKENS") return true;
            #endif
            #ifdef DEBUG_PARSER
                if (std::string(component) == "PARSER") return true;
            #endif
            #ifdef DEBUG_PARSE_RESULT
                if (std::string(component) == "PARSE_RESULT") return true;
            #endif
            #ifdef DEBUG_TYPE
                if (std::string(component) == "TYPE") return true;
            #endif
            #ifdef DEBUG_SEMANTIC
                if (std::string(component) == "SEMANTIC") return true;
            #endif
            #ifdef DEBUG_DUMP_SYMBOL
                if (std::string(component) == "DUMP_SYMBOL") return true;
            #endif
            #ifdef DEBUG_CODEGEN
                if (std::string(component) == "CODEGEN") return true;
            #endif
            return false;
        #endif
    }

    inline int getVerbosity() {
        #ifdef DEBUG_VERBOSITY
            // DEBUG_VERBOSITY is now a number (0,1,2,3)
            return DEBUG_VERBOSITY;
        #else
            return 1; // default NORMAL
        #endif
    }

    inline std::string getLogFilePath() {
        #ifdef DEBUG_FILE_PATH
            return DEBUG_FILE_PATH;
        #else
            return "debug.log";
        #endif
    }

    // Flag to track if we've printed the log file message
    inline bool& logFileMessagePrinted() {
        static bool printed = false;
        return printed;
    }

    inline std::ostream& getDebugStream() {
        #ifdef DEBUG_TO_FILE
            static std::ofstream file(getLogFilePath(), std::ios::out | std::ios::trunc);
            
            // Print message once when first accessed
            if (!logFileMessagePrinted()) {
                logFileMessagePrinted() = true;
                
                // Get absolute path for display
                std::string logPath = getLogFilePath();
                
                // Try to get absolute path (Windows)
                #ifdef _WIN32
                    char absPath[_MAX_PATH];
                    if (_fullpath(absPath, logPath.c_str(), _MAX_PATH)) {
                        logPath = absPath;
                    }
                #endif
                
                std::cout << "[DEBUG] Log file enabled: " << logPath << std::endl;
            }
            
            return file;
        #else
            return std::cout;
        #endif
    }
    
    inline std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        return ss.str();
    }
}

// =============================================================================
// Logging macros with verbosity control
// =============================================================================

// Verbosity levels
#define VERB_MINIMAL 0
#define VERB_NORMAL  1
#define VERB_VERBOSE 2
#define VERB_EXTREME 3

// Helper to check verbosity
#define VERB_GE(level) (LucDebug::getVerbosity() >= (level))

// Core logging macro with timestamp and component
#define LOG_CORE(COMPONENT, level, x) \
    do { \
        if (LucDebug::isDebugEnabled(COMPONENT) && VERB_GE(level)) { \
            LucDebug::getDebugStream() << "[" << LucDebug::timestamp() << "] [" << COMPONENT << "] " << x << std::endl; \
        } \
    } while(0)

// =============================================================================
// PARSER logging macros (with verbosity levels)
// =============================================================================

#define LOG_PARSER_MINIMAL(x)   LOG_CORE("PARSER", VERB_MINIMAL, x)
#define LOG_PARSER(x)           LOG_CORE("PARSER", VERB_NORMAL, x)
#define LOG_PARSER_VERBOSE(x)   LOG_CORE("PARSER", VERB_VERBOSE, x)
#define LOG_PARSER_EXTREME(x)   LOG_CORE("PARSER", VERB_EXTREME, x)

// =============================================================================
// PARSE_RESULT logging macros (with verbosity levels)
// =============================================================================

#define LOG_PARSE_RESULT_MINIMAL(x) LOG_CORE("PARSE_RESULT", VERB_MINIMAL, x)
#define LOG_PARSE_RESULT(x)         LOG_CORE("PARSE_RESULT", VERB_NORMAL, x)
#define LOG_PARSE_RESULT_VERBOSE(x) LOG_CORE("PARSE_RESULT", VERB_VERBOSE, x)
#define LOG_PARSE_RESULT_EXTREME(x) LOG_CORE("PARSE_RESULT", VERB_EXTREME, x)

// =============================================================================
// TYPE logging macros (with verbosity levels)
// =============================================================================

#define LOG_TYPE_MINIMAL(x)     LOG_CORE("TYPE", VERB_MINIMAL, x)
#define LOG_TYPE(x)             LOG_CORE("TYPE", VERB_NORMAL, x)
#define LOG_TYPE_VERBOSE(x)     LOG_CORE("TYPE", VERB_VERBOSE, x)
#define LOG_TYPE_EXTREME(x)     LOG_CORE("TYPE", VERB_EXTREME, x)

// =============================================================================
// SEMANTIC logging macros (with verbosity levels)
// =============================================================================

#define LOG_SEMANTIC_MINIMAL(x) LOG_CORE("SEMANTIC", VERB_MINIMAL, x)
#define LOG_SEMANTIC(x)         LOG_CORE("SEMANTIC", VERB_NORMAL, x)
#define LOG_SEMANTIC_VERBOSE(x) LOG_CORE("SEMANTIC", VERB_VERBOSE, x)
#define LOG_SEMANTIC_EXTREME(x) LOG_CORE("SEMANTIC", VERB_EXTREME, x)

// =============================================================================
// CODEGEN logging macros (with verbosity levels)
// =============================================================================

#define LOG_CODEGEN_MINIMAL(x)  LOG_CORE("CODEGEN", VERB_MINIMAL, x)
#define LOG_CODEGEN(x)          LOG_CORE("CODEGEN", VERB_NORMAL, x)
#define LOG_CODEGEN_VERBOSE(x)  LOG_CORE("CODEGEN", VERB_VERBOSE, x)
#define LOG_CODEGEN_EXTREME(x)  LOG_CORE("CODEGEN", VERB_EXTREME, x)

// =============================================================================
// LEXER logging macros (with verbosity levels)
// =============================================================================

#define LOG_LEXER_MINIMAL(x)    LOG_CORE("LEXER", VERB_MINIMAL, x)
#define LOG_LEXER(x)            LOG_CORE("LEXER", VERB_NORMAL, x)
#define LOG_LEXER_VERBOSE(x)    LOG_CORE("LEXER", VERB_VERBOSE, x)
#define LOG_LEXER_EXTREME(x)    LOG_CORE("LEXER", VERB_EXTREME, x)

// =============================================================================
// EXPRESSION logging macros (with verbosity levels)
// =============================================================================

#define LOG_EXPR_MINIMAL(x)     LOG_CORE("PARSER", VERB_MINIMAL, "[EXPR] " << x)
#define LOG_EXPR(x)             LOG_CORE("PARSER", VERB_NORMAL, "[EXPR] " << x)
#define LOG_EXPR_VERBOSE(x)     LOG_CORE("PARSER", VERB_VERBOSE, "[EXPR] " << x)
#define LOG_EXPR_EXTREME(x)     LOG_CORE("PARSER", VERB_EXTREME, "[EXPR] " << x)

// =============================================================================
// STATEMENT logging macros (with verbosity levels)
// =============================================================================

#define LOG_STMT_MINIMAL(x)     LOG_CORE("PARSER", VERB_MINIMAL, "[STMT] " << x)
#define LOG_STMT(x)             LOG_CORE("PARSER", VERB_NORMAL, "[STMT] " << x)
#define LOG_STMT_VERBOSE(x)     LOG_CORE("PARSER", VERB_VERBOSE, "[STMT] " << x)
#define LOG_STMT_EXTREME(x)     LOG_CORE("PARSER", VERB_EXTREME, "[STMT] " << x)

// =============================================================================
// DECLARATION logging macros (with verbosity levels)
// =============================================================================

#define LOG_DECL_MINIMAL(x)     LOG_CORE("PARSER", VERB_MINIMAL, "[DECL] " << x)
#define LOG_DECL(x)             LOG_CORE("PARSER", VERB_NORMAL, "[DECL] " << x)
#define LOG_DECL_VERBOSE(x)     LOG_CORE("PARSER", VERB_VERBOSE, "[DECL] " << x)
#define LOG_DECL_EXTREME(x)     LOG_CORE("PARSER", VERB_EXTREME, "[DECL] " << x)

// =============================================================================
// Legacy compatibility macros
// =============================================================================

#ifdef DEBUG_MASTER
    #define LOG(x) LucDebug::getDebugStream() << "[DEBUG] " << x << std::endl
    #define LOG_KIND(x) LOG_CORE("DEBUG", VERB_NORMAL, "kind: " << LucDebug::kindToString(x))
    #define LOG_TOKEN(t) LOG_CORE("DEBUG", VERB_NORMAL, "token: " << LucDebug::tokenTypeToString(t))
#else
    #define LOG(x)
    #define LOG_KIND(x)
    #define LOG_TOKEN(t)
#endif