/**
 * @file SemanticContext.cpp
 * @brief Implementation of SemanticContext methods.
 */

#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "ast/TypeAST.hpp"

bool SemanticContext::implementsTrait(TypeAST* type, InternedString traitName) const {
    if (!type) return false;

    // Get canonical mangled key for this type
    InternedString key = pool.intern(NameMangler::mangleType(type, pool, symbols));

    auto it = typeTraits.find(key);
    if (it == typeTraits.end()) return false;

    for (InternedString t : it->second) {
        if (t == traitName) return true;
    }
    return false;
}