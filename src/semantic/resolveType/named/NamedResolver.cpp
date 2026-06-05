/**
 * @file NamedResolver.cpp
 * @brief Implementation of named type resolution.
 */

#include "NamedResolver.hpp"
#include "../core/GenericParamHandler.hpp"
#include "../core/ConstraintChecker.hpp"
#include "debug/DebugMacros.hpp"

NamedResolver::NamedResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
    : ctx_(ctx), paramHandler_(paramHandler) {
    LUC_LOG_SEMANTIC("NamedResolver constructed");
}

TypeAST* NamedResolver::resolve(NamedTypeAST& node) {
    LUC_LOG_SEMANTIC("NamedResolver::resolve: name=" << ctx_.pool.lookup(node.name));

    if (paramHandler_.isParam(node.name)) {
        return resolveGenericParam(node);
    }
    return resolveConcreteType(node);
}

TypeAST* NamedResolver::resolveGenericParam(NamedTypeAST& node) {
    TypeAST* subst = paramHandler_.lookupSubst(node.name);
    if (subst) {
        LUC_LOG_SEMANTIC("NamedResolver: substituting '"
                         << ctx_.pool.lookup(node.name) << "'");
        return subst;
    }
    node.isGenericParam = true;
    return &node;
}

TypeAST* NamedResolver::resolveConcreteType(NamedTypeAST& node) {
    node.isGenericParam = false;

    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (!sym) {
        ctx_.error(node.loc, DiagCode::E2001,
                   "type '" + std::string(ctx_.pool.lookup(node.name)) + "' is not declared");
        return nullptr;
    }

    if (sym->kind == SymbolKind::Trait) {
        ctx_.error(node.loc, DiagCode::E2002,
                   "trait '" + std::string(ctx_.pool.lookup(node.name)) +
                   "' cannot be used as a value type");
        return nullptr;
    }

    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::TypeAlias) {
        ctx_.error(node.loc, DiagCode::E2002,
                   "'" + std::string(ctx_.pool.lookup(node.name)) + "' is a value, not a type");
        return nullptr;
    }

    if (sym->kind == SymbolKind::TypeAlias) {
        return unwrapTypeAlias(node, sym);
    }

    if (!node.genericArgs.empty()) {
        validateGenericConstraints(node, sym);
    }

    return &node;
}

TypeAST* NamedResolver::unwrapTypeAlias(NamedTypeAST& node, Symbol* sym) {
    TypeAST* aliased = sym->type;
    if (!aliased) {
        ctx_.error(node.loc, DiagCode::E2002,
                   "failed to resolve alias '" + std::string(ctx_.pool.lookup(node.name)) + "'");
        return nullptr;
    }
    return aliased;
}

void NamedResolver::validateGenericConstraints(NamedTypeAST& node, Symbol* structSym) {
    if (structSym->kind != SymbolKind::Struct) return;

    auto* structDecl = structSym->decl->as<StructDeclAST>();
    if (structDecl->genericParams.size() != node.genericArgs.size()) {
        ctx_.error(node.loc, DiagCode::E2035,
                   "struct '" + std::string(ctx_.pool.lookup(node.name)) + "' expects " +
                   std::to_string(structDecl->genericParams.size()) +
                   " generic arguments, got " + std::to_string(node.genericArgs.size()));
        return;
    }

    for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
        auto* gp = structDecl->genericParams[i].get();
        if (!gp) continue;

        TypeAST* argType = node.genericArgs[i].get();
        if (!argType) continue;

        std::vector<InternedString> requiredTraits;
        for (auto traitName : gp->constraints) {
            requiredTraits.push_back(traitName);
        }

        if (!ConstraintChecker::satisfies(ctx_, argType, requiredTraits)) {
            std::string constraintsStr;
            for (size_t j = 0; j < requiredTraits.size(); ++j) {
                if (j > 0) constraintsStr += ", ";
                constraintsStr += ctx_.pool.lookup(requiredTraits[j]);
            }
            ctx_.error(node.loc, DiagCode::E2002,
                       "type argument does not satisfy constraint '" + constraintsStr +
                       "' for generic parameter '" + std::string(ctx_.pool.lookup(gp->name)) + "'");
        }
    }
}