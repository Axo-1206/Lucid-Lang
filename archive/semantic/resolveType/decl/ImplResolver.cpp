/**
 * @file ImplResolver.cpp
 * @brief Implementation of impl resolution.
 */

#include "ImplResolver.hpp"
#include "../core/TypeCloner.hpp"
#include "../core/ConstraintChecker.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugMacros.hpp"

ImplResolver::ImplResolver(SemanticContext& ctx,
                           GenericParamHandler& paramHandler,
                           InjectionTransformer& injector,
                           CallableExtractor& callableExtractor)
    : ctx_(ctx), paramHandler_(paramHandler), injector_(injector), callableExtractor_(callableExtractor) {
    LUC_LOG_SEMANTIC("ImplResolver constructed");
}

void ImplResolver::resolve(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("ImplResolver::resolve");

    TypeAST* target = resolveTargetType(node);
    if (!target) {
        ctx_.error(node.loc, DiagCode::E2016, "cannot resolve impl target type");
        return;
    }

    TypeAST* underlying = unwrapTargetType(target);
    const ArenaSpan<GenericParamPtr>* targetGenericParams = nullptr;
    bool isGenericStruct = false;
    if (underlying && underlying->isa<NamedTypeAST>()) {
        auto* named = underlying->as<NamedTypeAST>();
        Symbol* sym = ctx_.symbols->lookup(named->name);
        if (sym && sym->kind == SymbolKind::Struct) {
            auto* structDecl = sym->decl->as<StructDeclAST>();
            if (structDecl && !structDecl->genericParams.empty()) {
                isGenericStruct = true;
                targetGenericParams = &structDecl->genericParams;
            }
        }
    }

    validateGenericArity(node, underlying, targetGenericParams);
    if (isGenericStruct && target->isa<NamedTypeAST>()) {
        buildSubstitutionMap(node, target, targetGenericParams);
    }

    std::string typeName = "unknown";
    if (underlying && underlying->isa<NamedTypeAST>()) {
        typeName = ctx_.pool.lookup(underlying->as<NamedTypeAST>()->name);
    } else if (underlying && underlying->isa<PrimitiveTypeAST>()) {
        typeName = NameMangler::primitiveKindToString(underlying->as<PrimitiveTypeAST>()->primitiveKind);
    }
    resolveMethods(node, typeName);

    node.resolvedSelfType = underlying;
}

TypeAST* ImplResolver::resolveTargetType(ImplDeclAST& node) {
    return node.targetType.get(); // resolved by caller
}

TypeAST* ImplResolver::unwrapTargetType(TypeAST* target) {
    TypeAST* current = target;
    while (current && current->isa<NamedTypeAST>()) {
        auto* named = current->as<NamedTypeAST>();
        Symbol* sym = ctx_.symbols->lookup(named->name);
        if (!sym || sym->kind != SymbolKind::TypeAlias) break;
        if (!sym->type) break;
        current = sym->type;
    }
    return current;
}

void ImplResolver::validateGenericArity(ImplDeclAST& node, TypeAST* underlying,
                                        const ArenaSpan<GenericParamPtr>* targetGenericParams) {
    bool isPrimitive = underlying && underlying->isa<PrimitiveTypeAST>();
    bool isEnum = false;
    if (underlying && underlying->isa<NamedTypeAST>()) {
        auto* named = underlying->as<NamedTypeAST>();
        Symbol* sym = ctx_.symbols->lookup(named->name);
        if (sym && sym->kind == SymbolKind::Enum) isEnum = true;
    }

    if ((isPrimitive || isEnum || !targetGenericParams) && !node.genericParams.empty()) {
        ctx_.error(node.loc, DiagCode::E2018,
                   "impl on primitive, enum, or non‑generic type cannot have generic parameters");
        return;
    }
    if (targetGenericParams && node.genericParams.size() != targetGenericParams->size()) {
        ctx_.error(node.loc, DiagCode::E2017,
                   "generic arity mismatch: impl expects " + std::to_string(node.genericParams.size()) +
                   " parameters, but target expects " + std::to_string(targetGenericParams->size()));
    }
}

void ImplResolver::buildSubstitutionMap(ImplDeclAST& node, TypeAST* target,
                                        const ArenaSpan<GenericParamPtr>* targetGenericParams) {
    if (!target->isa<NamedTypeAST>() || !targetGenericParams) return;
    auto* named = target->as<NamedTypeAST>();
    const auto& concreteArgs = named->genericArgs;
    node.resolvedSubstitutionMap.clear();
    for (size_t i = 0; i < targetGenericParams->size() && i < concreteArgs.size(); ++i) {
        auto* gp = (*targetGenericParams)[i].get();
        if (gp && concreteArgs[i]) {
            node.resolvedSubstitutionMap[gp->name] = concreteArgs[i].get();
        }
    }
}

void ImplResolver::resolveMethods(ImplDeclAST& node, const std::string& typeName) {
    paramHandler_.pushParams(&node.genericParams);
    paramHandler_.pushSubstMap(&node.resolvedSubstitutionMap);

    for (auto& method : node.methods) {
        if (!method) continue;
        if (method->isInlineBody()) {
            resolveMethodInline(*method);
        } else if (method->isPlainAssignment()) {
            resolveMethodPlainAssignment(*method);
        } else if (method->isInjectionAssignment()) {
            resolveMethodInjectionAssignment(*method);
        } else {
            ctx_.error(method->loc, DiagCode::E2002, "invalid method declaration form");
            continue;
        }
        if (method->funcType) {
            std::string mangled = NameMangler::mangleMethod(typeName, ctx_.pool.lookup(method->name));
            InternedString mangledName = ctx_.pool.intern(mangled);
            Symbol* methodSym = ctx_.symbols->lookup(mangledName);
            if (methodSym) methodSym->type = method->funcType.get();
            else {
                Symbol newSym;
                newSym.name = mangledName;
                newSym.kind = SymbolKind::Method;
                newSym.visibility = node.visibility;
                newSym.type = method->funcType.get();
                newSym.decl = method.get();
                newSym.loc = method->loc;
                ctx_.symbols->declare(newSym);
            }
        }
    }

    paramHandler_.popSubstMap();
    paramHandler_.popParams();
}

void ImplResolver::resolveMethodInline(MethodDeclAST& method) {
    if (!method.methodGenericParams.empty()) {
        paramHandler_.pushParams(&method.methodGenericParams);
        // Method body resolution will be done in Phase 3; type already resolved.
        paramHandler_.popParams();
    }
}

void ImplResolver::resolveMethodPlainAssignment(MethodDeclAST& method) {
    ArenaSpan<TypePtr> emptyTypeArgs;
    TypeAST* resolved = callableExtractor_.resolveReference(method.assignmentRef, emptyTypeArgs, method.loc);
    if (!resolved || !resolved->isa<FuncTypeAST>()) {
        ctx_.error(method.loc, DiagCode::E2002, "plain assignment must resolve to a function type");
        return;
    }
    method.funcType = ASTPtr<FuncTypeAST>(resolved->as<FuncTypeAST>());
}

void ImplResolver::resolveMethodInjectionAssignment(MethodDeclAST& method) {
    ArenaSpan<TypePtr> emptyTypeArgs;
    FuncTypeAST* baseType = callableExtractor_.extract(method.assignmentRef, emptyTypeArgs, method.loc);
    if (!baseType) {
        ctx_.error(method.loc, DiagCode::E2002, "failed to resolve function for injection");
        return;
    }
    FuncTypeAST* transformed = injector_.transform(baseType, method.receiverArg, method.loc);
    if (!transformed) {
        ctx_.error(method.loc, DiagCode::E2002, "failed to transform function type for injection");
        return;
    }
    method.funcType = ASTPtr<FuncTypeAST>(transformed);
}