/**
 * @file SymbolTable.cpp
 * @responsibility Implements lexical scoping for symbols during semantic analysis.
 *
 * This file implements the SymbolTable class: managing a stack of scopes,
 * declaring symbols, and looking them up from the innermost scope outward.
 * The backing container is a std::deque of unordered_maps, keyed by the
 * uint32_t ID of an InternedString for fast O(1) lookups.
 *
 * @related
 *   - SymbolTable.hpp – class declaration and public interface
 *   - SemanticCollector.cpp – populates the symbol table during Phase 1
 *   - SemanticAnalyzer.cpp – uses the table for type resolution and checking
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Scope Management
 *   pushScope()                         – creates a new empty scope on the stack
 *   popScope()                          – discards the innermost scope
 *
 * ██ Global Scope Access
 *   getGlobalScope()                    – returns const reference to the global scope
 *
 * ██ Symbol Declaration & Lookup
 *   declare()                           – adds a symbol to the current scope
 *   lookup()                            – searches all scopes (innermost first)
 *   lookupLocal()                       – searches only the current scope
 *   findSymbolsByPrefix()               – finds all symbols with a given prefix
 *
 * ██ Introspection & Debugging
 *   currentDepth()                      – returns the number of scopes on the stack
 *   dump()                              – prints the entire symbol table (requires StringPool)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note All scopes store symbols keyed by InternedString ID (uint32_t). This
 *       avoids string copies and gives O(1) equality checks. The symbol table
 *       does not own the AST nodes; only the arena does.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "header/SymbolTable.hpp"
#include "ast/support/InternedString.hpp"
#include "debug/DebugMacros.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Scope Management
// ─────────────────────────────────────────────────────────────────────────────

void SymbolTable::pushScope() {
    scopes_.emplace_back();
    LUC_LOG_SEMANTIC_VERBOSE("SymbolTable::pushScope: depth=" << scopes_.size());
}

void SymbolTable::popScope() {
    if (!scopes_.empty()) {
        LUC_LOG_SEMANTIC_VERBOSE("SymbolTable::popScope: depth=" << scopes_.size() - 1 
                                 << ", clearing " << scopes_.back().size() << " symbols");
        scopes_.pop_back();
    } else {
        LUC_LOG_SEMANTIC("SymbolTable::popScope: ERROR - no scope to pop");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Global Scope Access
// ─────────────────────────────────────────────────────────────────────────────

const std::unordered_map<uint32_t, Symbol>& SymbolTable::getGlobalScope() const {
    if (scopes_.empty()) {
        LUC_LOG_SEMANTIC("SymbolTable::getGlobalScope: ERROR - no scopes exist");
        static std::unordered_map<uint32_t, Symbol> empty;
        return empty;
    }
    return scopes_.front();
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Declaration & Lookup
// ─────────────────────────────────────────────────────────────────────────────

bool SymbolTable::declare(const Symbol& sym) {
    if (scopes_.empty()) {
        pushScope(); // Ensure at least global scope exists
    }
    
    auto& currentScope = scopes_.back();
    uint32_t id = sym.name.id;
    
    if (currentScope.find(id) != currentScope.end()) {
        LUC_LOG_SEMANTIC_VERBOSE("SymbolTable::declare: symbol '" 
                                 << sym.name.id << "' already exists in current scope");
        return false;
    }
    
    currentScope[id] = sym;
    LUC_LOG_SEMANTIC_EXTREME("SymbolTable::declare: added symbol id=" << id 
                             << " to scope depth=" << scopes_.size() - 1);
    return true;
}

Symbol* SymbolTable::lookup(InternedString name) {
    uint32_t id = name.id;
    
    // Search from innermost to outermost
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(id);
        if (found != it->end()) {
            LUC_LOG_SEMANTIC_EXTREME("SymbolTable::lookup: found id=" << id 
                                     << " at scope depth=" << (scopes_.size() - 1 - (it - scopes_.rbegin())));
            return &found->second;
        }
    }
    
    LUC_LOG_SEMANTIC_EXTREME("SymbolTable::lookup: id=" << id << " not found");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// lookupLocal (non‑const)
//
// Searches for a symbol only in the current (innermost) scope.
//
// @param name  The interned name of the symbol to look up.
// @return      A mutable pointer to the Symbol if found, nullptr otherwise.
//              The pointer is valid until the scope is popped.
// ─────────────────────────────────────────────────────────────────────────────
Symbol* SymbolTable::lookupLocal(InternedString name) {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& currentScope = scopes_.back();
    auto found = currentScope.find(id);
    
    if (found != currentScope.end()) {
        return &found->second;
    }
    
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// lookupLocal (const)
//
// Const version of lookupLocal. Searches only the current scope.
//
// @param name  The interned name of the symbol to look up.
// @return      A const pointer to the Symbol if found, nullptr otherwise.
// ─────────────────────────────────────────────────────────────────────────────
const Symbol* SymbolTable::lookupLocal(InternedString name) const {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& currentScope = scopes_.back();
    auto found = currentScope.find(id);
    
    if (found != currentScope.end()) {
        return &found->second;
    }
    
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// findSymbolsByPrefix (InternedString overload)
//
// Finds all symbols whose names start with the given interned prefix.
// Searches from innermost to outermost scope; if the same symbol appears in
// multiple scopes, only the innermost one is returned (shadowing handled).
//
// @param prefix  The interned prefix to match (e.g., "Vec2.from.").
// @param pool    StringPool to convert InternedString IDs to string_view.
// @return        Vector of mutable pointers to matching symbols (may be empty).
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Symbol*> SymbolTable::findSymbolsByPrefix(InternedString prefix, const StringPool& pool) {
    std::string_view prefixStr = pool.lookup(prefix);
    return findSymbolsByPrefix(std::string(prefixStr), pool);
}

// ─────────────────────────────────────────────────────────────────────────────
// findSymbolsByPrefix (std::string overload)
//
// Convenience overload that accepts a plain string prefix.
// Internally converts the string to a prefix and performs the same matching.
//
// @param prefix  The prefix string to match (e.g., "Vec2.from.").
// @param pool    StringPool to convert InternedString IDs to string_view.
// @return        Vector of mutable pointers to matching symbols (may be empty).
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Symbol*> SymbolTable::findSymbolsByPrefix(const std::string& prefix, const StringPool& pool) {
    std::vector<Symbol*> results;
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        for (auto& [id, sym] : *it) {
            std::string_view name = pool.lookup(InternedString(id));
            if (name.length() >= prefix.length() && name.substr(0, prefix.length()) == prefix) {
                bool shadowed = false;
                for (auto* r : results) {
                    if (r->name.id == id) { shadowed = true; break; }
                }
                if (!shadowed) {
                    results.push_back(&sym);
                }
            }
        }
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Introspection & Debugging
// ─────────────────────────────────────────────────────────────────────────────

int SymbolTable::currentDepth() const {
    return static_cast<int>(scopes_.size());
}

void SymbolTable::dump(const StringPool& pool) const {
    LUC_LOG_SEMANTIC("=== SymbolTable Dump (depth=" << scopes_.size() << ") ===");
    
    for (size_t i = 0; i < scopes_.size(); ++i) {
        LUC_LOG_SEMANTIC("  Scope[" << i << "] (" << scopes_[i].size() << " symbols):");
        for (const auto& pair : scopes_[i]) {
            const Symbol& sym = pair.second;
            std::string_view name = pool.lookup(sym.name);
            LUC_LOG_SEMANTIC("    - " << name << " (id=" << sym.name.id 
                             << ", kind=" << SymbolUtils::kindToString(sym.kind) << ")");
        }
    }
    LUC_LOG_SEMANTIC("=== End SymbolTable Dump ===");
}