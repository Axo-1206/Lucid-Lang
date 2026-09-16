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
#include "sema/Sema.hpp"
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
        templateDecl->name,
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

/// @brief Build and resolve the concrete form of a specialized struct.
///
/// This is where all field-related semantic work happens for a generic
/// struct. The template itself was never resolved — it was only
/// structurally validated. Here, after substitution has replaced every
/// `T` with a concrete `TypeAST*`, we resolve the specialized tree
/// exactly as if it had been written out by hand as a non-generic
/// struct.
static StructDeclAST* finalizeInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    StructDeclAST* shell,
    SemaContext& ctx)
{
    if (!templateDecl || !shell) return nullptr;

    GenericSubstitution subst{templateDecl->genericParams, typeArgs};
    SubstitutionContext sc{ctx, subst};

    // ─── 1. Substitute fields ──────────────────────────────────────────
    std::vector<FieldDeclAST*> fieldList;
    fieldList.reserve(templateDecl->fields.size());

    for (FieldDeclAST* field : templateDecl->fields) {
        TypeAST* substitutedType = substituteType(field->type, sc);
        if (!substitutedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, field,
                "field '", ctx.pool.lookup(field->name),
                "' has invalid type in instantiation");
            return nullptr;
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
        newField->attributes = field->attributes;
        fieldList.push_back(newField);
    }

    // ─── 2. Build the final struct ─────────────────────────────────────
    StructDeclAST* finalStruct = ctx.arena.make<StructDeclAST>(
        templateDecl->name,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        ctx.arena.makeSpan<FieldDeclAST*>(fieldList),
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    finalStruct->mangledName = shell->mangledName;
    finalStruct->loc = shell->loc;

    // ─── 3. Register in the instantiation cache BEFORE resolving ───────
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalStruct;

    // ─── 4. Resolve the specialized field list ────────────────────────
    //
    // Same helper the non-generic path uses. `finalStruct->fields` is
    // the span we just built — it holds the same `FieldDeclAST*` nodes
    // as `fieldList`, but wrapped in the arena-allocated span the
    // struct itself owns.
    if (!resolveStructFieldDeclarations(finalStruct->fields, finalStruct, ctx)) {
        return nullptr;
    }

    // ─── 5. Register in the structural storage map ─────────────────────
    ArenaSpan<TypeAST*> canonicalArgs = canonicalizeTypeArgList(typeArgs, ctx);
    ctx.registerGenericTypeInstantiation(templateDecl->name, canonicalArgs, finalStruct);

    Trace::detail("Finalized instantiated struct: ",
                  ctx.pool.lookup(finalStruct->mangledName),
                  " (", finalStruct->fields.size(), " fields)");

    if (ctx.currentModule) {
        ctx.currentModule->specializations.push_back(finalStruct);
    }

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
        templateDecl->name,
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

    // ─── Substitution ──────────────────────────────────────────────────
    //
    // The template's signature and body were never resolved. Both are
    // walked through substitution here, and every `T` becomes a concrete
    // `TypeAST*`. After this, the tree contains no abstract types.
    GenericSubstitution subst{templateDecl->genericParams, typeArgs};
    SubstitutionContext sc{ctx, subst};

    TypeAST* substitutedFuncType = substituteType(templateDecl->funcType, sc);
    if (!substitutedFuncType || !substitutedFuncType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, templateDecl,
            "failed to substitute function type for '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    // ─── Resolve the substituted signature ─────────────────────────────
    //
    // Concrete now — no `T` anywhere. Ordinary `resolveFuncType`.
    if (!resolveFuncType(substitutedFuncType->as<FuncTypeAST>(), ctx)) {
        return nullptr;
    }

    // ─── Substitute the body ───────────────────────────────────────────
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

    // ─── Register before resolving the body ────────────────────────────
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalFunc;

    // ─── Resolve the substituted body ──────────────────────────────────
    if (!resolveFunctionBody(finalFunc->init, finalFunc->funcType, ctx)) {
        return nullptr;
    }

    Trace::detail("Finalized instantiated function: ",
                  ctx.pool.lookup(finalFunc->mangledName));

    if (ctx.currentModule) {
        ctx.currentModule->specializations.push_back(finalFunc);
    }
    finalFunc->resourceKind = ctx.classifyResourceKind(finalFunc->funcType);

    return finalFunc;
}

// ─── Type Argument Canonicalization ────────────────────────────────────
//
// Maps a parser-produced type node to the canonical node that represents
// the same type in the type cache. Two nodes that describe the same type
// (same primitive kind, same named type with same args, same array shape,
// ...) must map to the same canonical pointer.
//
// The instantiation cache is keyed on type-argument pointer identity.
// This is the function that makes pointer identity equivalent to
// structural identity, at least for the type shapes that can appear as
// generic arguments.

static TypeAST* canonicalizeTypeArg(TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            // The primary case. Every `int` written in source is a fresh
            // PrimitiveTypeAST from the parser; route through the
            // singleton cache to collapse them to one node.
            return ctx.getPrimitiveType(
                type->as<PrimitiveTypeAST>()->primitiveKind);

        case ASTKind::NamedType: {
            // A user type used as a generic argument — `factorial<Point>`
            // or `factorial<Box<int>>`. Canonicalize the arguments
            // recursively, then ask the type cache for the canonical
            // outer node.
            //
            // `getNamedType` is keyed on (name, genericArgs) with pointer
            // identity on the args. If the args are already canonical
            // (because we recursed first), the key is stable across
            // call sites, and this returns the same node every time.
            NamedTypeAST* named = type->as<NamedTypeAST>();
            std::vector<TypeAST*> canonicalArgsList;
            canonicalArgsList.reserve(named->genericArgs.size());
            for (TypeAST* arg : named->genericArgs) {
                canonicalArgsList.push_back(canonicalizeTypeArg(arg, ctx));
            }
            ArenaSpan<TypeAST*> canonicalArgs = ctx.arena.makeSpan<TypeAST*>(canonicalArgsList);
            NamedTypeAST* canonical =
                ctx.getNamedType(named->name, canonicalArgs);
            // Preserve the resolved decl — a resolved NamedTypeAST should
            // not lose that on the way through the cache.
            if (!canonical->resolvedDecl) {
                canonical->resolvedDecl = named->resolvedDecl;
            }
            return canonical;
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            TypeAST* canonicalElement = canonicalizeTypeArg(arr->element, ctx);
            return ctx.getArrayType(arr->arrayKind, arr->size, canonicalElement);
        }

        case ASTKind::NullableType: {
            TypeAST* inner = canonicalizeTypeArg(type->as<NullableTypeAST>()->inner, ctx);
            return ctx.getNullableType(inner);
        }

        case ASTKind::FallibleType: {
            TypeAST* inner = canonicalizeTypeArg(type->as<FallibleTypeAST>()->inner, ctx);
            return ctx.getFallibleType(inner);
        }

        case ASTKind::CombinedType: {
            TypeAST* inner = canonicalizeTypeArg(type->as<CombinedTypeAST>()->inner, ctx);
            return ctx.getCombinedType(inner);
        }

        case ASTKind::RefType: {
            TypeAST* inner = canonicalizeTypeArg(type->as<RefTypeAST>()->inner, ctx);
            return ctx.getRefType(inner);
        }

        case ASTKind::PtrType: {
            TypeAST* inner = canonicalizeTypeArg(type->as<PtrTypeAST>()->inner, ctx);
            return ctx.getPtrType(inner);
        }

        // Function types are not valid generic arguments in Lucid
        // (nullable/fallible/generic on function types are all rejected
        // by the resolver), so they never reach the instantiation cache
        // as arguments. If a future feature makes them valid, add a
        // `FuncTypeAST` cache analogous to the others.
        case ASTKind::FuncType:
            return type;

        // Simd, Arena, ArenaDescriptor are compiler-builtin and cannot
        // be used as generic arguments. If they ever are, they'd need
        // their own canonicalization path.
        case ASTKind::SimdType:
        case ASTKind::ArenaType:
        case ASTKind::ArenaDescriptorType:
        case ASTKind::ModuleTypeAccess:
        default:
            return type;
    }
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

    // ─── Canonicalize the type arguments ────────────────────────────────
    //
    // The instantiation cache is keyed on `(templateDecl, typeArgs)`
    // with pointer identity on each type argument. Two call sites that
    // write the same type — `factorial<int>(10)` and `factorial<int>(9)`
    // — produce two distinct `PrimitiveTypeAST(Int)` nodes from the
    // parser. Without canonicalization, those become two distinct cache
    // keys, and the same specialization is built twice (or N times).
    //
    // Canonicalizing here — at the single funnel every instantiation
    // enters through — means the cache key is stable regardless of how
    // many call sites name the same type. It also means downstream
    // consumers (substitution, codegen) see the canonical nodes, so a
    // `T = int` substitution maps to the type singleton and not to a
    // per-call-site copy.
    ArenaSpan<TypeAST*> canonicalSpan = canonicalizeTypeArgList(typeArgs, ctx);

    if (isFunction) {
        FuncDeclAST* instantiated = createInstantiatedFunction(
            templateDecl->as<FuncDeclAST>(), canonicalSpan, ctx);
        if (!instantiated) return result;
        result.resolvedDecl = instantiated;
    } else {
        StructDeclAST* instantiated = createInstantiatedStruct(
            templateDecl->as<StructDeclAST>(), canonicalSpan, ctx);
        if (!instantiated) return result;
        result.resolvedDecl = instantiated;
    }

    return result;
}

// ─── canonicalizeTypeArgList ──────────────────────────────────────

ArenaSpan<TypeAST*> canonicalizeTypeArgList(const ArenaSpan<TypeAST*>& args, SemaContext& ctx) {
    std::vector<TypeAST*> canonicalList;
    canonicalList.reserve(args.size());
    for (TypeAST* arg : args) {
        canonicalList.push_back(canonicalizeTypeArg(arg, ctx));
    }
    return ctx.arena.makeSpan<TypeAST*>(canonicalList);
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