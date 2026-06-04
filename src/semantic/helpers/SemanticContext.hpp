/**
 * @file SemanticContext.hpp
 * @brief Plain data container for all semantic analysis state.
 *
 * SemanticContext holds references and flags needed during semantic passes.
 * It is passed by reference to every semantic function. It does NOT own
 * any components; ownership remains with SemanticAnalyzer.
 */

#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include <utility>
#include <string>
#include <sstream>

struct SemanticContext {
    // ── References to shared resources ──────────────────────────────────────
    StringPool&   pool;      // String pool (for name demangling)
    ASTArena&     arena;     // Arena for temporary type synthesis
    SymbolTable*  symbols;   // Symbol table (non‑owning, owned by SemanticAnalyzer)
    TypeResolver* resolver;  // Type resolver (non‑owning, owned by SemanticAnalyzer)

    // ── Per‑file state (set before processing each file) ────────────────────
    InternedString currentFile;

    // ── Mutable flags for checking phase ────────────────────────────────────
    int  loopDepth     = 0;
    int  parallelDepth = 0;
    bool insideExtern  = false;

    /**
     * @brief Constructor – binds references to required resources.
     * @param p String pool
     * @param a AST arena
     * @param sym Symbol table (pointer, may be null initially)
     * @param res Type resolver (pointer, may be null initially)
     */
    SemanticContext(StringPool& p, ASTArena& a, SymbolTable* sym = nullptr, TypeResolver* res = nullptr)
        : pool(p), arena(a), symbols(sym), resolver(res) {}

    // ── Convenience diagnostic helpers (using global diagnostic module) ─────
    
    /**
     * @brief Helper to convert any printable type to string.
     */
    template<typename T>
    static std::string toString(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    // Specialization for string_view
    static std::string toString(std::string_view sv) {
        return std::string(sv);
    }

    // Specialization for const char*
    static std::string toString(const char* str) {
        return std::string(str);
    }

    // Specialization for std::string
    static std::string toString(const std::string& s) {
        return s;
    }

    /**
     * @brief Report an error with any number of format arguments.
     * @param loc Source location
     * @param code Diagnostic code
     * @param args Format arguments to substitute for %s placeholders
     */
    template<typename... Args>
    void error(SourceLocation loc, DiagCode code, Args&&... args) const {
        std::initializer_list<std::string> argList = { toString(std::forward<Args>(args))... };
        diagnostic::error(DiagnosticCategory::Semantic, currentFile, loc, code, argList);
    }

    /**
     * @brief Report a warning with any number of format arguments.
     * @param loc Source location
     * @param code Diagnostic code
     * @param args Format arguments to substitute for %s placeholders
     */
    template<typename... Args>
    void warning(SourceLocation loc, DiagCode code, Args&&... args) const {
        std::initializer_list<std::string> argList = { toString(std::forward<Args>(args))... };
        diagnostic::warning(DiagnosticCategory::Semantic, currentFile, loc, code, argList);
    }

    /**
     * @brief Report a free‑text note.
     * @param loc Source location
     * @param msg Human‑readable message
     */
    void note(SourceLocation loc, const std::string& msg) const {
        diagnostic::note(currentFile, loc, msg);
    }

    // ── Depth counter helpers ──────────────────────────────────────────────
    void enterLoop()   { ++loopDepth; }
    void exitLoop()    { --loopDepth; }
    void enterParallel(){ ++parallelDepth; }
    void exitParallel() { --parallelDepth; }
    void enterExtern()  { insideExtern = true; }
    void exitExtern()   { insideExtern = false; }
};