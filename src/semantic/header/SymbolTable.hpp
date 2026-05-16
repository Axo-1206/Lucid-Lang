/**
 * @file SymbolTable.hpp
 *
 * @nutshell The stack tracker guaranteeing variables respect their bounding boxes.
 *
 * @reason Serves as the state-machine memory during parsing and semantic analysis to verify names don't leak sideways into unrelated functions or files.
 *
 * @responsibility The scope stack to track variable and type declarations across blocks, functions, and impls.
 *
 * @logic Every block, function body, and impl block pushes a new scope. Variable lookups walk the stack outward.
 *
 * @related SemanticSymbol.hpp, SemanticAnalyzer.hpp
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

private:
    // Each scope is a map from InternedString ID to Symbol.
    // Using uint32_t directly as the key is faster than InternedString
    // because it avoids the struct wrapper overhead.
    std::deque<std::unordered_map<uint32_t, Symbol>> scopes_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTableScopeGuard — RAII helper for automatic scope management
//
// Usage:
//   {
//       SymbolTableScopeGuard guard(symbolTable);
//       // ... declare symbols in the new scope ...
//   } // scope automatically popped
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTableScopeGuard {
public:
    explicit SymbolTableScopeGuard(SymbolTable& table) : table_(table) {
        table_.pushScope();
    }
    
    ~SymbolTableScopeGuard() {
        table_.popScope();
    }
    
    // Disable copy/move
    SymbolTableScopeGuard(const SymbolTableScopeGuard&) = delete;
    SymbolTableScopeGuard& operator=(const SymbolTableScopeGuard&) = delete;
    
private:
    SymbolTable& table_;
};