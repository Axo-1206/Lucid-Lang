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

/**
 * Luc Engine Entry Point — Semantic Test Driver
 * Handles source loading, lexical analysis, parsing, and full semantic validation.
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: luc-comp <source-file.luc>" << std::endl;
        return 1;
    }

    std::string filePath = argv[1];
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filePath << std::endl;
        return 1;
    }

    // Read full source into string
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    DiagnosticEngine dc;
    
    // Phase 1: Lexical Analysis
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();

    std::cout << "Token List (" << tokens.size() << " tokens found):" << "\n";
    std::cout << "------------------------------------------------" << "\n";
    
    // Error reporting for Lexer (Lexer doesn't take DiagnosticEngine currently)
    int errorCount = 0;
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            dc.error(DiagnosticCategory::Lexical, {tok.line, tok.column, filePath},
                    DiagCode::E1001, "Unexpected character: '" + tok.value + "'");
            errorCount++;
            if (errorCount > 50) break; // Stop after 50 errors to prevent "hanging"
        }
    }

    if (dc.hasErrors()) {
        std::cerr << "Lexical Analysis FAILED:" << std::endl;
        dc.dumpAll(std::cerr);
        return 1;
    }

    // Phase 2: Syntax Analysis (Parsing)
    Parser parser(tokens, dc, filePath);
    std::unique_ptr<ProgramAST> program = parser.parse();
    
    if (dc.hasErrors()) {
        std::cerr << "Syntax Analysis (Parsing) FAILED:" << std::endl;
        dc.dumpAll(std::cerr);
        return 1;
    }

    if (!program) {
        std::cerr << "Error: Parser returned null program without reporting errors." << std::endl;
        return 1;
    }

    // Phase 3: Semantic Analysis
    // The analyzer expects a vector of all files in the package.
    std::vector<ProgramAST*> files = { program.get() };
    SemanticAnalyzer analyzer(dc);
    
    bool success = analyzer.analyze(files);
    
    if (!success || dc.hasErrors()) {
        std::cerr << "\n>>> Semantic Analysis FAILED:" << std::endl;
        dc.dumpAll(std::cerr);
        return 1;
    }

    std::cout << ">>> Semantic Analysis SUCCESSFUL!" << std::endl;
    
    file.close();

    return 0;
}