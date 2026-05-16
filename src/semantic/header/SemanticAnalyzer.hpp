/**
 * @file SemanticAnalyzer.hpp
 *
 * @nutshell Public interface dictating the sequence of semantic validation.
 *
 * @responsibility Entry point orchestrating the four passes to enforce semantic rules.
 *
 * @architecture Phase 3 (checkDecls) is implemented using a "Global Function" pattern.
 *   The recursive calls between Declarations, Statements, and Expressions are distributed
 *   across SemanticDecl.cpp, SemanticStmt.cpp, and SemanticExpr.cpp.
 *
 * @note Why no headers for Phase 3? To avoid complex circular dependencies (e.g. Expr 
 *   needs Stmt, Stmt needs Expr), we use manual "Forward Declarations" at the top of 
 *   each .cpp file. The Linker connects these calls across files at build-time.
 *
 * @related 
 *   - SemanticAnalyzer.cpp, SemanticCollector.hpp, TypeResolver.hpp
 *   - SemanticDecl.cpp, SemanticStmt.cpp, SemanticExpr.cpp
 */
#pragma once

#include "ast/BaseAST.hpp"
#include "ast/support/ASTArena.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"

#include <memory>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// CompilationMode — records which execution model the @aot / @jit directive
// on the main entry point requested.
//
//   AOT     — @aot present on main (or no directive); produce a native binary at build time
//   JIT     — @jit present on main; compile and execute via LLVM JIT at runtime
//
// Written by SemanticAnalyzer during Phase 3.5 entry point validation.
// Read by the compiler driver and the codegen phase.
// ─────────────────────────────────────────────────────────────────────────────
enum class CompilationMode {
    AOT,      // @aot on main or no directive — ahead-of-time, produce native binary
    JIT,      // @jit on main — just-in-time via LLVM JIT
};

class SemanticAnalyzer {
public:
    // Constructor now requires StringPool and ASTArena for interned string handling
    // and arena-based type allocation during semantic analysis.
    explicit SemanticAnalyzer(DiagnosticEngine& dc, StringPool& pool, ASTArena& arena);
    ~SemanticAnalyzer();

    // Run the full semantic pass over all files in a package.
    // Returns false if any errors were emitted.
    bool analyze(std::vector<ProgramAST*>& files);
    void dumpSymbols() const;

    // Returns the compilation mode determined from the @aot / @jit directive
    // on the main entry point. Only meaningful after analyze() returns true.
    CompilationMode getCompilationMode() const { return compilationMode_; }

private:
    void resolveImports(std::vector<ProgramAST*>& files);  // Phase 0: cycle detection
    void collectSymbols(std::vector<ProgramAST*>& files);  // Phase 1: symbol table
    void resolveTypes  (std::vector<ProgramAST*>& files);  // Phase 2: type resolution
    void checkDecls    (std::vector<ProgramAST*>& files);  // Phase 3: full checking
    void annotate      (std::vector<ProgramAST*>& files);  // Phase 4: write annotations
    void validateNoDuplicateSymbols();

    // Sub-components owned by the analyzer.
    std::unique_ptr<SymbolTable>   symbols_;
    std::unique_ptr<TypeResolver>  typeResolver_;
    std::unique_ptr<TypeChecker>   typeChecker_;

    DiagnosticEngine& dc_;
    StringPool& pool_;      // For interned string lookups
    ASTArena& arena_;       // For allocating temporary types during semantic passes

    // Compilation mode determined from @aot / @jit on main.
    // Set during Phase 3.5 entry point validation.
    CompilationMode compilationMode_ = CompilationMode::AOT;

    // Context flags shared across Phase 3 sub-passes.
    bool insideExtern_  = false;
    int  loopDepth_     = 0;
    int  parallelDepth_ = 0;
};