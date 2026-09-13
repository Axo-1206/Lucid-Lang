/// @file sema/context/Instantiation.cpp
/// @brief Generic instantiation — orchestration, caching, validation.
///
/// See Generic.hpp for the split between substitution and instantiation.
/// This file implements instantiation: the "register a shell, then fill
/// it" pattern that handles recursion and deduplication, and produces the
/// final specialized declaration.
///
/// Substitution is a mechanical tree-rewriting pass (Generic.cpp);
/// instantiation drives it, manages the cache, and validates arity.

#include "Generic.hpp"
#include "sema/support/MangledName.hpp"
#include "core/trace/Trace.hpp"
#include "sema/types/SemaType.hpp"

namespace sema {

// ─── Shell Creation: Structs ──────────────────────────────────────────

static StructDeclAST* createInstantiatedStructShell(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    InternedString mangledName,
    SemaContext& ctx)
{
    if (!templateDecl || !mangledName.isValid()) {
        return nullptr;
    }

    StructDeclAST* shell = ctx.arena.make<StructDeclAST>(
        mangledName,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        ctx.arena.emptySpan<FieldDeclAST*>(),
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    shell->mangledName = mangledName;
    shell->loc = templateDecl->loc;

    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = shell;

    return shell;
}

static StructDeclAST* finalizeInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    StructDeclAST* shell,
    SemaContext& ctx)
{
    if (!templateDecl || !shell) return nullptr;

    GenericSubstitution subst{templateDecl->genericParams, typeArgs};
    SubstitutionContext sc{ctx, subst};

    std::vector<FieldDeclAST*> fieldList;
    fieldList.reserve(templateDecl->fields.size());
    bool hasError = false;

    for (FieldDeclAST* field : templateDecl->fields) {
        TypeAST* substitutedType = substituteType(field->type, sc);
        if (!substitutedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, field,
                "field '", ctx.pool.lookup(field->name),
                "' has invalid type in instantiation");
            hasError = true;
            break;
        }

        ExprAST* substitutedDefault = field->defaultVal
            ? substituteExpr(field->defaultVal, sc)
            : nullptr;

        FieldDeclAST* newField = ctx.arena.make<FieldDeclAST>(
            field->name,
            substitutedType,
            substitutedDefault,
            field->isConstField
        );
        newField->loc = field->loc;
        fieldList.push_back(newField);
    }

    if (hasError) return nullptr;

    StructDeclAST* finalStruct = ctx.arena.make<StructDeclAST>(
        shell->name,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        ctx.arena.makeSpan<FieldDeclAST*>(fieldList),
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    finalStruct->mangledName = shell->mangledName;
    finalStruct->loc = shell->loc;

    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalStruct;

    Trace::detail("Finalized instantiated struct: ",
                  ctx.pool.lookup(finalStruct->mangledName),
                  " (", fieldList.size(), " fields)");

    return finalStruct;
}

// ─── Shell Creation: Functions ────────────────────────────────────────

static FuncDeclAST* createInstantiatedFunctionShell(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    InternedString mangledName,
    SemaContext& ctx)
{
    if (!templateDecl || !mangledName.isValid()) {
        return nullptr;
    }

    FuncDeclAST* shell = ctx.arena.make<FuncDeclAST>(
        mangledName,
        templateDecl->keyword,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        nullptr,
        nullptr
    );
    shell->mangledName = mangledName;
    shell->isForeignFunction = templateDecl->isForeignFunction;
    shell->isInline = templateDecl->isInline;
    shell->isNoInline = templateDecl->isNoInline;
    shell->loc = templateDecl->loc;

    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = shell;

    return shell;
}

static FuncDeclAST* finalizeInstantiatedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    FuncDeclAST* shell,
    SemaContext& ctx)
{
    if (!templateDecl || !shell) return nullptr;

    GenericSubstitution subst{templateDecl->genericParams, typeArgs};
    SubstitutionContext sc{ctx, subst};

    TypeAST* substitutedFuncType = substituteType(templateDecl->funcType, sc);
    if (!substitutedFuncType || !substitutedFuncType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, templateDecl,
            "failed to substitute function type for '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    ExprAST* substitutedInit = nullptr;
    if (templateDecl->init) {
        substitutedInit = substituteExpr(templateDecl->init, sc);
        if (!substitutedInit) {
            ctx.diagnostics.error(DiagCode::Sem_MissingFuncBody, templateDecl,
                "failed to substitute initializer for '",
                ctx.pool.lookup(templateDecl->name), "'");
            return nullptr;
        }
    }

    FuncDeclAST* finalFunc = ctx.arena.make<FuncDeclAST>(
        shell->name,
        shell->keyword,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        substitutedFuncType->as<FuncTypeAST>(),
        substitutedInit
    );
    finalFunc->mangledName = shell->mangledName;
    finalFunc->isForeignFunction = shell->isForeignFunction;
    finalFunc->isInline = shell->isInline;
    finalFunc->isNoInline = shell->isNoInline;
    finalFunc->loc = shell->loc;

    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalFunc;

    Trace::detail("Finalized instantiated function: ",
                  ctx.pool.lookup(finalFunc->mangledName));

    return finalFunc;
}

// ─── resolveGenericInstantiation ──────────────────────────────────────

GenericResolution resolveGenericInstantiation(
    DeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx)
{
    GenericResolution result;

    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null declaration");
        return result;
    }

    bool isFunction = templateDecl->isa<FuncDeclAST>();
    bool isStruct = templateDecl->isa<StructDeclAST>();

    if (!isFunction && !isStruct) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, templateDecl,
                              "declaration '", ctx.pool.lookup(templateDecl->name),
                              "' cannot be instantiated with generic arguments");
        return result;
    }

    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (isFunction) {
        genericParams = templateDecl->as<FuncDeclAST>()->genericParams;
    } else {
        genericParams = templateDecl->as<StructDeclAST>()->genericParams;
    }

    if (typeArgs.size() != genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
                              "declaration '", ctx.pool.lookup(templateDecl->name),
                              "' expected ", genericParams.size(),
                              " generic arguments, got ", typeArgs.size());
        return result;
    }

    for (TypeAST* arg : typeArgs) {
        if (!arg) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, templateDecl,
                                  "invalid generic argument (null)");
            return result;
        }
        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* namedArg = arg->as<NamedTypeAST>();
            if (!namedArg->resolvedDecl) {
                resolveNamedType(namedArg, ctx);
                if (!namedArg->resolvedDecl) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, arg,
                                          "unresolved type '",
                                          ctx.pool.lookup(namedArg->name),
                                          "' in generic argument");
                    return result;
                }
            }
        }
    }

    if (isFunction) {
        FuncDeclAST* instantiated = createInstantiatedFunction(
            templateDecl->as<FuncDeclAST>(), typeArgs, ctx);
        if (!instantiated) return result;
        result.resolvedDecl = instantiated;
    } else {
        StructDeclAST* instantiated = createInstantiatedStruct(
            templateDecl->as<StructDeclAST>(), typeArgs, ctx);
        if (!instantiated) return result;
        result.resolvedDecl = instantiated;
    }

    return result;
}

// ─── createInstantiatedStruct ─────────────────────────────────────────

StructDeclAST* createInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx)
{
    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null struct declaration");
        return nullptr;
    }

    InstantiationKey key{templateDecl, typeArgs};
    auto it = ctx.instantiationCache.find(key);
    if (it != ctx.instantiationCache.end()) {
        return it->second ? it->second->as<StructDeclAST>() : nullptr;
    }

    if (typeArgs.size() != templateDecl->genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
            "struct '", ctx.pool.lookup(templateDecl->name),
            "' expected ", templateDecl->genericParams.size(),
            " generic arguments, got ", typeArgs.size());
        return nullptr;
    }

    InternedString mangledName = generateMangledNameForGeneric(
        templateDecl, typeArgs, ctx);

    if (!mangledName.isValid()) {
        ctx.diagnostics.error(DiagCode::Backend_InvalidIR, templateDecl,
            "failed to generate mangled name for generic struct '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    StructDeclAST* shell = createInstantiatedStructShell(
        templateDecl, typeArgs, mangledName, ctx);
    if (!shell) return nullptr;

    StructDeclAST* finalStruct = finalizeInstantiatedStruct(
        templateDecl, typeArgs, shell, ctx);
    if (!finalStruct) return nullptr;

    Trace::detail("Created instantiated struct: ",
                  ctx.pool.lookup(finalStruct->mangledName),
                  " (", finalStruct->fields.size(), " fields)");

    return finalStruct;
}

// ─── createInstantiatedFunction ───────────────────────────────────────

FuncDeclAST* createInstantiatedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx)
{
    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null function declaration");
        return nullptr;
    }

    InstantiationKey key{templateDecl, typeArgs};
    auto it = ctx.instantiationCache.find(key);
    if (it != ctx.instantiationCache.end()) {
        return it->second ? it->second->as<FuncDeclAST>() : nullptr;
    }

    if (typeArgs.size() != templateDecl->genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
            "function '", ctx.pool.lookup(templateDecl->name),
            "' expected ", templateDecl->genericParams.size(),
            " generic arguments, got ", typeArgs.size());
        return nullptr;
    }

    InternedString mangledName = generateMangledNameForGeneric(
        templateDecl, typeArgs, ctx);

    if (!mangledName.isValid()) {
        ctx.diagnostics.error(DiagCode::Backend_InvalidIR, templateDecl,
            "failed to generate mangled name for generic function '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    FuncDeclAST* shell = createInstantiatedFunctionShell(
        templateDecl, typeArgs, mangledName, ctx);
    if (!shell) return nullptr;

    FuncDeclAST* finalFunc = finalizeInstantiatedFunction(
        templateDecl, typeArgs, shell, ctx);
    if (!finalFunc) return nullptr;

    Trace::detail("Created instantiated function: ",
                  ctx.pool.lookup(finalFunc->mangledName));

    return finalFunc;
}

} // namespace sema