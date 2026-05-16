/**
 * @file SemanticAnalyzer.hpp
 * @responsibility Entry point orchestrating the four passes of semantic analysis.
 *
 * SemanticAnalyzer drives the entire semantic pipeline for a package:
 *   - Phase 0: resolveImports – detect circular `use` declarations.
 *   - Phase 1: collectSymbols – run SemanticCollector to populate the symbol table.
 *   - Phase 2: resolveTypes – run TypeResolver to validate and bind type annotations.
 *   - Phase 3: checkDecls – run the full declaration/expression/statement checkers.
 *   - Phase 4: annotate – write semantic properties (isConst, etc.) back to AST nodes.
 *
 * Phase 3 (checkDecls) is implemented using a "global function" pattern spread across
 * SemanticDecl.cpp, SemanticStmt.cpp, and SemanticExpr.cpp. Forward declarations
 * at the top of each .cpp file avoid circular header dependencies.
 *
 * @related
 *   - SemanticCollector.hpp – Phase 1 visitor
 *   - TypeResolver.hpp – Phase 2a type resolution
 *   - TypeChecker.hpp – Phase 2b type compatibility
 *   - SemanticDecl.cpp, SemanticStmt.cpp, SemanticExpr.cpp – Phase 3 implementation
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why these four passes are necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - Phase 0 (imports): prevents cycles in the module graph early, before
 *   expensive symbol collection and type resolution.
 * - Phase 1 (symbols): collects all names in a file before type resolution,
 *   enabling forward references (use a struct before its definition).
 * - Phase 2 (types): resolves every type annotation so that subsequent checks
 *   have concrete TypeAST nodes (no unresolved names).
 * - Phase 3 (checking): enforces all semantic rules: assignment compatibility,
 *   control‑flow correctness, generics instantiation, pattern exhaustiveness, etc.
 * - Phase 4 (annotation): stamps isConst, isBehaviorMember, and other flags
 *   on AST nodes, preparing them for code generation.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **Global functions for Phase 3** – The checking phase recursively traverses
 *   declarations, statements, and expressions, which would create circular
 *   includes if placed in headers. Instead, each .cpp file declares the external
 *   functions it needs, and the linker resolves them.
 * - **Explicit StringPool and ASTArena dependencies** – The analyzer stores
 *   references to the pool and arena, passing them to the sub‑components.
 * - **CompilationMode detection** – The analyzer reads @aot/@jit attributes on
 *   the `main` function and stores the mode for the driver and codegen.
 * - **Context flags for Phase 3** – `insideExtern_`, `loopDepth_`, `parallelDepth_`
 *   are shared across the recursive checking functions to enforce restrictions
 *   (e.g., no `await` inside parallel blocks).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * DiagnosticEngine dc;
 * StringPool pool;
 * ASTArena arena;
 * SemanticAnalyzer analyzer(dc, pool, arena);
 *
 * std::vector<ProgramAST*> files = ...; // from parsing
 * if (analyzer.analyze(files)) {
 *     // Semantic analysis succeeded – ready for code generation
 *     if (analyzer.getCompilationMode() == CompilationMode::JIT) {
 *         // Run JIT compilation
 *     }
 * }
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note Phase 3 uses manual forward declarations to avoid circular includes.
 *       Adding a new semantic check may require updating the forward declarations
 *       at the top of SemanticDecl.cpp, SemanticStmt.cpp, or SemanticExpr.cpp.
 * ─────────────────────────────────────────────────────────────────────────────
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