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
// Injection Form Type Transformation
// ─────────────────────────────────────────────────────────────────────────────

FuncTypeAST* TypeResolver::transformInjectionType(FuncTypeAST* funcType, InternedString receiverName, const SourceLocation& loc) {
    if (!funcType) {
        ctx_.error(loc, DiagCode::E2002, "cannot transform null function type for injection");
        return nullptr;
    }

    // Need at least one parameter group with at least one parameter
    if (funcType->sig.groupCount() == 0) {
        ctx_.error(loc, DiagCode::E2002, "injection requires a function with at least one parameter");
        return nullptr;
    }

    auto firstGroup = funcType->sig.getGroup(0);
    if (firstGroup.empty()) {
        ctx_.error(loc, DiagCode::E2002, "injection requires the first parameter group to have at least one parameter");
        return nullptr;
    }

    // Observe the first parameter (non-owning — the arena owns it)
    const ParamAST* firstParam = firstGroup[0].get();

    // Validate that the receiver name matches the parameter name or is a placeholder
    // In practice, the receiver name from the impl block ("self" or alias) should match
    // the first parameter's name in the function being injected, or we allow any name
    // (the injection just removes the first parameter regardless of name)

    // Create a new function type with the first parameter removed
    auto* newFuncType = ctx_.arena.make<FuncTypeAST>().release();
    
    // Copy qualifiers
    newFuncType->qualifiers = funcType->qualifiers;
    if (!funcType->rawQualifiers.empty()) {
        auto qBuilder = ctx_.arena.makeBuilder<InternedString>();
        for (const auto& q : funcType->rawQualifiers) {
            qBuilder.push_back(q);
        }
        newFuncType->rawQualifiers = qBuilder.build();
    }

    // Build new parameter list (all parameters except the first)
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    // Handle first group: skip first parameter
    size_t remainingInFirstGroup = firstGroup.size() - 1;
    if (remainingInFirstGroup > 0) {
        groupSizes.push_back(remainingInFirstGroup);
        for (size_t i = 1; i < firstGroup.size(); ++i) {
            auto* param = ctx_.arena.make<ParamAST>().release();
            param->name = firstGroup[i]->name;
            param->type = TypePtr(cloneType(firstGroup[i]->type.get()));
            param->isVariadic = firstGroup[i]->isVariadic;
            param->loc = firstGroup[i]->loc;
            allParams.emplace_back(param);
        }
    }
    
    // Copy remaining groups unchanged
    for (size_t g = 1; g < funcType->sig.groupCount(); ++g) {
        auto group = funcType->sig.getGroup(g);
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
    
    // Build the new parameter span
    auto paramBuilder = ctx_.arena.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    newFuncType->sig.allParams = paramBuilder.build();
    
    auto sizeBuilder = ctx_.arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    newFuncType->sig.groupSizes = sizeBuilder.build();
    
    // Copy return types unchanged
    auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
    for (const auto& ret : funcType->sig.returnTypes) {
        retBuilder.push_back(TypePtr(cloneType(ret.get())));
    }
    newFuncType->sig.returnTypes = retBuilder.build();
    
    newFuncType->loc = loc;
    
    return newFuncType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Extract Function Type from Callable (for from entries and method assignments)
// ─────────────────────────────────────────────────────────────────────────────

FuncTypeAST* TypeResolver::extractFuncTypeFromCallable(ExprPtr& callable, ArenaSpan<TypePtr>& explicitTypeArgs, const SourceLocation& loc) {
    if (!callable) return nullptr;
    
    // Handle different callable forms
    switch (callable->kind) {
        case ASTKind::IdentifierExpr: {
            auto* ident = callable->as<IdentifierExprAST>();
            Symbol* sym = ctx_.symbols->lookup(ident->name);
            if (!sym) {
                ctx_.error(loc, DiagCode::E2001, "function '", ctx_.pool.lookup(ident->name), "' not found");
                return nullptr;
            }
            if (sym->kind != SymbolKind::Func) {
                ctx_.error(loc, DiagCode::E2002, "'", ctx_.pool.lookup(ident->name), "' is not a function");
                return nullptr;
            }
            // The symbol's type should be a FuncTypeAST
            if (sym->type && sym->type->isa<FuncTypeAST>()) {
                return cloneFuncType(sym->type->as<FuncTypeAST>(), loc);
            }
            ctx_.error(loc, DiagCode::E2002, "function '", ctx_.pool.lookup(ident->name), "' has no resolved type");
            return nullptr;
        }
        
        case ASTKind::FieldAccessExpr: {
            auto* field = callable->as<FieldAccessExprAST>();
            // This could be a module path: math.utils.toString
            // For now, resolve the field as a symbol
            // Build a qualified name
            // Simplified: assume the field access resolves to a function
            // Full implementation would need to traverse the object chain
            
            // For now, try to resolve the field as a global symbol
            // This is a simplification; real implementation needs proper qualifier resolution
            InternedString qualifiedName = field->field; // Actually need to qualify
            Symbol* sym = ctx_.symbols->lookup(field->field);
            if (!sym) {
                ctx_.error(loc, DiagCode::E2001, "symbol not found");
                return nullptr;
            }
            if (sym->type && sym->type->isa<FuncTypeAST>()) {
                return cloneFuncType(sym->type->as<FuncTypeAST>(), loc);
            }
            return nullptr;
        }
        
        case ASTKind::CallableRefExpr: {
            auto* callableRef = callable->as<CallableRefExprAST>();
            // Extract the base entity first
            FuncTypeAST* baseType = extractFuncTypeFromCallable(callableRef->entity, callableRef->typeArgs, loc);
            if (!baseType) return nullptr;
            
            // Apply generic arguments if any
            if (!callableRef->typeArgs.empty()) {
                // TODO: Apply substitution of generic parameters
                // This requires having the generic parameters of the base function
                // For now, return the base type and log a warning
                LUC_LOG_SEMANTIC("extractFuncTypeFromCallable: generic arguments not yet applied");
            }
            return baseType;
        }
        
        case ASTKind::BehaviorAccessExpr: {
            auto* behavior = callable->as<BehaviorAccessExprAST>();
            // Look up the method in the symbol table
            // Method names are mangled as "Type::method"
            std::string mangled = NameMangler::mangleMethod(
                ctx_.pool.lookup(behavior->typeName),
                ctx_.pool.lookup(behavior->method)
            );
            InternedString mangledName = ctx_.pool.intern(mangled);
            Symbol* sym = ctx_.symbols->lookup(mangledName);
            if (!sym) {
                ctx_.error(loc, DiagCode::E2001, "method '", ctx_.pool.lookup(behavior->typeName), ":", 
                          ctx_.pool.lookup(behavior->method), "' not found");
                return nullptr;
            }
            if (sym->type && sym->type->isa<FuncTypeAST>()) {
                return cloneFuncType(sym->type->as<FuncTypeAST>(), loc);
            }
            ctx_.error(loc, DiagCode::E2002, "method has no resolved type");
            return nullptr;
        }
        
        default:
            ctx_.error(loc, DiagCode::E2002, "not a callable expression");
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionReference — resolve a function reference for from entries
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* TypeResolver::resolveFunctionReference(ExprPtr& ref, ArenaSpan<TypePtr>& typeArgs, const SourceLocation& loc) {
    FuncTypeAST* funcType = extractFuncTypeFromCallable(ref, typeArgs, loc);
    if (!funcType) return nullptr;
    
    // Resolve the function type
    return resolveType(funcType);
}

// ─────────────────────────────────────────────────────────────────────────────
// From Entry Resolution Helpers
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::resolveFromEntryInline(FromEntryAST& entry, TypeAST* targetType) {
    // Create a function type from the entry's signature
    auto* funcType = ctx_.arena.make<FuncTypeAST>().release();
    
    // Copy signature from entry
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    for (size_t g = 0; g < entry.sig.groupCount(); ++g) {
        auto group = entry.sig.getGroup(g);
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
    
    // Copy return types
    auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
    for (const auto& ret : entry.sig.returnTypes) {
        retBuilder.push_back(TypePtr(cloneType(ret.get())));
    }
    funcType->sig.returnTypes = retBuilder.build();
    
    funcType->loc = entry.loc;
    
    // Resolve the function type
    TypeAST* resolved = resolveType(funcType);
    
    // Validate that return type matches target type
    if (resolved && resolved->isa<FuncTypeAST>()) {
        auto* resolvedFunc = resolved->as<FuncTypeAST>();
        if (!resolvedFunc->sig.returnTypes.empty()) {
            TypeAST* returnType = resolvedFunc->sig.returnTypes[0].get();
            if (returnType && targetType) {
                // TODO: Check type equality more thoroughly
                // For now, just log a warning if they don't match
                // Type equality check should be done by TypeChecker
            }
        }
    }
}

void TypeResolver::resolveFromEntryPath(FromEntryAST& entry, TypeAST* targetType) {
    // Resolve the function reference
    ArenaSpan<TypePtr> emptyTypeArgs; // TODO: Extract from entry if it's a generic instantiation
    TypeAST* resolved = resolveFunctionReference(entry.path, emptyTypeArgs, entry.loc);
    
    if (!resolved) {
        ctx_.error(entry.loc, DiagCode::E2002, "failed to resolve function reference for from entry");
        return;
    }
    
    // Validate that the function's return type matches targetType
    if (resolved->isa<FuncTypeAST>()) {
        auto* funcType = resolved->as<FuncTypeAST>();
        if (!funcType->sig.returnTypes.empty()) {
            TypeAST* returnType = funcType->sig.returnTypes[0].get();
            if (returnType && targetType) {
                // TODO: Check type equality
                // TypeChecker::isEqual(returnType, targetType)
            }
        } else {
            ctx_.error(entry.loc, DiagCode::E2002, "from entry function must have a return type");
        }
    } else {
        ctx_.error(entry.loc, DiagCode::E2002, "from entry must resolve to a function type");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Method Resolution Helpers
// ─────────────────────────────────────────────────────────────────────────────

void TypeResolver::resolveMethodInline(MethodDeclAST& method, ImplDeclAST& impl) {
    // Push method's generic parameters if any
    if (!method.methodGenericParams.empty()) {
        pushGenericParams(&method.methodGenericParams);
    }
    
    // Clone and resolve the method's function type
    if (method.funcType) {
        FuncTypeAST* cloned = cloneFuncType(method.funcType.get(), method.loc);
        TypeAST* resolved = resolveType(cloned);
        if (resolved && resolved->isa<FuncTypeAST>()) {
            method.funcType = ASTPtr<FuncTypeAST>(cloned);
        } else {
            ctx_.error(method.loc, DiagCode::E2002, "failed to resolve method signature");
        }
    }
    
    if (!method.methodGenericParams.empty()) {
        popGenericParams();
    }
}

void TypeResolver::resolveMethodPlainAssignment(MethodDeclAST& method, ImplDeclAST& impl) {
    // Resolve the function reference
    ArenaSpan<TypePtr> emptyTypeArgs; // Generic args are on the CallableRefExpr if any
    TypeAST* resolved = resolveFunctionReference(method.assignmentRef, emptyTypeArgs, method.loc);
    
    if (!resolved) {
        ctx_.error(method.loc, DiagCode::E2002, "failed to resolve function reference for method assignment");
        return;
    }
    
    // The resolved type becomes the method's type
    if (resolved->isa<FuncTypeAST>()) {
        method.funcType = ASTPtr<FuncTypeAST>(resolved->as<FuncTypeAST>());
    } else {
        ctx_.error(method.loc, DiagCode::E2002, "method assignment must resolve to a function type");
    }
}

void TypeResolver::resolveMethodInjectionAssignment(MethodDeclAST& method, ImplDeclAST& impl) {
    // First resolve the base function
    ArenaSpan<TypePtr> emptyTypeArgs;
    FuncTypeAST* baseType = extractFuncTypeFromCallable(method.assignmentRef, emptyTypeArgs, method.loc);
    
    if (!baseType) {
        ctx_.error(method.loc, DiagCode::E2002, "failed to resolve function for injection");
        return;
    }
    
    // Transform the type by removing the first parameter
    FuncTypeAST* transformed = transformInjectionType(baseType, method.receiverArg, method.loc);
    
    if (!transformed) {
        ctx_.error(method.loc, DiagCode::E2002, "failed to transform function type for injection");
        return;
    }
    
    // Resolve the transformed type
    TypeAST* resolved = resolveType(transformed);
    if (resolved && resolved->isa<FuncTypeAST>()) {
        method.funcType = ASTPtr<FuncTypeAST>(resolved->as<FuncTypeAST>());
    } else {
        ctx_.error(method.loc, DiagCode::E2002, "failed to resolve transformed method type");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Node Resolution
// ─────────────────────────────────────────────────────────────────────────────



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

// ─────────────────────────────────────────────────────────────────────────────
// Constraint Checking
// ─────────────────────────────────────────────────────────────────────────────

bool TypeResolver::satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const {
    if (requiredTraits.empty()) return true;
    
    // If the type is still a generic parameter (not yet substituted),
    // defer constraint checking to instantiation time.
    if (type->isa<NamedTypeAST>() && type->as<NamedTypeAST>()->isGenericParam) {
        return true;
    }
    
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
        
        // Resolve method based on its form
        if (method->isInlineBody()) {
            resolveMethodInline(*method, node);
        } else if (method->isPlainAssignment()) {
            resolveMethodPlainAssignment(*method, node);
        } else if (method->isInjectionAssignment()) {
            resolveMethodInjectionAssignment(*method, node);
        } else {
            ctx_.error(method->loc, DiagCode::E2002, "invalid method declaration form");
            continue;
        }
        
        // After resolution, store in symbol table with mangled name
        if (method->funcType) {
            std::string mangledName = NameMangler::mangleMethod(typeName, ctx_.pool.lookup(method->name));
            Symbol* methodSym = ctx_.symbols->lookup(ctx_.pool.intern(mangledName));
            if (methodSym) {
                methodSym->type = method->funcType.get();
            } else {
                // Create a new symbol if it doesn't exist
                // (impl methods are registered during collection phase)
                LUC_LOG_SEMANTIC("method symbol not found for " << mangledName);
            }
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
        
        // Determine the form of the entry
        if (entry->path) {
            // Path entry: = func_ref
            resolveFromEntryPath(*entry, targetType);
        } else {
            // Inline entry: explicit signature and body
            resolveFromEntryInline(*entry, targetType);
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