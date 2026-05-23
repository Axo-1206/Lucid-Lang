/**
 * @file SemanticContext.hpp
 * @brief Bundles all shared state for the semantic checking phase (Phase 3).
 *
 * The SemanticContext holds references to the core services needed during
 * declaration, expression, and statement checking. It also tracks the
 * current nesting depths for loops and parallel blocks, and the `@extern`
 * flag that permits raw pointer usage.
 *
 * @note `expectedReturn` is **not** stored here because it changes per
 *       function and is passed explicitly to `checkStmt`. All other
 *       context is immutable or only mutated via depth counters.
 */

#pragma once

#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"

/**
 * @struct SemanticContext
 * @brief Carries all contextual information for the semantic checker.
 *
 * This struct is passed as a single argument to `checkTopLevelDecl`,
 * `checkExpr`, and `checkStmt`. It eliminates the need for long parameter
 * lists and ensures that all sub‑passes share the same state.
 */
struct SemanticContext {
    // ── Core services (immutable references) ──────────────────────────────────
    SymbolTable&      symbols;    // Symbol table (scopes, lookups)
    TypeResolver&     resolver;   // Type resolver (generic stacks, substitution)
    TypeChecker&      checker;    // Type compatibility predicates
    DiagnosticEngine& dc;         // Error/warning reporting
    StringPool&       pool;       // Name demangling for diagnostics
    ASTArena&         arena;      // Arena for temporary type synthesis
    InternedString currentFile;   // Current file path (set before processing each file)

    // ── Mutable context flags ────────────────────────────────────────────────
    int  loopDepth     = 0;       // Current nesting depth of loops (for break/continue)
    int  parallelDepth = 0;       // Nesting depth inside `parallel` blocks/loops
    bool insideExtern  = false;   // True when checking an @extern declaration

    /**
     * @brief Constructor that binds all required references.
     * @param syms   Symbol table
     * @param res    Type resolver
     * @param chk    Type checker
     * @param diag   Diagnostic engine
     * @param str    String pool
     * @param arn    AST arena
     */
    SemanticContext(SymbolTable& syms, TypeResolver& res, TypeChecker& chk,
                        DiagnosticEngine& diag, StringPool& str, ASTArena& arn,
                        InternedString file) noexcept
            : symbols(syms), resolver(res), checker(chk), dc(diag),
            pool(str), arena(arn), currentFile(file) {}

    // Disable copy/move – the context is owned by SemanticAnalyzer.
    SemanticContext(const SemanticContext&) = delete;
    SemanticContext& operator=(const SemanticContext&) = delete;

    // Error helpers – automatically use currentFile
    void error(SourceLocation loc, DiagCode code,
               std::initializer_list<std::string> args = {}) const {
        dc.error(DiagnosticCategory::Semantic, currentFile, loc, code, args);
    }

    void warning(SourceLocation loc, DiagCode code,
                 std::initializer_list<std::string> args = {}) const {
        dc.warning(DiagnosticCategory::Semantic, currentFile, loc, code, args);
    }

    void note(SourceLocation loc, const std::string& msg) const {
        dc.note(currentFile, loc, msg);
    }

    // ── Convenience accessors for depth counters ────────────────────────────
    void enterLoop()   { ++loopDepth; }
    void exitLoop()    { --loopDepth; }
    void enterParallel(){ ++parallelDepth; }
    void exitParallel() { --parallelDepth; }
    void enterExtern()  { insideExtern = true; }
    void exitExtern()   { insideExtern = false; }
};