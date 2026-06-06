/**
 * @file CallChecker.cpp
 * @brief Semantic checking for function call and type conversion expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// -----------------------------------------------------------------------------
// Helper: Unwrap type aliases recursively
// -----------------------------------------------------------------------------
static TypeAST* unwrapAlias(TypeAST* type, SemanticContext& ctx) {
    while (type && type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        Symbol* sym = ctx.symbols->lookup(named->name);
        if (!sym || sym->kind != SymbolKind::TypeAlias) break;
        if (!sym->type) break;
        type = sym->type;
    }
    return type;
}

// -----------------------------------------------------------------------------
// Helper: Create a function type from the remaining curry groups
// -----------------------------------------------------------------------------
static FuncTypeAST* makeRemainingFuncType(const FuncTypeAST* original, size_t consumedParams, ASTArena& arena) {
    auto remainingPtr = arena.make<FuncTypeAST>();
    FuncTypeAST* remaining = remainingPtr.get();
    remaining->qualifiers = original->qualifiers;

    // Collect remaining groups as raw pointers
    std::vector<ParamAST*> remAllParams;
    for (size_t g = 1; g < original->sig.groupCount(); ++g) {
        auto group = original->sig.getGroup(g);
        for (auto* p : group) {
            remAllParams.push_back(p);
        }
    }

    auto paramsBuilder = arena.makeBuilder<ParamAST*>();
    for (auto* p : remAllParams) paramsBuilder.push_back(p);
    remaining->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena.makeBuilder<size_t>();
    for (size_t g = 1; g < original->sig.groupCount(); ++g) {
        gsBuilder.push_back(original->sig.groupSizes[g]);
    }
    remaining->sig.groupSizes = gsBuilder.build();

    auto retBuilder = arena.makeBuilder<TypeAST*>();
    for (auto& rt : original->sig.returnTypes) retBuilder.push_back(rt);
    remaining->sig.returnTypes = retBuilder.build();

    return remaining;
}

// -----------------------------------------------------------------------------
// Helper: Build a FuncTypeAST from a FromEntryAST (copying its signature)
// -----------------------------------------------------------------------------
static FuncTypeAST* funcTypeFromFromEntry(FromEntryAST* entry, ASTArena& arena) {
    auto funcPtr = arena.make<FuncTypeAST>();
    FuncTypeAST* func = funcPtr.get();
    func->qualifiers = 0;

    // Copy allParams (raw pointers)
    std::vector<ParamAST*> allParams;
    for (auto* p : entry->sig.allParams) allParams.push_back(p);
    auto paramsBuilder = arena.makeBuilder<ParamAST*>();
    for (auto* p : allParams) paramsBuilder.push_back(p);
    func->sig.allParams = paramsBuilder.build();

    // Copy groupSizes
    auto gsBuilder = arena.makeBuilder<size_t>();
    for (auto sz : entry->sig.groupSizes) gsBuilder.push_back(sz);
    func->sig.groupSizes = gsBuilder.build();

    // Copy returnTypes (raw pointers)
    auto retBuilder = arena.makeBuilder<TypeAST*>();
    for (auto* rt : entry->sig.returnTypes) retBuilder.push_back(rt);
    func->sig.returnTypes = retBuilder.build();

    return func;
}

// -----------------------------------------------------------------------------
// Helper: Find a 'from' conversion entry for a target type and argument types
// -----------------------------------------------------------------------------
static FromEntryAST* findFromConversion(TypeAST* targetType,
                                        const std::vector<TypeAST*>& argTypes,
                                        SemanticContext& ctx) {
    if (!targetType || !ctx.symbols) return nullptr;

    targetType = unwrapAlias(targetType, ctx);
    if (!targetType) return nullptr;

    std::string targetMangled = NameMangler::mangleType(targetType, ctx.pool, ctx.symbols);
    std::string prefix = NameMangler::getFromPrefix(targetMangled);
    std::vector<Symbol*> candidates = ctx.symbols->findSymbolsByPrefix(prefix, ctx.pool);

    for (Symbol* sym : candidates) {
        if (!sym || sym->kind != SymbolKind::Casting) continue;
        if (!sym->decl || !sym->decl->isa<FromEntryAST>()) continue;
        auto* entry = sym->decl->as<FromEntryAST>();
        if (entry->sig.groupCount() == 0) continue;
        auto firstGroup = entry->sig.getGroup(0);
        if (firstGroup.size() != argTypes.size()) continue;
        bool match = true;
        for (size_t i = 0; i < firstGroup.size(); ++i) {
            TypeAST* paramType = firstGroup[i]->type.get();
            if (!paramType) { match = false; break; }
            if (!TypeChecker::isAssignable(argTypes[i], paramType, ctx)) {
                match = false;
                break;
            }
        }
        if (match) return entry;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// Helper: Check if a type is a primitive
// -----------------------------------------------------------------------------
static bool isPrimitiveType(TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

// -----------------------------------------------------------------------------
// Helper: Check if a type is an enum (by symbol kind)
// -----------------------------------------------------------------------------
static bool isEnumType(TypeAST* type, SemanticContext& ctx) {
    type = unwrapAlias(type, ctx);
    if (!type || !type->isa<NamedTypeAST>()) return false;
    auto* named = type->as<NamedTypeAST>();
    Symbol* sym = ctx.symbols->lookup(named->name);
    return sym && sym->kind == SymbolKind::Enum;
}

// -----------------------------------------------------------------------------
// Main checker
// -----------------------------------------------------------------------------
TypeAST* checkCallExpr(CallExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkCallExpr: args=" << node.args.size());

    // -------------------------------------------------------------------------
    // 1. Detect if this is a type conversion call (callee is a type identifier)
    // -------------------------------------------------------------------------
    if (auto* ident = node.callee->as<IdentifierExprAST>()) {
        Symbol* sym = ctx.symbols->lookup(ident->name);
        if (sym && sym->type) {
            TypeAST* targetType = sym->type;
            targetType = unwrapAlias(targetType, ctx);
            if (!targetType) {
                ctx.error(node.loc, DiagCode::E2001, "cannot resolve type alias '", ident->name, "'");
                return nullptr;
            }

            // TODO: Handle generic arguments if present

            // Gather argument types
            std::vector<TypeAST*> argTypes;
            for (auto& arg : node.args) {
                TypeAST* at = checkExpr(arg.get(), ctx);
                if (!at) return nullptr;
                argTypes.push_back(at);
            }

            if (isPrimitiveType(targetType)) {
                node.resolvedType = targetType;
                node.isConst = false;
                return targetType;
            }

            if (isEnumType(targetType, ctx)) {
                node.resolvedType = targetType;
                node.isConst = false;
                return targetType;
            }

            FromEntryAST* fromEntry = findFromConversion(targetType, argTypes, ctx);
            if (!fromEntry) {
                ctx.error(node.loc, DiagCode::E2008,
                          "cannot convert arguments to type '", ident->name,
                          "' – no matching 'from' conversion");
                return nullptr;
            }

            FuncTypeAST* fromFuncType = funcTypeFromFromEntry(fromEntry, ctx.arena);
            if (fromEntry->sig.groupCount() > 1) {
                FuncTypeAST* remaining = makeRemainingFuncType(fromFuncType, argTypes.size(), ctx.arena);
                node.resolvedType = remaining;
                return remaining;
            } else {
                if (fromEntry->sig.returnTypes.empty()) {
                    node.resolvedType = nullptr;
                    return nullptr;
                }
                node.resolvedType = fromEntry->sig.returnTypes[0];
                return node.resolvedType;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Normal function call
    // -------------------------------------------------------------------------
    TypeAST* calleeType = checkExpr(node.callee.get(), ctx);
    if (!calleeType) return nullptr;

    FuncTypeAST* funcType = nullptr;
    if (calleeType->isa<FuncTypeAST>()) {
        funcType = calleeType->as<FuncTypeAST>();
    } else if (calleeType->isa<NullableTypeAST>()) {
        auto* nullable = calleeType->as<NullableTypeAST>();
        if (nullable->inner->isa<FuncTypeAST>()) {
            funcType = nullable->inner->as<FuncTypeAST>();
            ctx.warning(node.loc, DiagCode::W6014, "calling nullable function; will panic if nil");
        } else {
            ctx.error(node.loc, DiagCode::E2002, "nullable value is not a function");
            return nullptr;
        }
    } else {
        ctx.error(node.loc, DiagCode::E2002, "callee is not a function type");
        return nullptr;
    }

    if (funcType->sig.groupCount() == 0) {
        ctx.error(node.loc, DiagCode::E2002, "function has no parameter groups");
        return nullptr;
    }

    auto firstGroup = funcType->sig.getGroup(0);
    if (firstGroup.size() != node.args.size()) {
        ctx.error(node.loc, DiagCode::E2003,
                  "argument count mismatch: expected ", firstGroup.size(),
                  ", got ", node.args.size());
        return nullptr;
    }

    for (size_t i = 0; i < firstGroup.size(); ++i) {
        TypeAST* argType = checkExpr(node.args[i].get(), ctx);
        if (!argType) return nullptr;

        TypeAST* paramType = firstGroup[i]->type.get();
        if (!paramType) {
            ctx.error(node.loc, DiagCode::E2001, "parameter type not resolved");
            return nullptr;
        }

        if (!TypeChecker::isAssignable(argType, paramType, ctx)) {
            Symbol* fromCast = TypeChecker::isFromCastable(argType, paramType, ctx);
            if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
                continue;
            } else {
                ctx.error(node.args[i]->loc, DiagCode::E2002,
                          "argument ", i + 1, " type mismatch: expected ",
                          LucDebug::formatType(paramType, ctx.pool), ", got ",
                          LucDebug::formatType(argType, ctx.pool));
                return nullptr;
            }
        }
    }

    if (funcType->sig.groupCount() > 1) {
        FuncTypeAST* remaining = makeRemainingFuncType(funcType, firstGroup.size(), ctx.arena);
        node.resolvedType = remaining;
        node.isAsyncCall = funcType->isAsync();
        return remaining;
    }

    if (funcType->sig.returnTypes.empty()) {
        node.resolvedType = nullptr;
        return nullptr;
    }
    node.resolvedType = funcType->sig.returnTypes[0];
    node.isAsyncCall = funcType->isAsync();
    return node.resolvedType;
}