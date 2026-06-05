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
namespace LucDebug {
    inline bool isDebugEnabled(const char* component) {
        #ifdef LUC_DEBUG_MASTER
            return true;
        #else
            #ifdef LUC_DEBUG_LEXER
                if (std::string(component) == "LEXER") return true;
            #endif
            #ifdef LUC_DEBUG_PARSER
                if (std::string(component) == "PARSER") return true;
            #endif
            #ifdef LUC_DEBUG_PARSE_RESULT
                if (std::string(component) == "PARSE_RESULT") return true;
            #endif
            #ifdef LUC_DEBUG_TYPE
                if (std::string(component) == "TYPE") return true;
            #endif
            #ifdef LUC_DEBUG_SEMANTIC
                if (std::string(component) == "SEMANTIC") return true;
            #endif
            #ifdef LUC_DEBUG_DUMP_SYMBOL
                if (std::string(component) == "DUMP_SYMBOL") return true;
            #endif
            #ifdef LUC_DEBUG_CODEGEN
                if (std::string(component) == "CODEGEN") return true;
            #endif
            return false;
        #endif
    }

    inline int getVerbosity() {
        #ifdef LUC_DEBUG_VERBOSITY
            // LUC_DEBUG_VERBOSITY is now a number (0,1,2,3)
            return LUC_DEBUG_VERBOSITY;
        #else
            return 1; // default NORMAL
        #endif
    }

    inline std::string getLogFilePath() {
        #ifdef LUC_DEBUG_FILE_PATH
            return LUC_DEBUG_FILE_PATH;
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
        #ifdef LUC_DEBUG_TO_FILE
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
#define LUC_VERB_MINIMAL 0
#define LUC_VERB_NORMAL  1
#define LUC_VERB_VERBOSE 2
#define LUC_VERB_EXTREME 3

// Helper to check verbosity
#define LUC_VERB_GE(level) (LucDebug::getVerbosity() >= (level))

// Core logging macro with timestamp and component
#define LUC_LOG_CORE(COMPONENT, level, x) \
    do { \
        if (LucDebug::isDebugEnabled(COMPONENT) && LUC_VERB_GE(level)) { \
            LucDebug::getDebugStream() << "[" << LucDebug::timestamp() << "] [" << COMPONENT << "] " << x << std::endl; \
        } \
    } while(0)

// =============================================================================
// PARSER logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_PARSER_MINIMAL(x)   LUC_LOG_CORE("PARSER", LUC_VERB_MINIMAL, x)
#define LUC_LOG_PARSER(x)           LUC_LOG_CORE("PARSER", LUC_VERB_NORMAL, x)
#define LUC_LOG_PARSER_VERBOSE(x)   LUC_LOG_CORE("PARSER", LUC_VERB_VERBOSE, x)
#define LUC_LOG_PARSER_EXTREME(x)   LUC_LOG_CORE("PARSER", LUC_VERB_EXTREME, x)

// =============================================================================
// PARSE_RESULT logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_PARSE_RESULT_MINIMAL(x) LUC_LOG_CORE("PARSE_RESULT", LUC_VERB_MINIMAL, x)
#define LUC_LOG_PARSE_RESULT(x)         LUC_LOG_CORE("PARSE_RESULT", LUC_VERB_NORMAL, x)
#define LUC_LOG_PARSE_RESULT_VERBOSE(x) LUC_LOG_CORE("PARSE_RESULT", LUC_VERB_VERBOSE, x)
#define LUC_LOG_PARSE_RESULT_EXTREME(x) LUC_LOG_CORE("PARSE_RESULT", LUC_VERB_EXTREME, x)

// =============================================================================
// TYPE logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_TYPE_MINIMAL(x)     LUC_LOG_CORE("TYPE", LUC_VERB_MINIMAL, x)
#define LUC_LOG_TYPE(x)             LUC_LOG_CORE("TYPE", LUC_VERB_NORMAL, x)
#define LUC_LOG_TYPE_VERBOSE(x)     LUC_LOG_CORE("TYPE", LUC_VERB_VERBOSE, x)
#define LUC_LOG_TYPE_EXTREME(x)     LUC_LOG_CORE("TYPE", LUC_VERB_EXTREME, x)

// =============================================================================
// SEMANTIC logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_SEMANTIC_MINIMAL(x) LUC_LOG_CORE("SEMANTIC", LUC_VERB_MINIMAL, x)
#define LUC_LOG_SEMANTIC(x)         LUC_LOG_CORE("SEMANTIC", LUC_VERB_NORMAL, x)
#define LUC_LOG_SEMANTIC_VERBOSE(x) LUC_LOG_CORE("SEMANTIC", LUC_VERB_VERBOSE, x)
#define LUC_LOG_SEMANTIC_EXTREME(x) LUC_LOG_CORE("SEMANTIC", LUC_VERB_EXTREME, x)

// =============================================================================
// CODEGEN logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_CODEGEN_MINIMAL(x)  LUC_LOG_CORE("CODEGEN", LUC_VERB_MINIMAL, x)
#define LUC_LOG_CODEGEN(x)          LUC_LOG_CORE("CODEGEN", LUC_VERB_NORMAL, x)
#define LUC_LOG_CODEGEN_VERBOSE(x)  LUC_LOG_CORE("CODEGEN", LUC_VERB_VERBOSE, x)
#define LUC_LOG_CODEGEN_EXTREME(x)  LUC_LOG_CORE("CODEGEN", LUC_VERB_EXTREME, x)

// =============================================================================
// LEXER logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_LEXER_MINIMAL(x)    LUC_LOG_CORE("LEXER", LUC_VERB_MINIMAL, x)
#define LUC_LOG_LEXER(x)            LUC_LOG_CORE("LEXER", LUC_VERB_NORMAL, x)
#define LUC_LOG_LEXER_VERBOSE(x)    LUC_LOG_CORE("LEXER", LUC_VERB_VERBOSE, x)
#define LUC_LOG_LEXER_EXTREME(x)    LUC_LOG_CORE("LEXER", LUC_VERB_EXTREME, x)

// =============================================================================
// EXPRESSION logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_EXPR_MINIMAL(x)     LUC_LOG_CORE("PARSER", LUC_VERB_MINIMAL, "[EXPR] " << x)
#define LUC_LOG_EXPR(x)             LUC_LOG_CORE("PARSER", LUC_VERB_NORMAL, "[EXPR] " << x)
#define LUC_LOG_EXPR_VERBOSE(x)     LUC_LOG_CORE("PARSER", LUC_VERB_VERBOSE, "[EXPR] " << x)
#define LUC_LOG_EXPR_EXTREME(x)     LUC_LOG_CORE("PARSER", LUC_VERB_EXTREME, "[EXPR] " << x)

// =============================================================================
// STATEMENT logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_STMT_MINIMAL(x)     LUC_LOG_CORE("PARSER", LUC_VERB_MINIMAL, "[STMT] " << x)
#define LUC_LOG_STMT(x)             LUC_LOG_CORE("PARSER", LUC_VERB_NORMAL, "[STMT] " << x)
#define LUC_LOG_STMT_VERBOSE(x)     LUC_LOG_CORE("PARSER", LUC_VERB_VERBOSE, "[STMT] " << x)
#define LUC_LOG_STMT_EXTREME(x)     LUC_LOG_CORE("PARSER", LUC_VERB_EXTREME, "[STMT] " << x)

// =============================================================================
// DECLARATION logging macros (with verbosity levels)
// =============================================================================

#define LUC_LOG_DECL_MINIMAL(x)     LUC_LOG_CORE("PARSER", LUC_VERB_MINIMAL, "[DECL] " << x)
#define LUC_LOG_DECL(x)             LUC_LOG_CORE("PARSER", LUC_VERB_NORMAL, "[DECL] " << x)
#define LUC_LOG_DECL_VERBOSE(x)     LUC_LOG_CORE("PARSER", LUC_VERB_VERBOSE, "[DECL] " << x)
#define LUC_LOG_DECL_EXTREME(x)     LUC_LOG_CORE("PARSER", LUC_VERB_EXTREME, "[DECL] " << x)

// =============================================================================
// Legacy compatibility macros
// =============================================================================

#ifdef LUC_DEBUG_MASTER
    #define LUC_LOG(x) LucDebug::getDebugStream() << "[DEBUG] " << x << std::endl
    #define LUC_LOG_KIND(x) LUC_LOG_CORE("DEBUG", LUC_VERB_NORMAL, "kind: " << LucDebug::kindToString(x))
    #define LUC_LOG_TOKEN(t) LUC_LOG_CORE("DEBUG", LUC_VERB_NORMAL, "token: " << LucDebug::tokenTypeToString(t))
#else
    #define LUC_LOG(x)
    #define LUC_LOG_KIND(x)
    #define LUC_LOG_TOKEN(t)
#endif