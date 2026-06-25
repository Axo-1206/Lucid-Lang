#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <unordered_set>

#include "parser/Lexer.hpp"

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    // ========================================================================
    // DEBUG INITIALISATION
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
#ifdef LUC_DEBUG_DUMP_SYMBOL
    std::cout << "[DEBUG] LUC_DEBUG_DUMP_SYMBOL is ENABLED" << std::endl;
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

//     std::cout << "\n===============================================" << std::endl;
//     std::cout << "[MAIN] Luc Compiler Starting" << std::endl;
//     std::cout << "[MAIN] ========================================" << std::endl;

//     // ========================================================================
//     // COMMAND LINE VALIDATION
//     // ========================================================================
//     if (argc != 2) {
//         std::cerr << "Usage: luc-comp <source-file.luc>" << std::endl;
//         std::cerr << "  Only the root file (containing 'main') should be provided." << std::endl;
//         std::cerr << "  All dependencies are discovered via 'use' declarations." << std::endl;
//         return 1;
//     }

//     // ========================================================================
//     // GLOBAL RESOURCES
//     // ========================================================================
//     StringPool stringPool;                     // String interning (shared)
//     attribute::initialize(stringPool);         // @attr registry
//     intrinsic::initialize(stringPool);         // #intrinsic registry
//     qualifier::initialize(stringPool);          // ~qualifier registry
//     ASTArena arena;                            // Bump‑pointer allocator for AST nodes

//     // Intern "main" once for all comparisons
//     uint32_t mainId = stringPool.intern("main").id;

//     // ========================================================================
//     // PHASE 1: PARSE ROOT FILE & ALL IMPORTS
//     // ========================================================================
    
//     std::string rootFile = getAbsolutePath(argv[1]);
//     std::cout << "[MAIN] Root file: " << rootFile << std::endl;
    
//     std::unordered_set<std::string> parsedFiles;
//     std::vector<ProgramAST*> programs;
//     bool hasError = false;
    
//     ProgramAST* rootProgram = parseFileRecursive(rootFile, stringPool, arena, parsedFiles, programs, hasError);
    
//     if (!rootProgram || hasError) {
//         std::cerr << "\n>>> Failed to parse root file or its dependencies." << std::endl;
//         diagnostic::dumpAll(stringPool, std::cerr);
//         attribute::shutdown();
//         intrinsic::shutdown();
//         qualifier::shutdown();
//         return 1;
//     }
    
//     // EARLY 'main' VALIDATION – existence only, not semantics
//     if (!hasMainFunction(rootProgram, mainId)) {
//         std::cerr << "\n>>> ERROR: No 'main' function found in root file: " << argv[1] << std::endl;
//         std::cerr << "    A program must have a 'main' entry point." << std::endl;
//         attribute::shutdown();
//         intrinsic::shutdown();
//         qualifier::shutdown();
//         return 1;
//     }
    
//     std::cout << "[MAIN] Root file parsed successfully. 'main' function found." << std::endl;
//     std::cout << "[MAIN] Total files parsed: " << programs.size() << std::endl;

//     // ========================================================================
//     // PHASE 2: SEMANTIC ANALYSIS
//     // ========================================================================
//     // The semantic analyser performs:
//     //   - Symbol collection (Phase 1)
//     //   - Type resolution (Phase 2)
//     //   - Trait conformance mapping (Phase 2.5)
//     //   - Declaration checking (Phase 3)
//     //   - Entry point validation (Phase 3.5) – full 'main' signature validation
//     //   - Annotation (Phase 4)
//     // ========================================================================
    
//     std::cout << "[MAIN] Starting semantic analysis on " << programs.size() << " files..." << std::endl;
//     SemanticAnalyzer analyzer(stringPool, arena);
//     bool semanticSuccess = analyzer.analyze(programs);
//     std::cout << "[MAIN] Semantic analysis complete: " << (semanticSuccess ? "SUCCESS" : "FAILED") << std::endl;

//     // ========================================================================
//     // ERROR REPORTING (Semantic Analysis)
//     // ========================================================================
//     if (diagnostic::hasErrors()) {
//         std::cerr << "\n>>> Semantic Analysis FAILED:" << std::endl;
//         diagnostic::dumpAll(stringPool, std::cerr);
//         attribute::shutdown();
//         intrinsic::shutdown();
//         qualifier::shutdown();
//         return 1;
//     }

//     if (diagnostic::hasWarnings()) {
//         std::cerr << "\n>>> Semantic Analysis SUCCESSFUL with warnings:" << std::endl;
//         diagnostic::dumpAll(stringPool, std::cerr);
//     } else {
//         std::cout << "\n>>> Semantic Analysis SUCCESSFUL!" << std::endl;
//     }

//     // ========================================================================
//     // CLEANUP
//     // ========================================================================
//     attribute::shutdown();
//     intrinsic::shutdown();
//     qualifier::shutdown();

//     // Flush debug stream if logging to file
// #ifdef LUC_DEBUG_TO_FILE
//     LucDebug::getDebugStream() << std::flush;
//     std::cout << "[MAIN] Debug logs written to: " << getAbsolutePath(LUC_DEBUG_FILE_PATH) << std::endl;
// #endif

//     std::cout << "[MAIN] Compilation finished successfully." << std::endl;
    return 0;
}