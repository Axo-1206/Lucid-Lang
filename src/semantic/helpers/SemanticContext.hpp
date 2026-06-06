/**
 * @file SemanticContext.hpp
 * @brief Plain data container for all semantic analysis state.
 */

#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "semantic/SymbolTable.hpp"
#include <utility>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

class TypeDispatcher;

struct SemanticContext {
    // ── References to shared resources ──────────────────────────────────────
    StringPool&   pool;      // String pool (for name demangling)
    ASTArena&     arena;     // Arena for temporary type synthesis
    SymbolTable*  symbols;   // Symbol table (non‑owning, owned by SemanticAnalyzer)
    TypeDispatcher* dispatcher = nullptr;  // Type dispatcher (set after construction)
    
    // ── Type → Traits Mapping (built in Phase 2, read-only thereafter) ──────
    //
    // This map records which types implement which traits.
    // Key: Canonical mangled type string (e.g., "prim5" for int, "slice_int" for [_, int])
    // Value: List of trait names that the type implements
    //
    // Built by TraitResolver in Phase 2 after all type resolution is complete.
    // Used by ConstraintChecker::satisfies() to verify generic constraints.
    std::unordered_map<InternedString, std::vector<InternedString>> typeTraits;

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
    SemanticContext(StringPool& p, ASTArena& a, SymbolTable* sym = nullptr)
        : pool(p), arena(a), symbols(sym) {}

    // ── Convenience diagnostic helpers (using global diagnostic module) ─────
    
    /**
     * @brief Helper to convert any printable type to string.
     */
    template<typename T>
    static std::string toString(const T& value) {
        // For types with operator<<, use ostringstream
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*>) {
            return toString_impl(value);
        } else {
            std::ostringstream oss;
            oss << value;
            return oss.str();
        }
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

    // Specialization for InternedString
    static std::string toString(const InternedString& is);

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

    template<typename T>
    static std::string toString_impl(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
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
    
    // ── Trait helper ────────────────────────────────────────────────────────
    bool implementsTrait(TypeAST* type, InternedString traitName) const;
};