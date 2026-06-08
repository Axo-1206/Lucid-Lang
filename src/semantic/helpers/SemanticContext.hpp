/**
 * @file SemanticContext.hpp
 * @brief Plain data container for all semantic analysis state.
 * 
 * ============================================================================
 * DESIGN NOTES
 * ============================================================================
 * 
 * This context is passed by reference through all semantic phases.
 * It holds non-owning references to shared resources.
 * 
 * Key changes from previous design:
 *   - Replaced SymbolTable* with ScopeStack& (AST-node-only design)
 *   - Replaced TypeDispatcher* with TypeResolver* (simpler resolver)
 *   - Added typeTraits map for trait conformance (built in Phase 2.5)
 *   - Added diagnostic helpers with variadic template support
 * 
 * The context is mutable during analysis (error counts, depths, etc.)
 * but references to shared resources are immutable after construction.
 */

#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"
#include "ast/TypeAST.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "scope/ScopeStack.hpp"
#include <utility>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

// Forward declaration
namespace luc {
class TypeResolver;
}

/**
 * @brief Holds all mutable state during semantic analysis.
 * 
 * This struct is passed by reference to every checker function.
 * It provides:
 *   - Access to shared resources (pool, arena, scope, resolver)
 *   - Diagnostic reporting helpers (error, warning, note)
 *   - Depth tracking for loops and parallel blocks
 *   - Trait conformance map (built after type resolution)
 */
struct SemanticContext {
    // ─────────────────────────────────────────────────────────────────────────
    // Shared Resources (non-owning references)
    // ─────────────────────────────────────────────────────────────────────────
    
    StringPool& pool;           // String pool for name demangling
    ASTArena& arena;            // Arena for temporary type synthesis
    luc::ScopeStack& scope;     // Scope stack for name lookup (AST-node-only)
    luc::TypeResolver* typeResolver = nullptr;  // Type resolver (set after construction)
    
    // ─────────────────────────────────────────────────────────────────────────
    // Trait Conformance Map (built in Phase 2.5, read-only thereafter)
    // ─────────────────────────────────────────────────────────────────────────
    //
    // This map records which types implement which traits.
    // Key: Canonical mangled type string (e.g., "Pint" for int, "Aslice_int" for [_, int])
    // Value: List of trait names that the type implements
    //
    // Built by buildTraitConformanceMap() after all type resolution is complete.
    // Used by ConstraintChecker::satisfies() to verify generic constraints.
    std::unordered_map<std::string, std::vector<InternedString>> typeTraits;

    // ─────────────────────────────────────────────────────────────────────────
    // Per‑file State (set before processing each file)
    // ─────────────────────────────────────────────────────────────────────────
    
    InternedString currentFile;   // Current file being processed (for diagnostics)
    
    // ─────────────────────────────────────────────────────────────────────────
    // Mutable Flags for Checking Phase
    // ─────────────────────────────────────────────────────────────────────────
    
    int loopDepth = 0;          // Current loop nesting depth (for break/continue)
    int parallelDepth = 0;      // Current parallel block depth (for restrictions)
    bool insideExtern = false;  // True when inside an @extern function

    // ─────────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Constructs a SemanticContext with required resources.
     * 
     * @param p String pool reference
     * @param a AST arena reference
     * @param s Scope stack reference (for name lookup)
     */
    SemanticContext(StringPool& p, ASTArena& a, luc::ScopeStack& s)
        : pool(p), arena(a), scope(s) {}

    // ─────────────────────────────────────────────────────────────────────────
    // Diagnostic Helpers (using global diagnostic module)
    // ─────────────────────────────────────────────────────────────────────────
    
private:
    // Internal helper for converting any printable type to string
    template<typename T>
    static std::string toStringImpl(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }
    
    static std::string toStringImpl(const std::string& s) { return s; }
    static std::string toStringImpl(const std::string_view& sv) { return std::string(sv); }
    static std::string toStringImpl(const char* str) { return std::string(str); }
    static std::string toStringImpl(const InternedString& is);
    static std::string toStringImpl(const SourceLocation& loc) {
        return std::to_string(loc.line()) + ":" + std::to_string(loc.column());
    }

public:
    /**
     * @brief Converts any printable type to string.
     * 
     * Specialized for common types: string, string_view, const char*, InternedString.
     * Falls back to ostringstream for other types.
     */
    template<typename T>
    static std::string toString(const T& value) {
        return toStringImpl(value);
    }

    /**
     * @brief Reports an error with variadic format arguments.
     * 
     * Example: ctx.error(loc, DiagCode::E2001, "variable '", name, "' not found");
     * 
     * @param loc Source location
     * @param code Diagnostic code
     * @param args Format arguments (any printable type)
     */
    template<typename... Args>
    void error(SourceLocation loc, DiagCode code, Args&&... args) const {
        std::initializer_list<std::string> argList = { toString(std::forward<Args>(args))... };
        diagnostic::error(DiagnosticCategory::Semantic, currentFile, loc, code, argList);
    }

    /**
     * @brief Reports a warning with variadic format arguments.
     * 
     * @param loc Source location
     * @param code Diagnostic code
     * @param args Format arguments (any printable type)
     */
    template<typename... Args>
    void warning(SourceLocation loc, DiagCode code, Args&&... args) const {
        std::initializer_list<std::string> argList = { toString(std::forward<Args>(args))... };
        diagnostic::warning(DiagnosticCategory::Semantic, currentFile, loc, code, argList);
    }

    /**
     * @brief Reports a free‑text note (no diagnostic code).
     * 
     * @param loc Source location
     * @param msg Human‑readable message
     */
    void note(SourceLocation loc, const std::string& msg) const {
        diagnostic::note(currentFile, loc, msg);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Depth Counter Helpers
    // ─────────────────────────────────────────────────────────────────────────
    
    void enterLoop()   { ++loopDepth; }
    void exitLoop()    { --loopDepth; }
    void enterParallel(){ ++parallelDepth; }
    void exitParallel() { --parallelDepth; }
    void enterExtern()  { insideExtern = true; }
    void exitExtern()   { insideExtern = false; }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Trait Helper
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Checks if a type implements a given trait.
     * 
     * Looks up the type's canonical mangled key in typeTraits map.
     * 
     * @param type The type to check (after alias resolution)
     * @param traitName The trait name to check for
     * @return true if the type implements the trait
     */
    bool implementsTrait(TypeAST* type, InternedString traitName) const;
};

// Specialization for InternedString (needs pool lookup)
inline std::string SemanticContext::toStringImpl(const InternedString& is) {
    if (!is.isValid()) return "<invalid>";
    // Note: This requires pool access – we'll handle it in the calling context
    // For now, return the ID as string
    return std::to_string(is.id);
}