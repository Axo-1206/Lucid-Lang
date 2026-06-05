/**
 * @file SymbolTable.hpp
 * @responsibility Manages lexical scoping for symbols during semantic analysis.
 *
 * SymbolTable maintains a stack of scopes (hash maps). Each scope maps an
 * InternedString ID to a Symbol. Entering a block, function, or generic
 * declaration pushes a new scope; leaving it pops. Lookups search from the
 * innermost scope outward, implementing standard lexical scoping rules.
 *
 * @related
 *   - SemanticSymbol.hpp – defines the Symbol structure
 *   - SemanticCollector.hpp – populates the symbol table during Phase 1
 *   - SemanticAnalyzer.hpp – uses the table for type resolution and checking
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why a scope stack is necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - Nested blocks (if, for, while) and nested functions create inner scopes
 *   that shadow outer declarations. The stack naturally models this.
 * - Function parameters are declared in the function's own scope, separate
 *   from the enclosing block.
 * - Generic declarations introduce type parameters that are only visible
 *   inside the declaration body.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **InternedString keys** – scopes use uint32_t IDs (from InternedString)
 *   directly as keys, avoiding string copies and giving O(1) comparisons.
 * - **Non‑owning storage** – the table stores Symbols by value; they are not
 *   owned by the table beyond the symbol's lifetime (the AST arena owns the
 *   underlying declaration nodes).
 * - **Stack of unordered_maps** – each scope is a hash map for O(1) average
 *   lookup and insertion. The stack is implemented with std::deque for
 *   efficient push/pop at both ends.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * SymbolTable symbols;
 * symbols.pushScope();                // enter file scope
 *
 * Symbol sym;
 * sym.name = pool.intern("x");
 * sym.kind = SymbolKind::Var;
 * symbols.declare(sym);               // declares 'x' in current scope
 *
 * symbols.pushScope();                // enter a block
 * Symbol* found = symbols.lookup(pool.intern("x")); // finds outer 'x'
 * symbols.popScope();                 // exit block, discarding inner symbols
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The table does not own StringPool; the pool must outlive the table.
 *       All lookup and declaration methods require InternedString arguments,
 *       which are cheap to pass and compare.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "SemanticSymbol.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"

#include <vector>
#include <unordered_map>
#include <string>
#include <deque>

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTable  — Manages the scope stack for symbol declarations
//
// A stack of hash maps ensuring proper visibility of variables and types.
// On entering a block, function, or trait, a new scope is pushed. On exit,
// it is popped. Variable lookups traverse from the innermost scope outwards,
// matching the standard lexical scoping rules.
//
// ## InternedString
//
// All lookups now use InternedString instead of std::string. This provides:
//   - O(1) comparisons (integer comparison)
//   - Reduced memory footprint
//   - Faster lookups in the symbol table
//
// The StringPool is only needed when you need to display symbol names
// (e.g., in diagnostics). Lookup operations themselves work directly
// with InternedString IDs.
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    void pushScope();
    void popScope();
    
    // Declare a symbol in the current scope.
    // Returns false if a symbol with the same name already exists in this scope.
    bool declare(const Symbol& sym);
    
    // Look up a symbol by its interned name, searching from innermost to outermost scope.
    // Returns nullptr if no symbol with that name is found in any accessible scope.
    Symbol* lookup(InternedString name);

    // Const version – returns const pointer, does not modify table.
    const Symbol* lookup(InternedString name) const;
    
    // Look up a symbol only in the current (innermost) scope.
    // Returns nullptr if not found in the current scope.
    Symbol* lookupLocal(InternedString name);
    const Symbol* lookupLocal(InternedString name) const;
    
    // Find all symbols whose names start with the given prefix.
    // Useful for auto-completion and IDE features.
    // Returns a vector of pointers to matching symbols (may be empty).
    // NOTE: This requires the StringPool to convert InternedString to string for prefix matching.
    std::vector<Symbol*> findSymbolsByPrefix(InternedString prefix, const StringPool& pool);
    
    // Find all symbols whose names start with the given string prefix.
    // Convenience overload for string literals (interns the prefix automatically).
    std::vector<Symbol*> findSymbolsByPrefix(const std::string& prefix, const StringPool& pool);
    
    // Get the current scope nesting depth (0 = global scope).
    int currentDepth() const;
    
    // Dump the entire symbol table contents to the log (for debugging).
    // Requires StringPool to convert InternedString to readable strings.
    void dump(const StringPool& pool) const;
    
    // Get a const reference to the global scope (scope index 0).
    // Logs an error and returns an empty map if no scopes exist.
    const std::unordered_map<uint32_t, Symbol>& getGlobalScope() const;
    
    // Check if a symbol exists in any scope (for error recovery)
    bool exists(InternedString name) const {
        return const_cast<SymbolTable*>(this)->lookup(name) != nullptr;
    }
    
    // Get the number of scopes currently on the stack
    size_t scopeCount() const { return scopes_.size(); }

    const std::deque<std::unordered_map<uint32_t, Symbol>>& getScopes() const { return scopes_; }

private:
    // Each scope is a map from InternedString ID to Symbol.
    // Using uint32_t directly as the key is faster than InternedString
    // because it avoids the struct wrapper overhead.
    std::deque<std::unordered_map<uint32_t, Symbol>> scopes_;
};
