/**
 * @file ScopeStack.cpp
 * @brief Implementation of ScopeStack for AST-node-only name resolution.
 */

#include "ScopeStack.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

#include <sstream>
#include <algorithm>

namespace luc {

// ============================================================================
// Scope Management
// ============================================================================

void ScopeStack::push() {
    scopes_.emplace_back();
    LUC_LOG_SEMANTIC_EXTREME("ScopeStack::push: depth=" << scopes_.size());
}

void ScopeStack::pop() {
    if (!scopes_.empty()) {
        size_t poppedSize = scopes_.back().size();
        LUC_LOG_SEMANTIC_EXTREME("ScopeStack::pop: depth=" << scopes_.size() - 1 
                                 << ", cleared " << poppedSize << " symbols");
        scopes_.pop_back();
    } else {
        LUC_LOG_SEMANTIC("ScopeStack::pop: ERROR - no scope to pop");
    }
}

Scope& ScopeStack::current() {
    // Ensure at least one scope exists
    if (scopes_.empty()) {
        push();
    }
    return scopes_.back();
}

const Scope& ScopeStack::current() const {
    // If empty, return a static empty scope (caller should check depth first)
    static Scope emptyScope;
    if (scopes_.empty()) {
        return emptyScope;
    }
    return scopes_.back();
}

const Scope& ScopeStack::getGlobalScope() const {
    if (scopes_.empty()) {
        static Scope emptyScope;
        LUC_LOG_SEMANTIC("ScopeStack::getGlobalScope: no scopes exist");
        return emptyScope;
    }
    return scopes_.front();
}

// ============================================================================
// Declaration (current scope only)
// ============================================================================

bool ScopeStack::declareValue(ValueDeclAST* decl) {
    if (!decl) return false;
    
    auto& scope = current();
    uint32_t id = decl->name.id;
    
    // Check for duplicate in current scope's value namespace
    if (scope.values.find(id) != scope.values.end()) {
        LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareValue: '" 
                                 << decl->name.id << "' already exists in current scope");
        return false;
    }
    
    // Store the declaration
    scope.values[id] = decl;
    LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareValue: added " << decl->name.id 
                             << " to scope depth=" << scopes_.size() - 1);
    return true;
}

bool ScopeStack::declareType(TypeDeclAST* decl) {
    if (!decl) return false;
    
    auto& scope = current();
    uint32_t id = decl->name.id;
    
    // Check for duplicate in current scope's type namespace
    if (scope.types.find(id) != scope.types.end()) {
        LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareType: '" 
                                 << decl->name.id << "' already exists in current scope");
        return false;
    }
    
    // Store the declaration
    scope.types[id] = decl;
    LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareType: added " << decl->name.id 
                             << " to scope depth=" << scopes_.size() - 1);
    return true;
}

bool ScopeStack::declareOverload(FuncDeclAST* func) {
    if (!func) return false;
    
    uint32_t id = func->name.id;
    
    // First, check if there's already an overload set for this name in any scope
    std::vector<FuncDeclAST*>* existingSet = findOverloadSet(func->name);
    
    if (existingSet) {
        // Check for duplicate signature in the existing overload set
        if (hasConflictingSignature(func, *existingSet)) {
            LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareOverload: duplicate signature for '" 
                                     << func->name.id << "'");
            return false;
        }
        // Add to existing set
        existingSet->push_back(func);
        LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareOverload: added to existing overload set for '"
                                 << func->name.id << "' (now " << existingSet->size() << " candidates)");
    } else {
        // First function with this name – create a new overload set in current scope
        auto& scope = current();
        
        // Check if there's a non-function value with this name in current scope
        if (scope.values.find(id) != scope.values.end()) {
            LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareOverload: non-function value with name '"
                                     << func->name.id << "' already exists");
            return false;
        }
        
        // Create new overload set
        scope.overloads[id] = {func};
        LUC_LOG_SEMANTIC_EXTREME("ScopeStack::declareOverload: created new overload set for '"
                                 << func->name.id << "'");
    }
    
    // Also store in value namespace for regular lookup
    // Note: This will be overwritten if multiple overloads exist, but that's fine
    // because the overload set is the source of truth for overloaded functions.
    current().values[id] = func;
    
    return true;
}

// ============================================================================
// Lookup (outward search)
// ============================================================================

ValueDeclAST* ScopeStack::lookupValue(InternedString name) {
    uint32_t id = name.id;
    
    // Search from innermost to outermost
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->values.find(id);
        if (found != it->values.end()) {
            LUC_LOG_SEMANTIC_EXTREME("ScopeStack::lookupValue: found " << id 
                                     << " at depth=" << (scopes_.size() - 1 - (it - scopes_.rbegin())));
            return found->second;
        }
    }
    
    LUC_LOG_SEMANTIC_EXTREME("ScopeStack::lookupValue: " << id << " not found");
    return nullptr;
}

const ValueDeclAST* ScopeStack::lookupValue(InternedString name) const {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->values.find(id);
        if (found != it->values.end()) {
            return found->second;
        }
    }
    
    return nullptr;
}

TypeDeclAST* ScopeStack::lookupType(InternedString name) {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->types.find(id);
        if (found != it->types.end()) {
            LUC_LOG_SEMANTIC_EXTREME("ScopeStack::lookupType: found " << id 
                                     << " at depth=" << (scopes_.size() - 1 - (it - scopes_.rbegin())));
            return found->second;
        }
    }
    
    LUC_LOG_SEMANTIC_EXTREME("ScopeStack::lookupType: " << id << " not found");
    return nullptr;
}

const TypeDeclAST* ScopeStack::lookupType(InternedString name) const {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->types.find(id);
        if (found != it->types.end()) {
            return found->second;
        }
    }
    
    return nullptr;
}

std::vector<FuncDeclAST*>* ScopeStack::lookupOverloads(InternedString name) {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->overloads.find(id);
        if (found != it->overloads.end()) {
            LUC_LOG_SEMANTIC_EXTREME("ScopeStack::lookupOverloads: found " << id 
                                     << " with " << found->second.size() << " candidates");
            return &found->second;
        }
    }
    
    return nullptr;
}

const std::vector<FuncDeclAST*>* ScopeStack::lookupOverloads(InternedString name) const {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->overloads.find(id);
        if (found != it->overloads.end()) {
            return &found->second;
        }
    }
    
    return nullptr;
}

// ============================================================================
// Local Lookup (current scope only)
// ============================================================================

ValueDeclAST* ScopeStack::lookupLocalValue(InternedString name) {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& scope = scopes_.back();
    auto found = scope.values.find(id);
    
    if (found != scope.values.end()) {
        return found->second;
    }
    
    return nullptr;
}

const ValueDeclAST* ScopeStack::lookupLocalValue(InternedString name) const {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& scope = scopes_.back();
    auto found = scope.values.find(id);
    
    if (found != scope.values.end()) {
        return found->second;
    }
    
    return nullptr;
}

TypeDeclAST* ScopeStack::lookupLocalType(InternedString name) {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& scope = scopes_.back();
    auto found = scope.types.find(id);
    
    if (found != scope.types.end()) {
        return found->second;
    }
    
    return nullptr;
}

const TypeDeclAST* ScopeStack::lookupLocalType(InternedString name) const {
    if (scopes_.empty()) return nullptr;
    
    uint32_t id = name.id;
    auto& scope = scopes_.back();
    auto found = scope.types.find(id);
    
    if (found != scope.types.end()) {
        return found->second;
    }
    
    return nullptr;
}

// ============================================================================
// Private Helpers
// ============================================================================

std::vector<FuncDeclAST*>* ScopeStack::findOverloadSet(InternedString name) {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->overloads.find(id);
        if (found != it->overloads.end()) {
            return &found->second;
        }
    }
    
    return nullptr;
}

const std::vector<FuncDeclAST*>* ScopeStack::findOverloadSet(InternedString name) const {
    uint32_t id = name.id;
    
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->overloads.find(id);
        if (found != it->overloads.end()) {
            return &found->second;
        }
    }
    
    return nullptr;
}

bool ScopeStack::signaturesEqual(FuncDeclAST* a, FuncDeclAST* b) const {
    if (!a || !b) return false;
    if (!a->funcType || !b->funcType) return false;
    
    const auto& paramsA = a->funcType->params;
    const auto& paramsB = b->funcType->params;
    
    if (paramsA.size() != paramsB.size()) return false;
    
    for (size_t i = 0; i < paramsA.size(); ++i) {
        TypeAST* typeA = paramsA[i]->type;
        TypeAST* typeB = paramsB[i]->type;
        
        // Simple pointer comparison – types are resolved during Phase 2
        // For now, just compare pointers (aliases should be resolved already)
        if (typeA != typeB) return false;
    }
    
    return true;
}

bool ScopeStack::hasConflictingSignature(FuncDeclAST* func, const std::vector<FuncDeclAST*>& overloads) const {
    for (const auto* existing : overloads) {
        if (signaturesEqual(func, const_cast<FuncDeclAST*>(existing))) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Debugging
// ============================================================================

void ScopeStack::dump(const StringPool& pool) const {
    LUC_LOG_SEMANTIC("=== ScopeStack Dump (depth=" << scopes_.size() << ") ===");
    LUC_LOG_SEMANTIC(toString(pool));
    LUC_LOG_SEMANTIC("=== End ScopeStack Dump ===");
}

std::string ScopeStack::toString(const StringPool& pool) const {
    std::stringstream ss;
    
    for (size_t i = 0; i < scopes_.size(); ++i) {
        const auto& scope = scopes_[i];
        ss << "Scope[" << i << "] (depth=" << i << "):\n";
        
        // Value namespace
        if (!scope.values.empty()) {
            ss << "  Values:\n";
            for (const auto& [id, decl] : scope.values) {
                std::string_view name = pool.lookup(InternedString(id));
                ss << "    - " << name << " (id=" << id 
                   << ", kind=" << LucDebug::kindToString(decl->kind) << ")\n";
            }
        }
        
        // Type namespace
        if (!scope.types.empty()) {
            ss << "  Types:\n";
            for (const auto& [id, decl] : scope.types) {
                std::string_view name = pool.lookup(InternedString(id));
                ss << "    - " << name << " (id=" << id 
                   << ", kind=" << LucDebug::kindToString(decl->kind) << ")\n";
            }
        }
        
        // Overload sets
        if (!scope.overloads.empty()) {
            ss << "  Overloads:\n";
            for (const auto& [id, overloads] : scope.overloads) {
                std::string_view name = pool.lookup(InternedString(id));
                ss << "    - " << name << " (id=" << id 
                   << ", " << overloads.size() << " overloads)\n";
            }
        }
    }
    
    return ss.str();
}

} // namespace luc