/**
 * @file SymbolTable.cpp
 *
 * @nutshell Implementation of the lexical scope traversing logic.
 *
 * @reason Variable shadowing and boundary blocks require complex map stacking behaviors. This file securely handles memory indexing and lookup mechanics behind the scenes.
 *
 * @responsibility Implementation of the scope stack for symbol declarations and lookups.
 *
 * @logic Manages pushed/popped scopes and outward resolution of symbols across depth levels.
 *
 * @related SymbolTable.hpp, SemanticSymbol.hpp
 */

#include "SymbolTable.hpp"
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// pushScope  — Pushes a newly created local scope
//
// Appends a new empty hash map to the end of the scope stack. All subsequent
// declarations will be bound to this top-most scope level. Commonly called
// when processing open-braces of function bodies or code blocks.
// ─────────────────────────────────────────────────────────────────────────────
void SymbolTable::pushScope() {
    scopes_.emplace_back();
}

// ─────────────────────────────────────────────────────────────────────────────
// popScope  — Removes the top-most local scope
//
// Eliminates the inner-most scope map off the stack, effectively un-binding
// any symbols declared within it. Invoked when concluding the semantic
// analysis of a syntactic block or function definition.
// ─────────────────────────────────────────────────────────────────────────────
void SymbolTable::popScope() {
    if (!scopes_.empty()) {
        scopes_.pop_back();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// declare  — Registers a symbol in the current scope
//
// Ensures a scope is initialized if none exists. Checks if the symbol name
// already resides in the innermost scope. If not, it safely inserts it. 
// This strict insertion prevents redeclarations of the exact same identifier 
// within a shared block level.
// ─────────────────────────────────────────────────────────────────────────────
bool SymbolTable::declare(const Symbol& sym) {
    if (scopes_.empty()) pushScope();
    auto& currentScope = scopes_.back();
    if (currentScope.find(sym.name) != currentScope.end()) {
        return false; // Already declared in this scope
    }
    currentScope[sym.name] = sym;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// lookup  — Resolves a symbol across all visible scopes
//
// Iteratively traverses the scope maps backwards (innermost outward). 
// Yields the first matched internal symbol pointer, mimicking standard lexical 
// shadowing visibility rules. Returns nullptr when reaching past the global 
// scope without finding a symbol.
// ─────────────────────────────────────────────────────────────────────────────
Symbol* SymbolTable::lookup(const std::string& name) {
    for (int i = (int)scopes_.size() - 1; i >= 0; --i) {
        const auto& scope = scopes_[i];
        auto found = scope.find(name);
        if (found != scope.end()) {
            return const_cast<Symbol*>(&found->second);
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// lookupLocal  — Looks up a symbol strictly in the current block
//
// Only queries the top-most (most recent) scope for the symbol. Often utilized
// to probe if an identifier attempts to shadow another locally, prior to
// finalizing a cross-scope lookup. Returns nullptr if the symbol is absent.
// ─────────────────────────────────────────────────────────────────────────────
Symbol* SymbolTable::lookupLocal(const std::string& name) {
    if (scopes_.empty()) return nullptr;
    auto& currentScope = scopes_.back();
    auto found = currentScope.find(name);
    if (found != currentScope.end()) {
        return &found->second;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// currentDepth  — Retrieves the current nesting distance
//
// Returns the raw number of scopes currently residing on the stack. Highly 
// useful for stamping exact declaration scope depths dynamically onto AST 
// elements during traversal and code generation.
// ─────────────────────────────────────────────────────────────────────────────
int SymbolTable::currentDepth() const {
    return static_cast<int>(scopes_.size());
}

#include <iostream>
#include <iomanip>

static std::string kindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Var:         return "Var";
        case SymbolKind::Func:        return "Func";
        case SymbolKind::Struct:      return "Struct";
        case SymbolKind::Enum:        return "Enum";
        case SymbolKind::Trait:       return "Trait";
        case SymbolKind::TypeAlias:   return "TypeAlias";
        case SymbolKind::Param:       return "Param";
        case SymbolKind::Field:       return "Field";
        case SymbolKind::Method:      return "Method";
        case SymbolKind::EnumVariant: return "EnumVariant";
        case SymbolKind::Conversion:  return "Conversion";
        default:                      return "Unknown";
    }
}

void SymbolTable::dump() const {
    std::cout << "\n--- SYMBOL TABLE DUMP ---" << std::endl;
    if (scopes_.empty()) {
        std::cout << " (empty)" << std::endl;
        return;
    }

    for (int i = 0; i < scopes_.size(); ++i) {
        std::cout << "Scope Depth " << i << ":" << std::endl;
        const auto& scope = scopes_[i];
        if (scope.empty()) {
            std::cout << "  (empty)" << std::endl;
            continue;
        }

        for (const auto& [name, sym] : scope) {
            std::cout << "  - " << std::left << std::setw(15) << name 
                      << " [" << kindToString(sym.kind) << "]";
            if (sym.loc.isKnown()) {
                std::cout << " at " << sym.loc.file << ":" << sym.loc.line;
            }
            std::cout << std::endl;
        }
    }
    std::cout << "-------------------------\n" << std::endl;
}
