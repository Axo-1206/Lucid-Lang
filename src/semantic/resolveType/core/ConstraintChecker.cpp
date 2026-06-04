/**
 * @file ConstraintChecker.cpp
 * @brief Implementation of trait constraint satisfaction checking.
 */

#include "ConstraintChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

ConstraintChecker::ConstraintChecker(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("ConstraintChecker constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Data Management
// ─────────────────────────────────────────────────────────────────────────────

void ConstraintChecker::setStructTraits(
    const std::unordered_map<InternedString, std::vector<InternedString>>* map) {
    structTraits_ = map;
    LUC_LOG_SEMANTIC("ConstraintChecker::setStructTraits: map=" << (map ? "set" : "cleared"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Constraint Checking
// ─────────────────────────────────────────────────────────────────────────────

bool ConstraintChecker::satisfies(TypeAST* type, const std::vector<InternedString>& requiredTraits) const {
    // Empty constraint list is always satisfied
    if (requiredTraits.empty()) {
        return true;
    }
    
    if (!type) {
        return false;
    }
    
    // Unwrap type aliases to get to the underlying type
    TypeAST* underlying = unwrapAliases(type);
    
    // If the type is still a generic parameter (not yet substituted),
    // defer constraint checking to instantiation time.
    if (underlying->isa<NamedTypeAST>() && underlying->as<NamedTypeAST>()->isGenericParam) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: deferring check for generic parameter");
        return true;
    }
    
    // Only named types (structs, enums, aliased types) can satisfy constraints
    if (!underlying->isa<NamedTypeAST>()) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: type is not a named type");
        return false;
    }
    
    // Get the struct/trait mapping
    if (!structTraits_) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: no struct traits map available");
        return false;
    }
    
    InternedString typeName = getTypeName(underlying);
    auto it = structTraits_->find(typeName);
    if (it == structTraits_->end()) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: type '" << ctx_.pool.lookup(typeName) 
                         << "' has no implemented traits");
        return false;
    }
    
    const auto& implemented = it->second;
    
    // Check each required trait
    for (InternedString req : requiredTraits) {
        bool found = false;
        for (InternedString impl : implemented) {
            if (impl == req) {
                found = true;
                break;
            }
        }
        if (!found) {
            LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: type '" << ctx_.pool.lookup(typeName) 
                             << "' does not implement trait '" << ctx_.pool.lookup(req) << "'");
            return false;
        }
    }
    
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Classification
// ─────────────────────────────────────────────────────────────────────────────

bool ConstraintChecker::isValueType(TypeAST* type) const {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(type);
    
    switch (underlying->kind) {
        case ASTKind::PrimitiveType:
            return true;
            
        case ASTKind::NamedType: {
            auto* named = underlying->as<NamedTypeAST>();
            Symbol* sym = ctx_.symbols->lookup(named->name);
            if (!sym) return false;
            // Structs and enums are value types
            return (sym->kind == SymbolKind::Struct || sym->kind == SymbolKind::Enum);
        }
        
        case ASTKind::NullableType:
            return isValueType(underlying->as<NullableTypeAST>()->inner.get());
            
        case ASTKind::ArrayType:
            return true;  // Arrays are value types (they own their data or are views)
            
        case ASTKind::FuncType:
            return false;  // Function types are NOT value types (cannot be nullable directly)
            
        case ASTKind::RefType:
            return false;  // References are not value types (they're borrows)
            
        case ASTKind::PtrType:
            return true;   // Raw pointers can be nullable (they're just addresses)
            
        default:
            return false;
    }
}

bool ConstraintChecker::isStructType(TypeAST* type) const {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(type);
    
    if (!underlying->isa<NamedTypeAST>()) {
        return false;
    }
    
    auto* named = underlying->as<NamedTypeAST>();
    Symbol* sym = ctx_.symbols->lookup(named->name);
    if (!sym) return false;
    
    return sym->kind == SymbolKind::Struct;
}

bool ConstraintChecker::isEnumType(TypeAST* type) const {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(type);
    
    if (!underlying->isa<NamedTypeAST>()) {
        return false;
    }
    
    auto* named = underlying->as<NamedTypeAST>();
    Symbol* sym = ctx_.symbols->lookup(named->name);
    if (!sym) return false;
    
    return sym->kind == SymbolKind::Enum;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper Methods
// ─────────────────────────────────────────────────────────────────────────────

InternedString ConstraintChecker::getTypeName(TypeAST* type) const {
    if (!type) return InternedString();
    
    TypeAST* underlying = unwrapAliases(type);
    
    if (underlying->isa<NamedTypeAST>()) {
        return underlying->as<NamedTypeAST>()->name;
    }
    
    // For primitive types, we could return a special name, but constraints
    // are only relevant for structs/enums, so we return empty.
    return InternedString();
}

TypeAST* ConstraintChecker::unwrapAliases(TypeAST* type) const {
    if (!type) return nullptr;
    
    // Keep unwrapping while we have a named type that's a type alias
    while (type && type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        Symbol* sym = ctx_.symbols->lookup(named->name);
        
        // If not found or not a type alias, stop
        if (!sym || sym->kind != SymbolKind::TypeAlias) {
            break;
        }
        
        // Get the aliased type and continue unwrapping
        TypeAST* aliased = sym->type;
        if (!aliased) {
            break;
        }
        
        type = aliased;
    }
    
    return type;
}