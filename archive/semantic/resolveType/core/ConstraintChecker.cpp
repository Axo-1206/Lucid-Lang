/**
 * @file ConstraintChecker.cpp
 * @brief Implementation of trait constraint satisfaction checking.
 */

#include "ConstraintChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugMacros.hpp"

namespace ConstraintChecker {

// ─────────────────────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────────────────────

InternedString getTypeName(SemanticContext& ctx, TypeAST* type) {
    if (!type) return InternedString();
    
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    if (underlying->isa<NamedTypeAST>()) {
        return underlying->as<NamedTypeAST>()->name;
    }
    
    return InternedString();
}

TypeAST* unwrapAliases(SemanticContext& ctx, TypeAST* type) {
    if (!type) return nullptr;
    
    while (type && type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        Symbol* sym = ctx.symbols->lookup(named->name);
        
        if (!sym || sym->kind != SymbolKind::TypeAlias) {
            break;
        }
        
        TypeAST* aliased = sym->type;
        if (!aliased) {
            break;
        }
        
        type = aliased;
    }
    
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Constraint Checking
// ─────────────────────────────────────────────────────────────────────────────

bool satisfies(SemanticContext& ctx,
               TypeAST* type, 
               const std::vector<InternedString>& requiredTraits) {
    
    if (requiredTraits.empty()) {
        return true;
    }
    
    if (!type) {
        return false;
    }
    
    // Unwrap type aliases
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    // If still a generic parameter, defer to instantiation time
    if (underlying->isa<NamedTypeAST>() && underlying->as<NamedTypeAST>()->isGenericParam) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: deferring check for generic parameter");
        return true;
    }
    
    // Get canonical mangled key for this type
    InternedString key = ctx.pool.intern(NameMangler::mangleType(underlying, ctx.pool, ctx.symbols));
    
    // Look up in typeTraits map
    auto it = ctx.typeTraits.find(key);
    if (it == ctx.typeTraits.end()) {
        LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: type '" 
                         << ctx.pool.lookup(key) << "' has no implemented traits");
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
            LUC_LOG_SEMANTIC("ConstraintChecker::satisfies: type '" << ctx.pool.lookup(key) 
                             << "' does not implement trait '" << ctx.pool.lookup(req) << "'");
            return false;
        }
    }
    
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Classification
// ─────────────────────────────────────────────────────────────────────────────

bool isValueType(SemanticContext& ctx, TypeAST* type) {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    switch (underlying->kind) {
        case ASTKind::PrimitiveType:
            return true;
            
        case ASTKind::NamedType: {
            auto* named = underlying->as<NamedTypeAST>();
            Symbol* sym = ctx.symbols->lookup(named->name);
            if (!sym) return false;
            return (sym->kind == SymbolKind::Struct || sym->kind == SymbolKind::Enum);
        }
        
        case ASTKind::NullableType:
            return isValueType(ctx, underlying->as<NullableTypeAST>()->inner.get());
            
        case ASTKind::ArrayType:
            return true;
            
        case ASTKind::FuncType:
            return false;
            
        case ASTKind::RefType:
            return false;
            
        case ASTKind::PtrType:
            return true;
            
        default:
            return false;
    }
}

bool isStructType(SemanticContext& ctx, TypeAST* type) {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    if (!underlying->isa<NamedTypeAST>()) {
        return false;
    }
    
    auto* named = underlying->as<NamedTypeAST>();
    Symbol* sym = ctx.symbols->lookup(named->name);
    if (!sym) return false;
    
    return sym->kind == SymbolKind::Struct;
}

bool isEnumType(SemanticContext& ctx, TypeAST* type) {
    if (!type) return false;
    
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    if (!underlying->isa<NamedTypeAST>()) {
        return false;
    }
    
    auto* named = underlying->as<NamedTypeAST>();
    Symbol* sym = ctx.symbols->lookup(named->name);
    if (!sym) return false;
    
    return sym->kind == SymbolKind::Enum;
}

bool isFunctionType(SemanticContext& /*ctx*/, TypeAST* type) {
    if (!type) return false;
    return type->isa<FuncTypeAST>();
}

bool isReferenceType(SemanticContext& /*ctx*/, TypeAST* type) {
    if (!type) return false;
    return type->isa<RefTypeAST>();
}

bool isArrayType(SemanticContext& /*ctx*/, TypeAST* type) {
    if (!type) return false;
    return type->isa<ArrayTypeAST>();
}

bool isValidImplTarget(SemanticContext& ctx, TypeAST* type) {
    if (!type) return false;
    
    // Unwrap aliases
    TypeAST* underlying = unwrapAliases(ctx, type);
    
    // Function types cannot be impl targets (grammar rule)
    if (underlying->isa<FuncTypeAST>()) {
        return false;
    }
    
    // All other types are valid impl targets:
    // - Primitives (int, string, etc.)
    // - Structs
    // - Enums
    // - Arrays ([_, T], [*, T], [N, T])
    // - References (&T)
    // - Pointers (*T)
    // - Nullable types (T?)
    // - Result types (T!E)
    return true;
}

} // namespace ConstraintChecker