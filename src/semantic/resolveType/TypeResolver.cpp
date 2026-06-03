/**
 * @file TypeResolver.cpp
 * @responsibility Implements Phase 2a of semantic analysis: resolving type annotations.
 *
 * Uses switch-case dispatch on ASTKind instead of the visitor pattern.
 * All state is passed via SemanticContext stored as ctx_ member.
 */

#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor – stores reference to context
// ─────────────────────────────────────────────────────────────────────────────
TypeResolver::TypeResolver(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("TypeResolver constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveType  —  Main entry point for type resolution
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeResolver::resolveType(TypeAST* typeNode) {
    if (!typeNode) return nullptr;

    switch (typeNode->kind) {
        case ASTKind::PrimitiveType:
            return resolvePrimitiveType(*typeNode->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return resolveNamedType(*typeNode->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return resolveNullableType(*typeNode->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return resolveResultType(*typeNode->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return resolveArrayType(*typeNode->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return resolveRefType(*typeNode->as<RefTypeAST>());
        case ASTKind::PtrType:
            return resolvePtrType(*typeNode->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return resolveFuncType(*typeNode->as<FuncTypeAST>());
        default:
            LUC_LOG_SEMANTIC("resolveType: unhandled kind " << static_cast<int>(typeNode->kind));
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter & Substitution Stacks
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::pushGenericParams(const ArenaSpan<GenericParamPtr>* params) {
    genericParamsStack_.push_back(params);
    LUC_LOG_SEMANTIC_EXTREME("pushGenericParams: depth=" << genericParamsStack_.size());
}

void TypeResolver::popGenericParams() {
    if (!genericParamsStack_.empty()) {
        genericParamsStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popGenericParams: depth=" << genericParamsStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popGenericParams: ERROR - stack empty");
    }
}

bool TypeResolver::isGenericParam(InternedString name) const {
    for (auto it = genericParamsStack_.rbegin(); it != genericParamsStack_.rend(); ++it) {
        const auto* params = *it;
        if (!params) continue;
        for (const auto& gp : *params) {
            if (gp && gp->name == name) return true;
        }
    }
    return false;
}

void TypeResolver::pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map) {
    substitutionMapStack_.push_back(map);
    LUC_LOG_SEMANTIC_EXTREME("pushSubstitutionMap: depth=" << substitutionMapStack_.size());
}

void TypeResolver::popSubstitutionMap() {
    if (!substitutionMapStack_.empty()) {
        substitutionMapStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popSubstitutionMap: depth=" << substitutionMapStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popSubstitutionMap: ERROR - stack empty");
    }
}

TypeAST* TypeResolver::lookupSubstitution(InternedString name) const {
    for (auto it = substitutionMapStack_.rbegin(); it != substitutionMapStack_.rend(); ++it) {
        const auto* map = *it;
        if (!map) continue;
        auto found = map->find(name);
        if (found != map->end()) return found->second;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper Methods
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* TypeResolver::cloneType(const TypeAST* type) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return ctx_.arena.make<PrimitiveTypeAST>(type->as<PrimitiveTypeAST>()->primitiveKind).release();

        case ASTKind::NamedType: {
            auto* src = type->as<NamedTypeAST>();
            auto* dst = ctx_.arena.make<NamedTypeAST>(src->name).release();
            dst->loc = src->loc;
            dst->isGenericParam = src->isGenericParam;
            if (!src->genericArgs.empty()) {
                auto builder = ctx_.arena.makeBuilder<TypePtr>();
                for (const auto& arg : src->genericArgs) {
                    builder.push_back(TypePtr(cloneType(arg.get())));
                }
                dst->genericArgs = builder.build();
            }
            return dst;
        }

        case ASTKind::NullableType:
            return ctx_.arena.make<NullableTypeAST>(
                TypePtr(cloneType(type->as<NullableTypeAST>()->inner.get()))).release();

        case ASTKind::ResultType: {
            auto* src = type->as<ResultTypeAST>();
            return ctx_.arena.make<ResultTypeAST>(
                TypePtr(cloneType(src->inner.get())),
                TypePtr(cloneType(src->errorType.get()))).release();
        }

        case ASTKind::ArrayType: {
            auto* src = type->as<ArrayTypeAST>();
            return ctx_.arena.make<ArrayTypeAST>(
                src->arrayKind, src->size,
                TypePtr(cloneType(src->element.get()))).release();
        }

        case ASTKind::RefType:
            return ctx_.arena.make<RefTypeAST>(
                TypePtr(cloneType(type->as<RefTypeAST>()->inner.get()))).release();

        case ASTKind::PtrType:
            return ctx_.arena.make<PtrTypeAST>(
                TypePtr(cloneType(type->as<PtrTypeAST>()->inner.get()))).release();

        case ASTKind::FuncType:
            return cloneFuncType(type->as<FuncTypeAST>(), type->loc);

        default:
            return nullptr;
    }
}

FuncTypeAST* TypeResolver::cloneFuncTypeInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc) {
    // Copy qualifiers and raw qualifiers
    dst->qualifiers = src->qualifiers;
    
    // Copy raw qualifiers (ArenaSpan of InternedString)
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = ctx_.arena.makeBuilder<InternedString>();
        for (const auto& q : src->rawQualifiers) {
            qBuilder.push_back(q);
        }
        dst->rawQualifiers = qBuilder.build();
    }

    // Clone parameters (flattened)
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;

    for (size_t g = 0; g < src->sig.groupCount(); ++g) {
        auto group = src->sig.getGroup(g);
        groupSizes.push_back(group.size());
        for (const auto& param : group) {
            auto* newParam = ctx_.arena.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(cloneType(param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }

    auto paramBuilder = ctx_.arena.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();

    auto sizeBuilder = ctx_.arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();

    // Clone return types
    auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(cloneType(ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();

    dst->loc = loc;
    return dst;
}

FuncTypeAST* TypeResolver::cloneFuncType(const FuncTypeAST* src, const SourceLocation& loc) {
    if (!src) return nullptr;
    auto* dst = ctx_.arena.make<FuncTypeAST>().release();
    return cloneFuncTypeInternal(dst, src, loc);
}

TypeAST* TypeResolver::getFunctionReturnType(const FuncTypeAST& type, const SourceLocation* loc) {
    if (type.sig.returnTypes.empty()) return nullptr;
    if (type.sig.returnTypes.size() > 1) {
        if (loc) {
            ctx_.error(*loc, DiagCode::E2002,
                       "function has multiple return types but single return expected");
        }
        return nullptr;
    }
    return resolveType(type.sig.returnTypes[0].get());
}

std::vector<TypeAST*> TypeResolver::getFunctionReturnTypes(const FuncTypeAST& type) {
    std::vector<TypeAST*> result;
    for (auto& retType : type.sig.returnTypes) {
        if (retType) result.push_back(resolveType(retType.get()));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Node Resolution
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* TypeResolver::resolvePrimitiveType(PrimitiveTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolvePrimitiveType: kind=" << static_cast<int>(node.primitiveKind));
    return &node;
}

TypeAST* TypeResolver::resolveNullableType(NullableTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveNullableType");
    if (node.inner) resolveType(node.inner.get());
    return &node;
}

TypeAST* TypeResolver::resolveResultType(ResultTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveResultType");
    if (node.inner) resolveType(node.inner.get());
    if (node.errorType) resolveType(node.errorType.get());
    return &node;
}

TypeAST* TypeResolver::resolveArrayType(ArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveArrayType: kind=" << static_cast<int>(node.arrayKind));
    if (node.arrayKind == ArrayKind::Fixed && node.size == 0) {
        ctx_.error(node.loc, DiagCode::E2002, "fixed array size must be greater than zero");
    }
    if (node.element) resolveType(node.element.get());
    return &node;
}

TypeAST* TypeResolver::resolveRefType(RefTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveRefType");
    if (node.inner) resolveType(node.inner.get());
    return &node;
}

TypeAST* TypeResolver::resolvePtrType(PtrTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolvePtrType");
    if (node.inner) resolveType(node.inner.get());
    return &node;
}

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

    // Must be a type kind
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait && sym->kind != SymbolKind::TypeAlias) {
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

TypeAST* TypeResolver::resolveFuncType(FuncTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveFuncType");

    // Resolve qualifiers
    for (const auto& qualName : node.rawQualifiers) {
        const QualifierEntry* entry = qualifier::lookup(qualName);
        if (!entry) {
            ctx_.error(node.loc, DiagCode::E1016, "~", ctx_.pool.lookup(qualName));
        } else {
            node.qualifiers |= entry->bit;
        }
    }

    // Resolve parameters
    for (auto& param : node.sig.allParams) {
        if (param && param->type) resolveType(param->type.get());
    }

    // Resolve return types
    for (auto& retType : node.sig.returnTypes) {
        if (retType) resolveType(retType.get());
    }

    return &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constraint Checking
// ─────────────────────────────────────────────────────────────────────────────

bool TypeResolver::satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const {
    if (requiredTraits.empty()) return true;
    if (!type->isa<NamedTypeAST>()) return false;
    auto* named = type->as<NamedTypeAST>();
    if (!structTraits_) return false;
    auto it = structTraits_->find(named->name);
    if (it == structTraits_->end()) return false;
    const auto& implemented = it->second;
    for (InternedString req : requiredTraits) {
        bool found = false;
        for (InternedString impl : implemented) {
            if (impl == req) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Declaration Resolution
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::resolveTypeAlias(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveTypeAlias: " << ctx_.pool.lookup(node.name));
    pushGenericParams(&node.genericParams);
    TypeAST* resolved = node.aliasedType ? resolveType(node.aliasedType.get()) : nullptr;
    popGenericParams();

    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym && resolved) sym->type = resolved;
}

void TypeResolver::resolveStructFields(StructDeclAST& node) {
    pushGenericParams(&node.genericParams);
    for (auto& field : node.fields) {
        if (field && field->type) resolveType(field->type.get());
    }
    popGenericParams();

    if (!node.selfType) {
        node.selfType = ctx_.arena.make<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
    }
    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym) sym->type = node.selfType.get();
}

void TypeResolver::resolveFunctionSignature(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveFunctionSignature: " << ctx_.pool.lookup(node.name));
    pushGenericParams(&node.genericParams);
    TypeAST* resolved = node.funcType ? resolveType(node.funcType.get()) : nullptr;
    popGenericParams();

    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym && resolved) sym->type = resolved;
}

void TypeResolver::resolveImplMethods(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveImplMethods");

    // Resolve target type
    TypeAST* target = node.targetType ? resolveType(node.targetType.get()) : nullptr;
    if (!target) {
        ctx_.error(node.loc, DiagCode::E2016, "cannot resolve impl target type");
        return;
    }

    // Unwrap type aliases
    TypeAST* underlying = target;
    while (underlying && underlying->isa<NamedTypeAST>()) {
        auto* named = underlying->as<NamedTypeAST>();
        Symbol* sym = ctx_.symbols->lookup(named->name);
        if (!sym || sym->kind != SymbolKind::TypeAlias) break;
        TypeAST* aliased = sym->type ? resolveType(sym->type) : nullptr;
        if (!aliased) break;
        underlying = aliased;
    }

    // Determine target kind
    bool isPrimitive = underlying && underlying->isa<PrimitiveTypeAST>();
    bool isEnum = false;
    bool isStruct = false;
    const ArenaSpan<GenericParamPtr>* targetGenericParams = nullptr;

    if (!isPrimitive && underlying && underlying->isa<NamedTypeAST>()) {
        auto* namedTarget = underlying->as<NamedTypeAST>();
        Symbol* targetSym = ctx_.symbols->lookup(namedTarget->name);
        if (targetSym) {
            if (targetSym->kind == SymbolKind::Enum) isEnum = true;
            else if (targetSym->kind == SymbolKind::Struct) {
                isStruct = true;
                auto* sd = targetSym->decl->as<StructDeclAST>();
                targetGenericParams = &sd->genericParams;
            }
        }
    }

    // Validate generic parameters
    bool isGenericStruct = isStruct && targetGenericParams && !targetGenericParams->empty();
    if ((isPrimitive || isEnum || (!isGenericStruct && isStruct)) && !node.genericParams.empty()) {
        ctx_.error(node.loc, DiagCode::E2018, "impl on primitive, enum, or non‑generic type cannot have generic parameters");
        return;
    }

    if (isGenericStruct && node.genericParams.size() != targetGenericParams->size()) {
        ctx_.error(node.loc, DiagCode::E2017,
                  "generic arity mismatch: impl expects ", node.genericParams.size(),
                  " parameters, but target expects ", targetGenericParams->size());
        return;
    }

    // Build substitution map
    node.resolvedSubstitutionMap.clear();
    if (isGenericStruct && target->isa<NamedTypeAST>()) {
        const auto& concreteArgs = target->as<NamedTypeAST>()->genericArgs;
        for (size_t i = 0; i < targetGenericParams->size() && i < concreteArgs.size(); ++i) {
            auto* gp = (*targetGenericParams)[i].get();
            if (gp && concreteArgs[i]) {
                node.resolvedSubstitutionMap[gp->name] = concreteArgs[i].get();
            }
        }
    }

    // Resolve methods
    pushGenericParams(&node.genericParams);
    pushSubstitutionMap(&node.resolvedSubstitutionMap);

    std::string typeName = isPrimitive
        ? LucDebug::primitiveKindToString(underlying->as<PrimitiveTypeAST>()->primitiveKind)
        : (underlying && underlying->isa<NamedTypeAST>()
            ? std::string(ctx_.pool.lookup(underlying->as<NamedTypeAST>()->name))
            : "unknown");

    for (auto& method : node.methods) {
        if (!method) continue;
        if (method->funcType) {
            // Create a clone of the method's function type before resolving
            FuncTypeAST* clonedFuncType = cloneFuncType(method->funcType.get(), method->loc);
            TypeAST* resolved = resolveType(clonedFuncType);
            if (resolved) {
                method->funcType = ASTPtr<FuncTypeAST>(clonedFuncType);
            }
        }
        std::string mangledName = NameMangler::mangleMethod(typeName, ctx_.pool.lookup(method->name));
        Symbol* methodSym = ctx_.symbols->lookup(ctx_.pool.intern(mangledName));
        if (methodSym && method->funcType) {
            methodSym->type = method->funcType.get();
        }
    }

    popSubstitutionMap();
    popGenericParams();

    node.resolvedSelfType = underlying;
}

void TypeResolver::resolveFromEntries(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveFromEntries");

    TypeAST* targetType = node.targetType ? resolveType(node.targetType.get()) : nullptr;
    if (!targetType) {
        ctx_.error(node.loc, DiagCode::E2001, "cannot resolve from target type");
        return;
    }

    pushGenericParams(&node.genericParams);

    for (auto& entry : node.entries) {
        if (!entry) continue;
        
        // Create a FuncTypeAST from the entry's signature
        // Note: FromEntryAST does not have its own FuncTypeAST, so we need to
        // create one for resolution. The entry's signature is stored directly.
        auto* funcType = ctx_.arena.make<FuncTypeAST>().release();
        
        // Manually copy the signature members (can't use assignment due to deleted copy constructor)
        // Flatten parameters
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        
        for (size_t g = 0; g < entry->sig.groupCount(); ++g) {
            auto group = entry->sig.getGroup(g);
            groupSizes.push_back(group.size());
            for (const auto& param : group) {
                auto* newParam = ctx_.arena.make<ParamAST>().release();
                newParam->name = param->name;
                newParam->type = TypePtr(cloneType(param->type.get()));
                newParam->isVariadic = param->isVariadic;
                newParam->loc = param->loc;
                allParams.emplace_back(newParam);
            }
        }
        
        auto paramBuilder = ctx_.arena.makeBuilder<ParamPtr>();
        for (auto& p : allParams) paramBuilder.push_back(std::move(p));
        funcType->sig.allParams = paramBuilder.build();
        
        auto sizeBuilder = ctx_.arena.makeBuilder<size_t>();
        for (auto sz : groupSizes) sizeBuilder.push_back(sz);
        funcType->sig.groupSizes = sizeBuilder.build();
        
        // Clone return types
        auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
        for (const auto& ret : entry->sig.returnTypes) {
            retBuilder.push_back(TypePtr(cloneType(ret.get())));
        }
        funcType->sig.returnTypes = retBuilder.build();
        
        funcType->loc = entry->loc;
        
        // Resolve the function type
        TypeAST* resolved = resolveType(funcType);
        
        // Resolve return type if present (already done inside resolveType)
        if (entry->returnType) {
            resolveType(entry->returnType.get());
        }
    }

    popGenericParams();
}

void TypeResolver::resolveVarType(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveVarType: " << ctx_.pool.lookup(node.name));
    TypeAST* resolved = node.type ? resolveType(node.type.get()) : nullptr;
    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym && resolved) sym->type = resolved;
}