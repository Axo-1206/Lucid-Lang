#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <memory>
#include <filesystem>
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "semantic/SemanticAnalyzer.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/ASTDumper.hpp"

// Helper to get absolute path (Windows)
std::string getAbsolutePath(const std::string& path) {
    #ifdef _WIN32
        char absPath[_MAX_PATH];
        if (_fullpath(absPath, path.c_str(), _MAX_PATH)) {
            return std::string(absPath);
        }
    #endif
    return path;
}

/**
 * Luc Engine Entry Point — Semantic Test Driver
 * Handles source loading, lexical analysis, parsing, and full semantic validation.
 */
int main(int argc, char* argv[]) {
    // ========================================================================
    // DEBUG INITIALIZATION - Print configuration
    // ========================================================================
    #ifdef LUC_DEBUG_MASTER
        std::cout << "[DEBUG] LUC_DEBUG_MASTER is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_PARSER
        std::cout << "[DEBUG] LUC_DEBUG_PARSER is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_TYPE
        std::cout << "[DEBUG] LUC_DEBUG_TYPE is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_SEMANTIC
        std::cout << "[DEBUG] LUC_DEBUG_SEMANTIC is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_PARSE_RESULT
        std::cout << "[DEBUG] LUC_DEBUG_PARSE_RESULT is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_TO_FILE
        std::cout << "[DEBUG] LUC_DEBUG_TO_FILE is ENABLED" << std::endl;
        std::cout << "[DEBUG] Log file path: " << getAbsolutePath(LUC_DEBUG_FILE_PATH) << std::endl;
    #endif
    #ifdef LUC_DEBUG_VERBOSITY
        std::cout << "[DEBUG] LUC_DEBUG_VERBOSITY = " << LUC_DEBUG_VERBOSITY << std::endl;
    #endif
    
    // ========================================================================
    // MAIN LOGS - Using direct cout for clarity
    // ========================================================================
    std::cout << "\n===============================================" << std::endl;
    std::cout << "[MAIN] Luc Compiler Starting" << std::endl;
    std::cout << "[MAIN] ========================================" << std::endl;
    std::cout << "[MAIN] Program started with " << (argc - 1) << " arguments" << std::endl;
    
    if (argc < 2) {
        std::cerr << "Usage: luc-comp <source-file.luc>" << std::endl;
        return 1;
    }

    std::string filePath = argv[1];
    std::cout << "[MAIN] Source file: " << filePath << std::endl;
    
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[MAIN] ERROR: Could not open file " << filePath << std::endl;
        return 1;
    }

    // Read full source into string
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();
    std::cout << "[MAIN] Source file size: " << source.size() << " bytes" << std::endl;

    DiagnosticEngine dc;
    
    // Phase 1: Lexical Analysis
    std::cout << "[MAIN] Starting lexical analysis..." << std::endl;
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();
    std::cout << "[MAIN] Lexical analysis complete: " << tokens.size() << " tokens" << std::endl;

    // Optional: Uncomment to see token list
    // std::cout << "Token List (" << tokens.size() << " tokens found):" << "\n";
    // std::cout << "--------------------------------------------------\n";
    // std::cout << "Line:Col | Type (ID) | Value\n";
    // std::cout << "--------------------------------------------------\n";
    // for (const auto& token : tokens) {
    //     std::printf("%4d:%-3d | %-10d | %s\n", 
    //         token.line, 
    //         token.column, 
    //         static_cast<int>(token.type), 
    //         token.value.c_str()
    //     );
    // }
    // std::cout << "------------------------------------------------" << "\n";
    
    // Error reporting for Lexer
    int errorCount = 0;
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            dc.error(DiagnosticCategory::Lexical, {tok.line, tok.column, filePath},
                    DiagCode::E1001, "Unexpected character: '" + tok.value + "'");
            errorCount++;
            if (errorCount > 50) break;
        }
    }

    if (dc.hasErrors()) {
        std::cerr << "[MAIN] Lexical Analysis FAILED:" << std::endl;
        dc.dumpAll(std::cerr);
        return 1;
    }

    // Phase 2: Syntax Analysis (Parsing)
    std::cout << "[MAIN] Starting syntax analysis..." << std::endl;
    Parser parser(tokens, dc, filePath);
    std::unique_ptr<ProgramAST> program = parser.parse();
    
    if (program && LucDebug::isDebugEnabled("PARSE_RESULT")) {
        LucDebug::ASTDumper dumper(LucDebug::getVerbosity());
        program->accept(dumper);
        LUC_LOG_PARSE_RESULT_MINIMAL("\n" << dumper.getOutput());
    }

    std::cout << "[MAIN] Syntax analysis complete" << std::endl;
    
    if (dc.hasErrors()) {
        std::cerr << "[MAIN] Syntax Analysis (Parsing) FAILED:" << std::endl;
        dc.dumpAll(std::cerr);
        return 1;
    }

    if (!program) {
        std::cerr << "[MAIN] ERROR: Parser returned null program without reporting errors." << std::endl;
        return 1;
    }

    // Phase 3: Semantic Analysis
    // std::cout << "[MAIN] Starting semantic analysis..." << std::endl;
    // std::vector<ProgramAST*> files = { program.get() };
    // SemanticAnalyzer analyzer(dc);
    
    // bool success = analyzer.analyze(files);
    // std::cout << "[MAIN] Semantic analysis complete: " << (success ? "SUCCESS" : "FAILED") << std::endl;
    
    // if (dc.hasErrors()) {
    //     std::cerr << "\n>>> Semantic Analysis FAILED:" << std::endl;
    //     dc.dumpAll(std::cerr);
    //     return 1;
    // }

    // if (dc.hasWarnings()) {
    //     std::cerr << "\n>>> Semantic Analysis SUCCESSFUL with warnings:" << std::endl;
    //     dc.dumpAll(std::cerr);
    // } else {
    //     std::cout << "\n>>> Semantic Analysis SUCCESSFUL!" << std::endl;
    // }
    
    file.close();
    
    std::cout << "[MAIN] === MAIN END ===" << std::endl;
    
    // Flush debug stream to ensure all logs are written
    #ifdef LUC_DEBUG_TO_FILE
        LucDebug::getDebugStream() << std::flush;
        std::cout << "[MAIN] Debug logs written to: " << getAbsolutePath(LUC_DEBUG_FILE_PATH) << std::endl;
    #endif

    return 0;
}