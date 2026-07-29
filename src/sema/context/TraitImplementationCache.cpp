/// @file TraitImplementationCache.cpp
/// @brief Implementation of TraitImplementationCache.

#include "TraitImplementationCache.hpp"
#include "SemaContext.hpp"
#include "sema/types/SemaType.hpp"

#include <algorithm>

namespace sema {

// ─── Registration ──────────────────────────────────────────────────────────

bool TraitImplementationCache::addImplementation(
    const StructDeclAST* structDecl,
    const TraitDeclAST* traitDecl) {
    
    if (!structDecl || !traitDecl) return false;

    // Validate that the struct correctly implements the trait
    if (!validateTraitImplementation(structDecl, traitDecl)) {
        return false;
    }

    // Check if already registered (avoid duplicates)
    auto it = m_structToTraits.find(structDecl);
    if (it != m_structToTraits.end()) {
        for (const TraitDeclAST* existing : it->second) {
            if (existing == traitDecl) {
                return false; // Already registered
            }
        }
    }

    // Register in struct → traits map
    m_structToTraits[structDecl].push_back(traitDecl);

    // Register in trait → structs map
    m_traitToStructs[traitDecl].insert(structDecl);

    return true;
}

// ─── Queries ──────────────────────────────────────────────────────────────

bool TraitImplementationCache::implements(
    const StructDeclAST* structDecl,
    const TraitDeclAST* traitDecl) const {
    
    if (!structDecl || !traitDecl) return false;

    auto it = m_structToTraits.find(structDecl);
    if (it == m_structToTraits.end()) {
        return false;
    }

    for (const TraitDeclAST* t : it->second) {
        if (t == traitDecl) {
            return true;
        }
    }

    return false;
}

bool TraitImplementationCache::implements(
    const TypeDeclAST* typeDecl,
    const TraitDeclAST* traitDecl) const {
    
    if (!typeDecl || !traitDecl) return false;

    // Only struct types can implement traits
    if (!typeDecl->isa<StructDeclAST>()) {
        return false;
    }

    return implements(typeDecl->as<StructDeclAST>(), traitDecl);
}

const std::vector<const TraitDeclAST*>& 
TraitImplementationCache::getTraitsForStruct(
    const StructDeclAST* structDecl) const {
    
    static const std::vector<const TraitDeclAST*> empty;
    
    if (!structDecl) return empty;

    auto it = m_structToTraits.find(structDecl);
    if (it == m_structToTraits.end()) {
        return empty;
    }

    return it->second;
}

std::vector<const StructDeclAST*> 
TraitImplementationCache::getStructsForTrait(
    const TraitDeclAST* traitDecl) const {
    
    std::vector<const StructDeclAST*> result;

    if (!traitDecl) return result;

    auto it = m_traitToStructs.find(traitDecl);
    if (it == m_traitToStructs.end()) {
        return result;
    }

    result.reserve(it->second.size());
    for (const StructDeclAST* s : it->second) {
        result.push_back(s);
    }

    return result;
}

// ─── Debug / Inspection ──────────────────────────────────────────────────

void TraitImplementationCache::clear() {
    m_structToTraits.clear();
    m_traitToStructs.clear();
}

bool TraitImplementationCache::hasImplementations(
    const StructDeclAST* structDecl) const {
    
    if (!structDecl) return false;
    return m_structToTraits.find(structDecl) != m_structToTraits.end();
}

size_t TraitImplementationCache::implementationCount() const {
    size_t count = 0;
    for (const auto& pair : m_structToTraits) {
        count += pair.second.size();
    }
    return count;
}

// ─── Helper Functions ──────────────────────────────────────────────────

bool TraitImplementationCache::validateTraitImplementation(
    const StructDeclAST* structDecl,
    const TraitDeclAST* traitDecl) const {
    
    // This is a validation check that should be performed during struct analysis.
    // We return true here because the actual validation is done in SemaDecl.cpp.
    // The cache only stores validated implementations.
    //
    // The validation in SemaDecl.cpp checks:
    //   1. All trait fields exist in the struct
    //   2. Field types match exactly
    //   3. Const-ness matches (trait const → struct const required)
    //
    // If validation fails, addImplementation() returns false.
    
    return true;
}

} // namespace sema