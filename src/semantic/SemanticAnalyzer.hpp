/**
 * @file SemanticAnalyzer.hpp
 *
 * @nutshell Public interface dictating the sequence of semantic validation.
 *
 * @responsibility Entry point orchestrating the four passes to enforce semantic rules.
 *
 * @related SemanticAnalyzer.cpp, SemanticCollector.hpp, TypeResolver.hpp
 */
#pragma once

#include "ast/BaseAST.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"

#include <memory>
#include <vector>

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(DiagnosticEngine& dc);
    ~SemanticAnalyzer();

    // Run the full semantic pass over all files in a package.
    // Returns false if any errors were emitted.
    bool analyze(std::vector<ProgramAST*>& files);

private:
    void resolveImports(std::vector<ProgramAST*>& files);  // Phase 0: cycle detection
    void collectSymbols(std::vector<ProgramAST*>& files);  // Phase 1: symbol table
    void resolveTypes  (std::vector<ProgramAST*>& files);  // Phase 2: type resolution
    void checkDecls    (std::vector<ProgramAST*>& files);  // Phase 3: full checking
    void annotate      (std::vector<ProgramAST*>& files);  // Phase 4: write annotations

    // Sub-components owned by the analyzer.
    std::unique_ptr<SymbolTable>   symbols_;
    std::unique_ptr<TypeResolver>  typeResolver_;
    std::unique_ptr<TypeChecker>   typeChecker_;

    DiagnosticEngine& dc_;

    // Context flags shared across Phase 3 sub-passes.
    bool insideExtern_  = false;
    int  asyncDepth_    = 0;
    int  loopDepth_     = 0;
    int  parallelDepth_ = 0;
};