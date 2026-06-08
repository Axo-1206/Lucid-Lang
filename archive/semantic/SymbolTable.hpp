/**
 * @file SymbolTable.hpp
 * @brief Enhanced symbol table with separate namespaces for values and types.
 */

#pragma once

#include "SemanticSymbol.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <unordered_map>
#include <deque>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// Scope – a single lexical scope with separate maps for values and types
// ─────────────────────────────────────────────────────────────────────────────
struct Scope {
    // Value namespace: variables, functions, parameters, fields, methods
    std::unordered_map<uint32_t, Symbol> values;
    
    // Type namespace: structs, enums, traits, type aliases
    std::unordered_map<uint32_t, Symbol> types;
    
    // Overload sets: function name → overload set (only in value namespace)
    std::unordered_map<uint32_t, OverloadSet> overloads;
    
    // Imported modules (for use declarations)
    std::unordered_map<uint32_t, Symbol> imports;
    
    void clear() {
        values.clear();
        types.clear();
        overloads.clear();
        imports.clear();
    }
    
    size_t size() const { return values.size() + types.size() + imports.size(); }
    bool empty() const { return values.empty() && types.empty() && imports.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// LookupContext – controls which namespaces are searched
// ─────────────────────────────────────────────────────────────────────────────
enum class LookupContext {
    Value,      // Search only value namespace (variables, functions)
    Type,       // Search only type namespace (structs, enums, traits)
    Both,       // Search both (value first, then type)
    Import      // Search imports only
};

// ─────────────────────────────────────────────────────────────────────────────
// LookupResult – result of a symbol lookup
// ─────────────────────────────────────────────────────────────────────────────
struct LookupResult {
    Symbol* symbol = nullptr;
    LookupContext foundIn = LookupContext::Value;
    
    explicit operator bool() const { return symbol != nullptr; }
    Symbol* operator->() const { return symbol; }
    Symbol& operator*() const { return *symbol; }
};

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTable – enhanced with separate namespaces
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Scope Management
    // ─────────────────────────────────────────────────────────────────────────
    void pushScope();
    void popScope();
    int currentDepth() const { return static_cast<int>(scopes_.size()); }
    size_t scopeCount() const { return scopes_.size(); }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Declaration
    // ─────────────────────────────────────────────────────────────────────────
    
    // Declare a symbol (auto-detects namespace from SymbolKind)
    bool declare(const Symbol& sym);
    
    // Explicitly declare in value namespace
    bool declareValue(const Symbol& sym);
    
    // Explicitly declare in type namespace
    bool declareType(const Symbol& sym);
    
    // Declare an overloaded function (adds to overload set)
    bool declareOverload(const Symbol& sym);
    
    // Declare an import (use declaration)
    bool declareImport(const Symbol& sym);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Lookup
    // ─────────────────────────────────────────────────────────────────────────
    
    // General lookup – searches according to context
    LookupResult lookup(InternedString name, LookupContext ctx = LookupContext::Both);
    
    // Lookup only in value namespace
    Symbol* lookupValue(InternedString name);
    const Symbol* lookupValue(InternedString name) const;
    
    // Lookup only in type namespace
    Symbol* lookupType(InternedString name);
    const Symbol* lookupType(InternedString name) const;
    
    // Lookup only in current (innermost) scope
    Symbol* lookupLocalValue(InternedString name);
    Symbol* lookupLocalType(InternedString name);
    
    // Lookup overload set (returns nullptr if not overloaded)
    OverloadSet* lookupOverloads(InternedString name);
    const OverloadSet* lookupOverloads(InternedString name) const;
    
    // Lookup import
    Symbol* lookupImport(InternedString name);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Module/Import Management
    // ─────────────────────────────────────────────────────────────────────────
    
    // Import all symbols from another module into current scope
    void importModule(const SymbolTable& moduleTable, InternedString moduleAlias);
    
    // Get the global scope (for debugging/iteration)
    const Scope& getGlobalScope() const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Debugging
    // ─────────────────────────────────────────────────────────────────────────
    void dump(const StringPool& pool) const;
    std::string toString(const StringPool& pool) const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Iteration helpers
    // ─────────────────────────────────────────────────────────────────────────
    void forEachSymbol(std::function<void(const Symbol&)> callback) const;
    void forEachValue(std::function<void(const Symbol&)> callback) const;
    void foreachType(std::function<void(const Symbol&)> callback) const;
    
private:
    std::deque<Scope> scopes_;
    
    // Helper to find overload set in current or outer scopes
    OverloadSet* findOverloadSet(InternedString name);
    const OverloadSet* findOverloadSet(InternedString name) const;
};