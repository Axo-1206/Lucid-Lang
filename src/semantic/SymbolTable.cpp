#include "header/SymbolTable.hpp"
#include "ast/support/InternedString.hpp"
#include "debug/DebugMacros.hpp"
#include <algorithm>

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

const std::unordered_map<uint32_t, Symbol>& SymbolTable::getGlobalScope() const {
    if (scopes_.empty()) {
        LUC_LOG_SEMANTIC("SymbolTable::getGlobalScope: ERROR - no scopes exist");
        static std::unordered_map<uint32_t, Symbol> empty;
        return empty;
    }
    return scopes_.front();
}

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

std::vector<Symbol*> SymbolTable::findSymbolsByPrefix(InternedString prefix, const StringPool& pool) {
    std::string_view prefixStr = pool.lookup(prefix);
    return findSymbolsByPrefix(std::string(prefixStr), pool);
}

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