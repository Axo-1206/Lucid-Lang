/**
 * @file SemaDecl.cpp
 * @brief Implements Sema.hpp's "Declarations" section — analyzeDecl() and
 *        every specific analyze*Decl()/analyzeFieldDecl()/analyzeParam()/
 *        analyzeGenericParamDecl() function it dispatches to.
 *
 * @architectural_note Insert-before-recurse, except where it would be wrong
 *   This file's central theme is Sema.hpp's own "insert this name, THEN
 *   recurse into its internals" pattern (see its file-level architecture
 *   notes on self-reference and per-module flow) — but that pattern is
 *   applied per declaration kind, not mechanically everywhere:
 *     - Structs/enums/traits insert their own name, open a
 *       `ScopedTypeDefinition`, THEN walk fields/variants — so `Node`
 *       resolves inside its own `next ptr<Node>?` field.
 *     - Functions insert their own name BEFORE resolving params/body — so
 *       recursive calls resolve. This isn't spelled out for functions
 *       specifically in Sema.hpp's architecture notes (only structs are
 *       used as the worked example there), but the same "insert this
 *       name, then recurse" phrasing is stated generally, and a language
 *       with no way to write a recursive function would be a strange
 *       omission — see analyzeFuncDecl()'s own comment.
 *     - Variables do the OPPOSITE on purpose: the initializer is
 *       type-checked BEFORE the variable's own name is inserted, so
 *       `let x int = x` correctly fails as "undefined value 'x'" rather
 *       than silently reading its own not-yet-initialized slot — see
 *       analyzeVarDecl()'s own comment for why this is not the same
 *       exception as structs/functions get.
 *
 * @architectural_note Redeclaration is checked against ONE tier, not lookup
 *   `ctx.lookupValue()`/`lookupType()` search outward through every open
 *   scope plus the module table — exactly right for resolving a *use*, but
 *   wrong for detecting a *redeclaration*: an inner scope legitimately
 *   shadowing an outer name is not an error. Every duplicate-name check in
 *   this file therefore looks only at the single tier a new declaration is
 *   about to occupy (`valueAlreadyDeclaredHere()`/`typeAlreadyDeclaredHere()`
 *   below use `ctx.currentModuleTable`/`ctx.currentScope()` directly, not
 *   `lookupValue()`), or — for fields/variants/trait-fields, which live in
 *   an `ArenaSpan` on their owner rather than a `Scope`/`ModuleTable` map —
 *   a linear scan over already-processed siblings.
 *
 */

#include "../Sema.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers — redeclaration checks and the shared "find @[foreign]" scan.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/**
 * @brief Is `name` already present in the exact tier `ctx.insertValue()`
 *        would write to right now?
 *
 * Deliberately NOT `ctx.lookupValue() != nullptr` — see this file's
 * "Redeclaration is checked against ONE tier" note above.
 */
bool valueAlreadyDeclaredHere(InternedString name, SemaContext& ctx) {
    if (ctx.isAtModuleLevel()) {
        return ctx.currentModuleTable && ctx.currentModuleTable->values.count(name) != 0;
    }
    return ctx.currentScope().values.count(name) != 0;
}

/// Type-namespace counterpart of valueAlreadyDeclaredHere() — same reasoning.
bool typeAlreadyDeclaredHere(InternedString name, SemaContext& ctx) {
    if (ctx.isAtModuleLevel()) {
        return ctx.currentModuleTable && ctx.currentModuleTable->types.count(name) != 0;
    }
    return ctx.currentScope().types.count(name) != 0;
}

/// Reports E2101 at `node` if `alreadyThere` — the one-line check every
/// analyze*() below runs right before inserting a new name.
void reportIfRedeclared(bool alreadyThere, InternedString name, BaseAST* node, SemaContext& ctx) {
    if (alreadyThere) {
        ctx.error(node, DiagCode::E2101, "redeclaration of '", ctx.toString(name), "'");
    }
}

/**
 * @brief Find this declaration's `@[foreign(...)]` attribute, if any.
 *
 * Compared as an InternedString against `ctx.pool.intern("foreign")`, not
 * text — same reasoning as AttributesRegistry.hpp's own "Comparing
 * InternedString, not text" note. `validateAttributes()` has already run
 * on `attrs` by the time every caller of this function reaches it (each
 * analyze*Decl() below calls it first), so a malformed `@[foreign(...)]`
 * has already been reported; this function just needs to know whether one
 * is present at all.
 */
AttributeAST* findForeignAttr(ArenaSpan<AttributePtr> attrs, SemaContext& ctx) {
    InternedString foreign = ctx.pool.intern("foreign");
    for (AttributeAST* attr : attrs) {
        if (attr->name == foreign) return attr;
    }
    return nullptr;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// analyzeDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Dispatch `decl` to its specific `analyze*Decl()` by `decl->kind`.
 *
 * How it works: a plain switch over `ASTKind`, one case per top-level
 * declaration kind — `ImportDecl`/`VarDecl`/`FuncDecl`/`EnumDecl`/
 * `TraitDecl`/`StructDecl`. Each case casts with `as<T>()` (safe: the
 * `case` label IS `T::staticKind`, so the assert inside `as<T>()` can never
 * fire here) and forwards to that kind's own function, which is where all
 * the actual work happens — this function does no validation of its own.
 *
 * Any other `ASTKind` — `UnknownDeclAST` (parser error-recovery) or one of
 * the four kinds that are DeclAST but never reached through THIS
 * dispatcher (`FieldDeclAST`, `EnumVariantAST`, `TraitFieldDeclAST`,
 * `ParamAST`/`GenericParamDeclAST` — these have their own owner-taking
 * signatures and are called directly by their owning struct/enum/trait/
 * function's analyze function instead, see below) — falls through to the
 * `default` and does nothing. A `nullptr` decl (defensive; callers should
 * not pass one) also does nothing.
 *
 * @param decl The declaration to analyze.
 * @param ctx  Forwarded unchanged to whichever specific function is called.
 */
void analyzeDecl(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl: analyzeImportDecl(decl->as<ImportDeclAST>(), ctx); return;
        case ASTKind::VarDecl:    analyzeVarDecl(decl->as<VarDeclAST>(), ctx); return;
        case ASTKind::FuncDecl:   analyzeFuncDecl(decl->as<FuncDeclAST>(), ctx); return;
        case ASTKind::EnumDecl:   analyzeEnumDecl(decl->as<EnumDeclAST>(), ctx); return;
        case ASTKind::TraitDecl:  analyzeTraitDecl(decl->as<TraitDeclAST>(), ctx); return;
        case ASTKind::StructDecl: analyzeStructDecl(decl->as<StructDeclAST>(), ctx); return;
        default:
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeImportDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve an `import`'s path to a `ModuleAST` and register its alias.
 *
 * How it works: `ImportDeclAST` is deliberately not a `ValueDeclAST`/
 * `TypeDeclAST` (see its own doc comment in DeclAST.hpp), so this doesn't
 * go through `insertValue()`/`insertType()` at all. Instead:
 *
 *   1. `ctx.findModuleByPath(decl->path)` — `path` is already the fully
 *      joined dotted string (`"std.math"`); the parser is responsible for
 *      producing that from the separate path-segment tokens, not this
 *      function (see ImportDeclAST's own doc comment). Not found → report
 *      it (reusing E2001's general "undefined X" shape — "value" is a loose
 *      fit for a module, but the code's numeric range is what tooling keys
 *      on, not this literal word) and stop.
 *   2. Found → `ctx.addImportAlias(decl->alias, target)`. `decl->alias` is
 *      already populated by the parser whether or not the source wrote an
 *      explicit `as` — see ImportDeclAST's own doc comment's examples,
 *      where `import std.io` (no `as`) still shows a non-empty `alias`.
 *
 * @param decl The import declaration.
 * @param ctx  Supplies the module lookup and the alias table (on the
 *             current module's `ModuleTable`).
 */
void analyzeImportDecl(ImportDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.error(decl, DiagCode::E2001, "undefined module '", ctx.toString(decl->path), "'");
        return;
    }

    ctx.addImportAlias(decl->alias, target);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeVarDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve a variable's type, type-check its initializer (if any),
 *        enforce that `const` always has one, then insert its name.
 *
 * How it works, in the order it runs — deliberately NOT insert-then-check
 * (see this file's own "Insert-before-recurse, except where it would be
 * wrong" note):
 *
 *   1. `resolveType(decl->type, ctx)` — always non-null per VarDeclAST's
 *      own doc comment ("Type annotation is always required in Lucid").
 *   2. If there's an initializer, type-check it (`checkExpr`) and compare
 *      against the declared type with `isAssignable()` — mismatch reports
 *      E3003. If there's no initializer, that's fine for `let` but an
 *      error for `const` (E3002) — see VarDeclAST's own doc comment on
 *      why `const` must always have one.
 *   3. Redeclaration check, THEN `ctx.insertValue()` — done last, after
 *      the initializer has already been checked against whatever was
 *      visible *before* this declaration, so `let x int = x` resolves
 *      `x` on the right-hand side against an outer `x` (if any) or fails
 *      as undefined — never against the variable being declared.
 *
 * @param decl The variable declaration.
 * @param ctx  Supplies type resolution, expression checking, and insertion.
 */
void analyzeVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    TypeAST* declaredType = resolveType(decl->type, ctx);

    if (decl->init) {
        TypeAST* initType = checkExpr(decl->init, ctx);
        if (declaredType && initType && !isAssignable(declaredType, initType, ctx)) {
            ctx.error(decl->init, DiagCode::E3003, "type mismatch for '", ctx.toString(decl->name), "'");
        }
    } else if (decl->keyword == DeclKeyword::Const) {
        ctx.error(decl, DiagCode::E3002, "'", ctx.toString(decl->name), "' must have an initializer");
    }

    reportIfRedeclared(valueAlreadyDeclaredHere(decl->name, ctx), decl->name, decl, ctx);
    decl->valueType = declaredType;
    ctx.insertValue(decl->name, decl);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeFuncDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve a function's signature, insert it (before its body, so it
 *        can call itself), then either validate it as an FFI declaration
 *        or type-check its body and confirm it returns on every path.
 *
 * How it works, top to bottom:
 *
 *   1. Redeclaration check, then `decl->valueType = decl->funcType` and
 *      `ctx.insertValue(decl->name, decl)` — inserted BEFORE anything
 *      about the signature or body is resolved, specifically so a
 *      recursive call inside the body finds this same declaration. See
 *      this file's own note on why this is the opposite order from
 *      `analyzeVarDecl()`.
 *   2. `findForeignAttr()` — checked now so the rest of this function
 *      knows which of the two remaining branches to take.
 *   3. One `ScopedScope` for the WHOLE function (all generic parameters
 *      AND all parameters, across every curried group, share it) —
 *      `analyzeGenericParamDecl()` for each of `genericParams`, then
 *      `resolveFuncType(decl->funcType, ctx)` to resolve every parameter
 *      and return type in place, then `analyzeParam()` for every
 *      `ParamAST` reached by walking the curried chain via
 *      `FuncTypeAST::getNext()`. Flattening every curry level's params
 *      into one scope, rather than a nested scope per level, is an
 *      inferred choice: `FuncDeclAST` has exactly one `body`, not one per
 *      curry level, which only makes sense if the whole curried parameter
 *      list is visible as one flat set inside that single body — nothing
 *      in DeclAST.hpp/TypeAST.hpp states this explicitly.               // X
 *   4. `decl->resolvedReturnType` is cached from the INNERMOST (fully
 *      un-curried) `FuncTypeAST`'s first return type — not the outermost,
 *      whose "return type" for an outer curry level is itself another
 *      `FuncTypeAST` describing the next closure, not a real value ever
 *      produced by running `body`.
 *   5. Branch:
 *      - **`@[foreign(...)]` present**: call `validateForeignFunc()` for
 *        the signature, then return WITHOUT walking `body` — an FFI
 *        declaration has nothing Lucid-level to type-check or verify
 *        returns on, even though the parser still populates `body` with
 *        an (empty) `BlockStmtAST` for every function uniformly (see
 *        FuncDeclAST's own doc comment — this is inferred from that, not
 *        stated outright).                                              // X
 *      - **Otherwise**: open a `ScopedSemanticContext(FuncBody)` (so
 *        `return` is legal inside), `analyzeBlock(decl->body, ctx)`, and
 *        read its `bool` return per `analyzeStmt()`'s own contract — if
 *        the function isn't void and the body doesn't guarantee a return
 *        on every path, report E3005.
 *
 * @param decl The function declaration.
 * @param ctx  Supplies insertion, scoping, type resolution, and body
 *             analysis (via functions implemented elsewhere in `sema/`).
 */
void analyzeFuncDecl(FuncDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    reportIfRedeclared(valueAlreadyDeclaredHere(decl->name, ctx), decl->name, decl, ctx);
    decl->valueType = decl->funcType;
    ctx.insertValue(decl->name, decl);

    AttributeAST* foreignAttr = findForeignAttr(decl->attributes, ctx);

    ScopedScope scope(ctx);
    for (GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    resolveFuncType(decl->funcType, ctx);
    for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
        for (ParamAST* p : group->params) {
            analyzeParam(p, ctx);
        }
    }

    FuncTypeAST* innermost = decl->funcType;
    while (innermost && innermost->isCurried()) {
        innermost = innermost->getNext();
    }
    decl->resolvedReturnType = (innermost && !innermost->returnTypes.empty())
        ? innermost->returnTypes[0] : nullptr;
    bool isVoid = innermost == nullptr || innermost->isVoid();

    if (foreignAttr) {
        validateForeignFunc(decl, foreignAttr, ctx);
        return;
    }

    ScopedSemanticContext funcCtx(ctx, SemanticContext::FuncBody, decl, decl->loc);
    bool bodyReturns = decl->body && decl->body->isa<BlockStmtAST>()
        ? analyzeBlock(decl->body->as<BlockStmtAST>(), ctx)
        : false;

    if (!isVoid && !bodyReturns) {
        ctx.error(decl, DiagCode::E3005, "function '", ctx.toString(decl->name), "' is missing a return");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeEnumDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Insert an enum's name (before its variants, for the same
 *        self-reference reason structs get inserted first — though an
 *        enum referencing itself is unusual, the ordering costs nothing
 *        to apply uniformly), resolve its backing type, then analyze
 *        every variant.
 *
 * How it works: redeclare-check and `ctx.insertType()` first. If
 * `backingType` was written explicitly, resolve it with
 * `resolvePrimitiveType()`; if it's `nullptr`, nothing further happens
 * here — `EnumDeclAST`'s own doc comment says a null `backingType`
 * "defaults to int32," which is a fact codegen/lowering reads directly off
 * a null field, not something this function needs to synthesize a node
 * for. Then a `ScopedTypeDefinition` (matching struct/trait) and a plain
 * loop calling `analyzeEnumVariant()` for each of `decl->variants` — that
 * function does the actual per-variant duplicate-name/duplicate-value
 * checking, since it needs to see every sibling variant to do so.
 *
 * @param decl The enum declaration.
 * @param ctx  Supplies insertion, type resolution, and the defining-type
 *             marker `analyzeEnumVariant()`'s callees may consult.
 */
void analyzeEnumDecl(EnumDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);
    reportIfRedeclared(typeAlreadyDeclaredHere(decl->name, ctx), decl->name, decl, ctx);
    ctx.insertType(decl->name, decl);

    if (decl->backingType) {
        resolvePrimitiveType(decl->backingType, ctx);
    }

    ScopedTypeDefinition defining(ctx, decl);
    for (EnumVariantAST* variant : decl->variants) {
        analyzeEnumVariant(variant, decl, ctx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeTraitDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Insert a trait's name, resolve its generic parameters, analyze
 *        every field requirement, then confirm every generic parameter
 *        was actually used.
 *
 * How it works: same insert-before-recurse shape as `analyzeStructDecl()`
 * below, minus the parts that don't apply to a trait (no `traitRefs` — see
 * TraitDeclAST's own doc comment rule 3, "No Trait Inheritance"). Redeclare
 * check + `ctx.insertType()`, `ScopedTypeDefinition`, one `ScopedScope` for
 * `genericParams` (each via `analyzeGenericParamDecl()`), then
 * `analyzeTraitField()` per field (which does the actual duplicate-name/
 * nullable-fallible checking — see its own doc comment). Finishes with
 * `validateGenericParamUsage()`, checked last since it needs every field's
 * (already-resolved) type to know which generic parameters were actually
 * referenced.
 *
 * @param decl The trait declaration.
 * @param ctx  Supplies insertion, scoping, and the field/generic-usage
 *             validators implemented elsewhere in `sema/`.
 */
void analyzeTraitDecl(TraitDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);
    reportIfRedeclared(typeAlreadyDeclaredHere(decl->name, ctx), decl->name, decl, ctx);
    ctx.insertType(decl->name, decl);

    ScopedTypeDefinition defining(ctx, decl);
    ScopedScope generics(ctx);
    for (GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    for (TraitFieldDeclAST* field : decl->fields) {
        analyzeTraitField(field, decl, ctx);
    }

    validateGenericParamUsage(decl->genericParams, decl, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeStructDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Insert a struct's name, analyze its fields, verify every
 *        implemented trait, then confirm every generic parameter was used.
 *
 * How it works: redeclare check + `ctx.insertType()` FIRST (so `next
 * ptr<Node>?` inside `Node`'s own fields resolves — see this file's own
 * "Insert-before-recurse" note and SemaContext.hpp's "Self-Reference"
 * architecture note), then a `ScopedTypeDefinition` and one `ScopedScope`
 * for `genericParams`. Fields are analyzed first (`analyzeFieldDecl()` per
 * field — that function does the actual duplicate-name/const-nullable/
 * default-value checking), THEN `traitRefs` — each resolved with
 * `resolveTraitRef()` and checked with `validateTraitImplementation()`
 * (StructDeclAST's own "Semantic Analysis Notes" rules 1–3).
 *
 * Rule 4 ("if a struct implements multiple traits, and their required
 * fields collide by name, that's an error") is THIS function's own job,
 * not `validateTraitImplementation()`'s — that function only sees one
 * trait at a time, so cross-trait collisions can only be detected by the
 * caller that sees all of them. Implemented with one
 * `unordered_map<InternedString, TraitRefAST*>` tracking which `TraitRefAST`
 * first required each field name; a second trait requiring the same name
 * reports E2101 (redeclaration) at that second trait's reference.
 *
 * `validateGenericParamUsage()` runs last, once every field's type (which
 * may reference a generic parameter) has already been resolved.
 *
 * @param decl The struct declaration.
 * @param ctx  Supplies insertion, scoping, and the field/trait/generic-
 *             usage validators implemented elsewhere in `sema/`.
 */
void analyzeStructDecl(StructDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);
    reportIfRedeclared(typeAlreadyDeclaredHere(decl->name, ctx), decl->name, decl, ctx);
    ctx.insertType(decl->name, decl);

    ScopedTypeDefinition defining(ctx, decl);
    ScopedScope generics(ctx);
    for (GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    for (FieldDeclAST* field : decl->fields) {
        analyzeFieldDecl(field, decl, ctx);
    }

    std::unordered_map<InternedString, TraitRefAST*> requiredBy;
    for (TraitRefAST* ref : decl->traitRefs) {
        TraitDeclAST* trait = resolveTraitRef(ref, ctx);
        if (!trait) continue;   // resolveTraitRef() already reported its own error

        validateTraitImplementation(decl, ref, ctx);

        for (TraitFieldDeclAST* tf : trait->fields) {
            auto [it, inserted] = requiredBy.try_emplace(tf->name, ref);
            if (!inserted && it->second != ref) {
                ctx.error(ref, DiagCode::E2101, "redeclaration of '", ctx.toString(tf->name),
                           "' required by multiple traits");
            }
        }
    }

    validateGenericParamUsage(decl->genericParams, decl, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeFieldDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Analyze one struct field: duplicate-name check, resolve its type
 *        (via `checkRecursiveFieldType()`, which also rejects a direct
 *        self-reference), the const/nullable-fallible rule, and — if
 *        present — the default value's type.
 *
 * How it works: `owner->fields` is a fully-populated `ArenaSpan` by the
 * time this runs (the parser already built the whole field list), so the
 * duplicate-name check is a linear scan over `owner->fields` that stops as
 * soon as it reaches `field` itself — every sibling before that point has
 * already been through this same function, so this check alone is enough
 * to catch any duplicate, from whichever of the two duplicate names is
 * processed second.
 *
 * `checkRecursiveFieldType(field, owner, ctx)` (implemented in
 * `sema/rules/Generics.cpp`, not this file) does the actual `field->type`
 * resolution AND the self-reference rejection in one call — see its own
 * doc comment in Sema.hpp; this function's job is everything ELSE a field
 * needs. `field->valueType` is set from the now-resolved `field->type`
 * right after, since caching the resolved type on the `ValueDeclAST` base
 * is this file's general bookkeeping responsibility (see `analyzeVarDecl()`/
 * `analyzeParam()` doing the same), not something the self-reference
 * check itself is responsible for.
 *
 * Const/nullable-fallible: FieldDeclAST's own "Semantic Analysis Notes"
 * rule 3 ("A const field may NOT be nullable or fallible") — reports
 * E3004 if violated (also reused, identically, by `analyzeTraitField()`
 * below, since `TraitFieldDeclAST` states the exact same rule).
 *
 * Default value: if `defaultVal` is present, type-check it and compare
 * against `field->type` with `isAssignable()` (E3003 on mismatch). If it's
 * absent on a `const` field, that's NOT an error here — FieldDeclAST's
 * rule 2 defers that requirement to struct-LITERAL time (`someStruct{...}`
 * must then supply it), which is `checkStructLiteralExpr()`'s job
 * elsewhere, not this declaration-time check's.
 *
 * @param field The field being analyzed.
 * @param owner The struct that declares it (see this function's own
 *              signature note in Sema.hpp for why the owner is required
 *              here but not in the parser's equivalent).
 * @param ctx   Supplies expression checking, type-compatibility helpers,
 *              and the self-reference check.
 */
void analyzeFieldDecl(FieldDeclAST* field, StructDeclAST* owner, SemaContext& ctx) {
    validateAttributes(field->attributes, field, ctx);

    for (FieldDeclAST* existing : owner->fields) {
        if (existing == field) break;
        if (existing->name == field->name) {
            ctx.error(field, DiagCode::E2101, "redeclaration of '", ctx.toString(field->name), "'");
            break;
        }
    }

    checkRecursiveFieldType(field, owner, ctx);
    field->valueType = field->type;

    if (field->isConst && field->type &&
        (isNullableType(field->type) || isFallibleType(field->type))) {
        ctx.error(field, DiagCode::E3004, "'", ctx.toString(field->name),
                   "' must not be nullable or fallible");
    }

    if (field->defaultVal) {
        TypeAST* initType = checkExpr(field->defaultVal, ctx);
        if (field->type && initType && !isAssignable(field->type, initType, ctx)) {
            ctx.error(field->defaultVal, DiagCode::E3003, "type mismatch for field '",
                       ctx.toString(field->name), "'");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeEnumVariant
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Analyze one enum variant: duplicate-name and duplicate-value
 *        checks against every sibling variant, then cache its type.
 *
 * How it works: same "linear scan up to self" pattern as
 * `analyzeFieldDecl()`, over `owner->variants` — but checking two things
 * per sibling instead of one, since `EnumVariantAST` has both a `name`
 * (InternedString) and an explicit `value` (int64_t, required — no
 * auto-increment, see EnumDeclAST's own doc comment) that each need their
 * own uniqueness: E2101 for a duplicate NAME, E3006 for a duplicate VALUE
 * (two different variants explicitly both written `= 2`) — these are
 * independent failures, so both are checked and both can fire for the
 * same pair of variants if source duplicates both.
 *
 * Deliberately does NOT call `ctx.insertValue()` — per EnumVariantAST's
 * own doc comment, variants "live in the value namespace of the enum's
 * scope," accessed as `Direction.North`, not as a bare module/scope-level
 * name; they stay reachable purely through `owner->variants`, the same way
 * struct fields are never independently name-resolvable (see ModuleTable's
 * own doc comment).
 *
 * `variant->valueType` is set to `selfTypeOf(owner, ctx)` — per
 * EnumVariantAST's own "Semantic Cache" note, a variant's type is the enum
 * itself, not the underlying integer.
 *
 * @param variant The variant being analyzed.
 * @param owner   The enum this variant belongs to.
 * @param ctx     Supplies the self-type cache (`selfTypeOf()`).
 */
void analyzeEnumVariant(EnumVariantAST* variant, EnumDeclAST* owner, SemaContext& ctx) {
    validateAttributes(variant->attributes, variant, ctx);

    for (EnumVariantAST* existing : owner->variants) {
        if (existing == variant) break;
        if (existing->name == variant->name) {
            ctx.error(variant, DiagCode::E2101, "redeclaration of '", ctx.toString(variant->name), "'");
        }
        if (existing->value == variant->value) {
            ctx.error(variant, DiagCode::E3006, "duplicate enum value ",
                       std::to_string(variant->value), " (also used by '",
                       ctx.toString(existing->name), "')");
        }
    }

    variant->valueType = selfTypeOf(owner, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeTraitField
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Analyze one trait field requirement: duplicate-name check,
 *        resolve its type, then enforce that it isn't nullable/fallible.
 *
 * How it works: the same "linear scan over `owner->fields` up to self"
 * duplicate-name pattern as `analyzeFieldDecl()`/`analyzeEnumVariant()`
 * (TraitDeclAST's own "Semantic Analysis Notes" rule 1, "Trait Field Name
 * Uniqueness"). Unlike a struct field, a trait field has no default value
 * and no self-reference concern (a trait never contains itself as a field
 * type in any meaningful sense — traits aren't sized types the way structs
 * are), so type resolution here is a direct `resolveType()` call rather
 * than `checkRecursiveFieldType()`. The nullable/fallible rejection
 * (TraitFieldDeclAST's own doc comment: "must not be nullable or
 * fallible") reuses E3004 — the identical rule `analyzeFieldDecl()`
 * enforces for `isConst` struct fields, stated here unconditionally since
 * EVERY trait field forbids it, not just `const` ones.
 *
 * @param field The trait field requirement being analyzed.
 * @param owner The trait this field requirement belongs to.
 * @param ctx   Supplies type resolution and the nullable/fallible checks.
 */
void analyzeTraitField(TraitFieldDeclAST* field, TraitDeclAST* owner, SemaContext& ctx) {
    validateAttributes(field->attributes, field, ctx);

    for (TraitFieldDeclAST* existing : owner->fields) {
        if (existing == field) break;
        if (existing->name == field->name) {
            ctx.error(field, DiagCode::E2101, "redeclaration of '", ctx.toString(field->name), "'");
            break;
        }
    }

    field->type = resolveType(field->type, ctx);

    if (field->type && (isNullableType(field->type) || isFallibleType(field->type))) {
        ctx.error(field, DiagCode::E3004, "'", ctx.toString(field->name),
                   "' must not be nullable or fallible");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeParam
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Analyze one function parameter: redeclaration check, resolve its
 *        type, insert it into the (already-open) function scope.
 *
 * How it works: unlike a field/variant/trait-field, a parameter lives in a
 * real `Scope` (pushed by `analyzeFuncDecl()` before this is ever called —
 * see that function's own doc comment on why every curried group's params
 * share one scope), so the redeclaration check here uses
 * `valueAlreadyDeclaredHere()`, the same tier-scoped check
 * `analyzeVarDecl()`/`analyzeFuncDecl()` use, rather than a sibling scan —
 * catching, for instance, two parameters both named `x` across two
 * different curried groups of the same function, since both land in the
 * one shared scope.
 *
 * @param param The parameter being analyzed.
 * @param ctx   Supplies type resolution and insertion into the current
 *              (function-body) scope.
 */
void analyzeParam(ParamAST* param, SemaContext& ctx) {
    validateAttributes(param->attributes, param, ctx);
    reportIfRedeclared(valueAlreadyDeclaredHere(param->name, ctx), param->name, param, ctx);

    param->type = resolveType(param->type, ctx);
    param->valueType = param->type;
    ctx.insertValue(param->name, param);
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeGenericParamDecl
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Analyze one generic type parameter: resolve its trait
 *        constraints, then insert it into the current scope's generic-
 *        parameter namespace.
 *
 * How it works: `param->constraints` (`<T : Vector2 + Named>`) is a span
 * of `TraitRefAST` — each resolved with `resolveTraitRef()`, which reports
 * its own diagnostic if a constraint doesn't name a real trait, so nothing
 * further is checked here about the constraints themselves (compatibility
 * between multiple constraints on the same parameter, if that's even a
 * real rule, would be `resolveTraitRef()`/`validateTraitImplementation()`
 * territory, not this function's). `ctx.insertGenericParam()` always
 * targets the innermost open `Scope` (see its own doc comment in
 * SemaContext.hpp) — this function does not open that scope itself; the
 * caller (`analyzeFuncDecl()`/`analyzeStructDecl()`/`analyzeTraitDecl()`)
 * already has one open before calling this for each of its
 * `genericParams`.
 *
 * @param param The generic parameter declaration.
 * @param ctx   Supplies constraint resolution and insertion.
 */
void analyzeGenericParamDecl(GenericParamDeclAST* param, SemaContext& ctx) {
    for (TraitRefAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
    ctx.insertGenericParam(param->name, param);
}