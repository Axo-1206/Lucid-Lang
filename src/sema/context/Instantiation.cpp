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

    // ─── Substitute fields ─────────────────────────────────────────────
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

    // ─── Build the final struct ────────────────────────────────────────
    StructDeclAST* finalStruct = ctx.arena.make<StructDeclAST>(
        shell->name,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),
        ctx.arena.makeSpan<FieldDeclAST*>(fieldList),
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    finalStruct->mangledName = shell->mangledName;
    finalStruct->loc = shell->loc;

    // ─── Update the cache BEFORE resolving fields ─────────────────────
    //
    // Same register-before-recursing pattern as the function case. If
    // resolving a substituted field type recursively triggers
    // createInstantiatedStruct for the same (templateDecl, typeArgs) —
    // which happens for self-referential structs like
    // `struct Node<T> { next Node<T>?; }` — the recursive call must find
    // this node in the cache and return it, rather than re-instantiating.
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalStruct;

    // ─── Re-resolve the specialized field types ───────────────────────
    //
    // The substitution produced copies of the template's field types with
    // `T` replaced by the concrete argument. Each copy's `NamedTypeAST`
    // nodes may carry a `resolvedDecl` from the template (pointing at the
    // template's type) or none at all. Re-resolving against the current
    // context fixes that.
    //
    // This is what makes a `T`-typed field concrete in the specialization
    // and gives it the correct `resolvedDecl` — the type it actually is
    // in this specialization, not the type it was written as in the
    // template.
    for (FieldDeclAST* newField : fieldList) {
        if (newField->hasSyntaxError) continue;

        // ─── Re-resolve the field type ────────────────────────────────
        if (!resolveType(newField->type, ctx)) {
            return nullptr;
        }

        // ─── Re-resolve the field default, if present ─────────────────
        //
        // The default is an expression in the struct's own scope. For a
        // non-function field, it resolves against `newField->type`.
        // For a function-typed field, its type already has `self` prepended
        // by the parser, and the default is an AnonFuncExprAST whose
        // `funcType` is that self-inclusive type. resolveExprWithTarget
        // handles both cases through the same path.
        if (newField->defaultVal) {
            TypeAST* initType = resolveExprWithTarget(
                newField->defaultVal,
                newField->type,
                ctx);
            if (!initType || initType->isa<UnknownTypeAST>()) {
                return nullptr;
            }
        }
    }

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

    // substitute signature
    GenericSubstitution subst{templateDecl->genericParams, typeArgs};
    SubstitutionContext sc{ctx, subst};

    // re-resolve the substituted signature
    TypeAST* substitutedFuncType = substituteType(templateDecl->funcType, sc);
    if (!substitutedFuncType || !substitutedFuncType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, templateDecl,
            "failed to substitute function type for '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    // Re-resolve the specialized signature after substitution so the
    // concrete NamedTypeAST nodes get their correct resolvedDecls and the
    // specialized function type matches the current context.
    if (!resolveFuncType(substitutedFuncType->as<FuncTypeAST>(), ctx)) {
        return nullptr;
    }

    // substitute body
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

    // Register the completed specialization before resolving the body so
    // recursive instantiations hit this node instead of expanding the
    // template again.
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalFunc;

    // resolve the specialized body
    if (finalFunc->init) {
        TypeAST* initType = resolveExprWithTarget(
            finalFunc->init,
            finalFunc->funcType,
            ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            return nullptr;
        }
    }

    Trace::detail("Finalized instantiated function: ",
                  ctx.pool.lookup(finalFunc->mangledName));

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
    std::vector<TypeAST*> canonicalArgs;
    canonicalArgs.reserve(typeArgs.size());
    for (TypeAST* arg : typeArgs) {
        canonicalArgs.push_back(canonicalizeTypeArg(arg, ctx));
    }
    ArenaSpan<TypeAST*> canonicalSpan =
        ctx.arena.makeSpan<TypeAST*>(canonicalArgs);

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