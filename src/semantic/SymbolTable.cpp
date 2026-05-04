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
#include "debug/DebugMacros.hpp"
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// pushScope  — Pushes a newly created local scope
//
// Appends a new empty hash map to the end of the scope stack. All subsequent
// declarations will be bound to this top-most scope level. Commonly called
// when processing open-braces of function bodies or code blocks.
// ─────────────────────────────────────────────────────────────────────────────
void SymbolTable::pushScope() {
    LUC_LOG_SEMANTIC_VERBOSE("pushScope: current depth=" << scopes_.size() << " -> " << (scopes_.size() + 1));
    
    // Pre-reserve capacity to prevent reallocation
    if (scopes_.capacity() == scopes_.size()) {
        size_t newCapacity = scopes_.size() + 32;
    LUC_LOG_SEMANTIC_EXTREME("\treserving capacity: " << newCapacity);
        scopes_.reserve(newCapacity);
    }
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
        size_t oldSize = scopes_.size();
    LUC_LOG_SEMANTIC_VERBOSE("popScope: depth " << oldSize << " -> " << (oldSize - 1));
        scopes_.pop_back();
    } else {
    LUC_LOG_SEMANTIC("popScope: WARNING - attempt to pop empty scope stack");
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
  LUC_LOG_SEMANTIC_VERBOSE("declare: name='" << sym.name 
                           << "', kind=" << static_cast<int>(sym.kind));
    
    if (scopes_.empty()) {
    LUC_LOG_SEMANTIC_EXTREME("\tscopes empty, pushing new scope");
        pushScope();
    }
    
    auto& currentScope = scopes_.back();
    auto it = currentScope.find(sym.name);
    
    if (it != currentScope.end()) {
    LUC_LOG_SEMANTIC("\tERROR: symbol '" << sym.name << "' already exists in current scope");
        return false;
    }
    
    currentScope[sym.name] = sym;
  LUC_LOG_SEMANTIC_EXTREME("\tsymbol declared successfully, scope size=" << currentScope.size());
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
  LUC_LOG_SEMANTIC_VERBOSE("lookup: name='" << name << "', depth=" << scopes_.size());
    
    for (int i = (int)scopes_.size() - 1; i >= 0; --i) {
        const auto& scope = scopes_[i];
        auto found = scope.find(name);
        if (found != scope.end()) {
            LUC_LOG_SEMANTIC_EXTREME("\tfound at scope depth " << i << ", kind=" << static_cast<int>(found->second.kind));
            return const_cast<Symbol*>(&found->second);
        }
    LUC_LOG_SEMANTIC_EXTREME("\tnot found at scope depth " << i);
    }
    
  LUC_LOG_SEMANTIC_VERBOSE("\tsymbol '" << name << "' not found in any scope");
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
  LUC_LOG_SEMANTIC_EXTREME("lookupLocal: name='" << name << "'");
    
    if (scopes_.empty()) {
    LUC_LOG_SEMANTIC_EXTREME("\tscopes empty, returning nullptr");
        return nullptr;
    }
    
    auto& currentScope = scopes_.back();
    auto found = currentScope.find(name);
    if (found != currentScope.end()) {
    LUC_LOG_SEMANTIC_EXTREME("\tfound in current scope");
        return &found->second;
    }
    
  LUC_LOG_SEMANTIC_EXTREME("\tnot found in current scope");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// findSymbolsByPrefix  — Retrieves all symbols whose name starts with a prefix
//
// Scans every scope from outermost to innermost, collecting every symbol whose
// name begins with the given prefix string. Used by the from-casting lookup to
// find all registered "TargetType.from.*" entries without knowing the exact
// mangled name generated from the pointer address in SemanticCollector.
//
// Returns raw pointers into the scope maps — callers must not push/pop scopes
// while holding these pointers.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Symbol*> SymbolTable::findSymbolsByPrefix(const std::string& prefix) {
  LUC_LOG_SEMANTIC_VERBOSE("findSymbolsByPrefix: prefix='" << prefix << "'");
    
    std::vector<Symbol*> results;
    int totalScopes = 0;
    int matchesFound = 0;
    
    for (auto& scope : scopes_) {
        totalScopes++;
        for (auto& [name, sym] : scope) {
            if (name.size() >= prefix.size() &&
                name.compare(0, prefix.size(), prefix) == 0) {
                results.push_back(&sym);
                matchesFound++;
        LUC_LOG_SEMANTIC_EXTREME("\tmatched: " << name);
            }
        }
    }
    
  LUC_LOG_SEMANTIC_VERBOSE("\tfound " << matchesFound << " matches in " << totalScopes << " scopes");
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// currentDepth  — Retrieves the current nesting distance
//
// Returns the raw number of scopes currently residing on the stack. Highly 
// useful for stamping exact declaration scope depths dynamically onto AST 
// elements during traversal and code generation.
// ─────────────────────────────────────────────────────────────────────────────
int SymbolTable::currentDepth() const {
    int depth = static_cast<int>(scopes_.size());
  LUC_LOG_SEMANTIC_EXTREME("currentDepth: " << depth);
    return depth;
}

// ─────────────────────────────────────────────────────────────────────────────
// kindToString  — Helper for symbol table dump
// ─────────────────────────────────────────────────────────────────────────────

static std::string kindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Var:         return "Var";
        case SymbolKind::Func:        return "Func";
        case SymbolKind::ExternFunc:  return "ExternFunc";
        case SymbolKind::Struct:      return "Struct";
        case SymbolKind::Enum:        return "Enum";
        case SymbolKind::Trait:       return "Trait";
        case SymbolKind::TypeAlias:   return "TypeAlias";
        case SymbolKind::Param:       return "Param";
        case SymbolKind::Field:       return "Field";
        case SymbolKind::Method:      return "Method";
        case SymbolKind::EnumVariant: return "EnumVariant";
        case SymbolKind::Casting:     return "Casting";
        default:                      return "Unknown";
    }
}

void SymbolTable::dump() const {
  LUC_LOG_SEMANTIC("=== SYMBOL TABLE DUMP ===");
    
    std::cout << "\n--- SYMBOL TABLE DUMP ---" << std::endl;
    if (scopes_.empty()) {
        std::cout << " (empty)" << std::endl;
    LUC_LOG_SEMANTIC("\t(empty)");
        return;
    }

    int totalSymbols = 0;
    for (int i = 0; i < scopes_.size(); ++i) {
        std::cout << "Scope Depth " << i << ":" << std::endl;
    LUC_LOG_SEMANTIC_EXTREME("Scope Depth " << i << ":");
        
        const auto& scope = scopes_[i];
        if (scope.empty()) {
            std::cout << "  (empty)" << std::endl;
            continue;
        }

        for (const auto& [name, sym] : scope) {
            totalSymbols++;
            std::string kindStr = "[" + kindToString(sym.kind) + "]";
            
            std::cout << "  - " 
                    << std::left << std::setw(35) << name
                    << std::left << std::setw(14) << kindStr;

            if (sym.loc.isKnown()) {
                std::cout << " : " << sym.loc.file << ":" << sym.loc.line;
            }
            // Show extern binding info for linker-resolved symbols.
            if (sym.isExtern) {
                std::cout << "  [@extern(\"" << sym.externSymbol << "\", \""
                          << sym.callingConv << "\")]";
            }
            
            std::cout << std::endl;
            
      LUC_LOG_SEMANTIC_EXTREME("\t\t" << name << " " << kindStr);
        }
    }
    std::cout << "-------------------------\n" << std::endl;
    
  LUC_LOG_SEMANTIC("Total symbols: " << totalSymbols);
}