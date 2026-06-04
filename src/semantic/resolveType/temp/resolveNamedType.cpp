#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolveNamedType(NamedTypeAST& node) {
    LUC_LOG_SEMANTIC("resolveNamedType: name=" << ctx_.pool.lookup(node.name));

    // Check if this is a generic parameter
    if (isGenericParam(node.name)) {
        TypeAST* subst = lookupSubstitution(node.name);
        if (subst) {
            LUC_LOG_SEMANTIC("substituting '" << ctx_.pool.lookup(node.name) << "'");
            return subst;
        }
        node.isGenericParam = true;
        return &node;
    }

    node.isGenericParam = false;

    // Lookup in symbol table
    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (!sym) {
        ctx_.error(node.loc, DiagCode::E2001, "type '", ctx_.pool.lookup(node.name), "' is not declared");
        return nullptr;
    }

    // REJECT TRAITS: A trait cannot be used as a value type (variable, parameter, return)
    if (sym->kind == SymbolKind::Trait) {
        ctx_.error(node.loc, DiagCode::E2002,
                  "trait '", ctx_.pool.lookup(node.name), "' cannot be used as a value type");
        return nullptr;
    }

    // Must be a type kind
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::TypeAlias) {
        ctx_.error(node.loc, DiagCode::E2002, "'", ctx_.pool.lookup(node.name), "' is a value, not a type");
        return nullptr;
    }

    // Unwrap type alias
    if (sym->kind == SymbolKind::TypeAlias) {
        TypeAST* resolvedAlias = resolveType(sym->type);
        if (resolvedAlias) return resolvedAlias;
        ctx_.error(node.loc, DiagCode::E2002, "failed to resolve alias '", ctx_.pool.lookup(node.name), "'");
        return nullptr;
    }

    // Resolve generic arguments
    for (auto& arg : node.genericArgs) {
        resolveType(arg.get());
    }

    // Generic constraint checking for structs
    if (sym->kind == SymbolKind::Struct && !node.genericArgs.empty()) {
        auto* structDecl = sym->decl->as<StructDeclAST>();
        if (structDecl->genericParams.size() != node.genericArgs.size()) {
            ctx_.error(node.loc, DiagCode::E2035,
                      "struct '", ctx_.pool.lookup(node.name), "' expects ",
                      structDecl->genericParams.size(), " generic arguments, got ",
                      node.genericArgs.size());
            return nullptr;
        }

        for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
            GenericParamAST* gp = structDecl->genericParams[i].get();
            if (!gp) continue;

            TypeAST* argType = node.genericArgs[i].get();
            if (!argType) {
                ctx_.error(node.loc, DiagCode::E2002,
                          "generic argument ", i, " is null for struct '",
                          ctx_.pool.lookup(node.name), "'");
                return nullptr;
            }

            std::vector<InternedString> requiredTraits;
            for (auto traitName : gp->constraints) {
                requiredTraits.push_back(traitName);
            }

            if (!satisfiesConstraints(argType, requiredTraits)) {
                std::string_view argName = argType->kind == ASTKind::NamedType
                    ? ctx_.pool.lookup(argType->as<NamedTypeAST>()->name)
                    : "unnamed";
                std::string constraintsStr;
                for (size_t j = 0; j < requiredTraits.size(); ++j) {
                    if (j > 0) constraintsStr += ", ";
                    constraintsStr += ctx_.pool.lookup(requiredTraits[j]);
                }
                ctx_.error(node.loc, DiagCode::E2002,
                          "type '", argName, "' does not satisfy constraints '",
                          constraintsStr, "' for generic parameter '",
                          ctx_.pool.lookup(gp->name), "'");
                return nullptr;
            }
        }
    }

    return &node;
}