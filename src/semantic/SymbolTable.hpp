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
#include <vector>
#include <unordered_map>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTable  — Manages the scope stack for symbol declarations
//
// A stack of hash maps ensuring proper visibility of variables and types.
// On entering a block, function, or trait, a new scope is pushed. On exit,
// it is popped. Variable lookups traverse from the innermost scope outwards,
// matching the standard lexical scoping rules.
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    void pushScope();
    void popScope();
    bool declare(const Symbol& sym);             // false = already declared in this scope
    Symbol* lookup(const std::string& name);     // walks scope stack outward
    Symbol* lookupLocal(const std::string& name);// current scope only
    std::vector<Symbol*> findSymbolsByPrefix(const std::string& prefix);

    int currentDepth() const;
    void dump() const;


    const std::unordered_map<std::string, Symbol>& getGlobalScope() const {
        static std::unordered_map<std::string, Symbol> empty;
        return scopes_.empty() ? empty : scopes_[0];
    }
private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};
