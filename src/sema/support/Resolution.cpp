/**
 * @file Resolution.cpp
 * @brief Implements Sema.hpp's "Resolution Helpers (lookup + diagnostic on
 *        failure)" — resolveValueOrError(), resolveTypeNameOrError(),
 *        resolveCalleeOrError(), selfTypeOf().
 *
 * @architectural_note Three shapes, one general code each
 *   Every failure in this file reduces to one of three shapes: the name
 *   didn't resolve to a value at all (E2001), it didn't resolve to a type
 *   at all (E2002), or it resolved to a value that isn't callable (E2003).
 *   That's true whether the name came from a bare identifier or a
 *   `module:member` qualifier — see resolveValueOrError()/
 *   resolveCalleeOrError() below reusing the same E2001 for both. No
 *   specialized 2101+ code has been needed yet; see DiagnosticCodes.hpp's
 *   2001–2100/2101–3000 sub-split note.
 *
 * @note '// X' marks resolveCalleeOrError()'s handling of a callee that
 *       isn't a plain identifier or `module:member` access — see that
 *       function's own comment for why it's out of scope here.
 */

#include "../Sema.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// resolveValueOrError
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve a bare identifier to the value declaration it names.
 *
 * How it works: delegates entirely to `ctx.lookupValue()` (innermost-to-
 * outermost open scopes, then the current module's top-level table — see
 * SemaContext.hpp's own doc comment on that search order). If that returns
 * something, this function just hands it back — no further checking, since
 * `lookupValue()` doesn't care whether the result is a variable, function,
 * parameter, field, or enum variant; all of those share one namespace (see
 * ValueDeclAST's doc comment in BaseAST.hpp).
 *
 * If the lookup comes back empty, that's where this function earns its
 * name over calling `lookupValue()` directly: it reports the "undefined
 * value" diagnostic (E2001) at `expr`'s own location before returning
 * `nullptr`, so every call site gets that diagnostic for free instead of
 * each one having to remember to report it themselves.
 *
 * @param expr The identifier expression being resolved (e.g. the `x` in
 *             `x + 1`).
 * @param ctx  Supplies the lookup (`lookupValue()`) and the diagnostic
 *             sink (`error()`) this function is built from.
 * @return The resolved declaration, or `nullptr` if `expr->name` isn't in
 *         scope (an E2001 diagnostic has already been reported in that
 *         case — callers do not need to report their own).
 */
ValueDeclAST* resolveValueOrError(IdentifierExprAST* expr, SemaContext& ctx) {
    if (ValueDeclAST* decl = ctx.lookupValue(expr->name)) {
        return decl;
    }
    ctx.error(expr, DiagCode::E2001, "undefined value '", ctx.toString(expr->name), "'");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTypeNameOrError
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve a named type reference (`Vec2`, `Buffer<int>`, ...) to the
 *        declaration it names, treating a generic-parameter match as a
 *        silent non-error rather than a failure.
 *
 * How it works: this is a two-step lookup, tried in order, because a bare
 * name in a type position can mean two different things depending on
 * context — a concrete declared type (`struct`/`enum`/`trait`), or a
 * generic parameter of the enclosing declaration (the `T` in
 * `struct Box<T> { value T }`):
 *
 *   1. `ctx.lookupType(type->name)` — searches scopes then the module
 *      table for a real `TypeDeclAST`. Found → return it immediately, done.
 *   2. Not found as a type? Check `ctx.lookupGenericParam(type->name)`.
 *      If that finds something, the name is valid — it's just not a
 *      `TypeDeclAST` (`GenericParamDeclAST` is a separate hierarchy, see
 *      its own doc comment in BaseAST.hpp), so there is nothing to return
 *      but `nullptr`, and — importantly — no diagnostic is reported. The
 *      name resolved, just not to the type this function's return type can
 *      express.
 *   3. Neither found anything → this is the actual failure case. Reports
 *      "undefined type" (E2002) at `type`'s location, then returns
 *      `nullptr`.
 *
 * Because steps 2 and 3 both return `nullptr`, a caller that needs to tell
 * "it's a generic parameter" apart from "it's genuinely undefined" (e.g.
 * to set a semantic annotation like `NamedTypeAST::isGenericParam`) has to
 * call `ctx.lookupGenericParam()` itself rather than infer it from this
 * function's return value alone — see `resolveNamedType()` in
 * `sema/rules/SemaType.cpp` for that caller.
 *
 * @param type The named-type AST node being resolved.
 * @param ctx  Supplies both lookups and the diagnostic sink.
 * @return The resolved `TypeDeclAST`, or `nullptr` — either because
 *         `type->name` is a generic parameter (no diagnostic reported) or
 *         because it's genuinely undefined (E2002 reported).
 */
TypeDeclAST* resolveTypeNameOrError(NamedTypeAST* type, SemaContext& ctx) {
    if (TypeDeclAST* decl = ctx.lookupType(type->name)) {
        return decl;
    }

    // Resolves to a generic parameter, not a concrete type declaration --
    // valid (see GenericParamDeclAST's own doc comment: generic params are
    // a separate hierarchy from TypeDeclAST, so there's no TypeDeclAST to
    // hand back here), but NOT an error. The caller distinguishes this
    // case from genuine failure by calling ctx.lookupGenericParam() itself
    // when it needs to know which case a nullptr return means — e.g.
    // resolveNamedType() in sema/rules/SemaType.cpp setting
    // NamedTypeAST::isGenericParam.
    if (ctx.lookupGenericParam(type->name) != nullptr) {
        return nullptr;
    }

    ctx.error(type, DiagCode::E2002, "undefined type '", ctx.toString(type->name), "'");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveCalleeOrError
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve a call expression's callee to the `FuncDeclAST` it names,
 *        for the two callee shapes that actually name a declaration.
 *
 * How it works: `callee` can be several different AST shapes depending on
 * what's being called, so this function branches on `callee->kind` (via
 * `isa<T>()`) and only two branches actually resolve to something:
 *
 *   - **`IdentifierExprAST`** (`foo(...)`): look the name up with
 *     `ctx.lookupValue()`, exactly like `resolveValueOrError()` does. Two
 *     ways this can fail, each with its own diagnostic: the name isn't in
 *     scope at all (E2001, same "undefined value" as any other unresolved
 *     identifier), or it IS in scope but names something that isn't a
 *     `FuncDeclAST` — a variable or parameter, say (E2003, "not callable").
 *
 *   - **`ModuleAccessExprAST`** (`module:member(...)`): the same two-
 *     outcome check, just reached through an extra hop — resolve
 *     `moduleName` to a `ModuleAST*` via `ctx.lookupImport()`, then look
 *     `memberName` up directly in that module's own `ModuleTable` (bypasses
 *     `ctx.lookupValue()` entirely, since that function deliberately never
 *     crosses into another module's table — see its own doc comment). If
 *     the module alias doesn't resolve, or the member doesn't exist in it,
 *     that's still reported as E2001 (using `module:member` as the
 *     "undefined value" name); if the member exists but isn't a function,
 *     that's E2003, same as the plain-identifier case.
 *
 *   - **Anything else** — a curried call's inner callee (the first `(1)`
 *     in `makeAdder(1)(2)`) or an immediately-invoked function literal —
 *     doesn't name a declaration at all; it's an *expression* whose
 *     *value* happens to be callable. This function has no lookup that
 *     could resolve that, so it returns `nullptr` silently (no diagnostic)
 *     and leaves it to the caller (`checkCallExpr()`) to fall back to
 *     checking `callee`'s own resolved type is a `FuncTypeAST` instead —
 *     see the `// X` note at that branch for why that check doesn't belong
 *     in this file.
 *
 * @param callee The callee expression of a `CallExprAST`.
 * @param ctx    Supplies the lookups (`lookupValue()`, `lookupImport()`,
 *               `findModuleTable()`) and the diagnostic sink.
 * @return The resolved `FuncDeclAST`, or `nullptr` — with an E2001/E2003
 *         diagnostic already reported for the two named-callee shapes, and
 *         no diagnostic at all for any other callee shape (see above).
 */
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx) {
    // Plain call: `foo(...)`.
    if (callee->isa<IdentifierExprAST>()) {
        InternedString name = callee->as<IdentifierExprAST>()->name;
        ValueDeclAST* value = ctx.lookupValue(name);
        if (!value) {
            ctx.error(callee, DiagCode::E2001, "undefined value '", ctx.toString(name), "'");
            return nullptr;
        }
        if (!value->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003, "'", ctx.toString(name), "' is not callable");
            return nullptr;
        }
        return value->as<FuncDeclAST>();
    }

    // Cross-module call: `module:member(...)` — see Grammar.md's
    // ModuleAccessExprAST examples (`math:sqrt(x)`, `std:io:printl("hi")`).
    if (callee->isa<ModuleAccessExprAST>()) {
        ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();

        ModuleAST* mod = ctx.lookupImport(access->moduleName);
        ModuleTable* table = mod ? ctx.findModuleTable(mod) : nullptr;
        ValueDeclAST* value = nullptr;
        if (table) {
            auto it = table->values.find(access->memberName);
            if (it != table->values.end()) {
                value = it->second;
            }
        }

        if (!value) {
            ctx.error(callee, DiagCode::E2001, "undefined value '",
                       ctx.toString(access->moduleName), ":", ctx.toString(access->memberName), "'");
            return nullptr;
        }
        if (!value->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003, "'", ctx.toString(access->moduleName), ":",
                       ctx.toString(access->memberName), "' is not callable");
            return nullptr;
        }
        return value->as<FuncDeclAST>();
    }

    // Any other callee shape — the result of a curried call
    // (`makeAdder(1)(2)`, where the inner callee is itself a CallExprAST
    // producing a function value, not naming a declaration) or an
    // immediately-invoked AnonFuncExprAST literal — doesn't name a
    // FuncDeclAST at all; there is no declaration to return. Whether it's
    // callable depends on the callee EXPRESSION's own resolved type being
    // a FuncTypeAST, which is checkExpr's job (it must already have type-
    // checked `callee` before calling this), not a lookup this function
    // can do. checkCallExpr() is expected to fall back to that type-based
    // check itself when this returns nullptr for a non-named callee — see
    // FuncTypeAST's own doc comment in TypeAST.hpp for the curried shape
    // such a check would be looking for.                              // X
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// selfTypeOf
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Get (building it once, if needed) the `NamedTypeAST` that refers
 *        back to `decl` itself.
 *
 * How it works: this is a lazy cache read/populate, not a lookup — nothing
 * is searched. `TypeDeclAST::selfType` (declared `mutable` in BaseAST.hpp
 * specifically so a `const TypeDeclAST*` could populate it too, though
 * this function only ever sees a non-const one) starts out `nullptr` for
 * every struct/enum/trait declaration:
 *
 *   1. Already built? (`decl->selfType != nullptr`) Return the cached
 *      pointer directly — this is the common case after the first call.
 *   2. Not built yet? Allocate a new `NamedTypeAST` from the arena
 *      (`ctx.arena.makeType<NamedTypeAST>(decl->name)`), whose `name`
 *      is `decl`'s own name — so `Point`'s self-type is a `NamedTypeAST`
 *      that reads exactly like a normal reference to `Point` written
 *      elsewhere. Its location is set to `decl->loc` (not the call site)
 *      so a diagnostic pointing at this synthetic node lands on the
 *      original declaration, which is the only source location that
 *      actually makes sense for it. Cache it on `decl->selfType`, then
 *      return it.
 *
 * Every subsequent call for the same `decl` — however many times a type's
 * own name is used as a value elsewhere in the program (see TypeDeclAST's
 * "Self-Type Cache" doc comment in BaseAST.hpp for the motivating
 * `int("42")`-style example) — takes the step-1 fast path and allocates
 * nothing further.
 *
 * @param decl The type declaration to build/fetch a self-type reference for.
 * @param ctx  Supplies the arena the (at most one) new node is allocated from.
 * @return A `NamedTypeAST` naming `decl`, stable across every call for the
 *         same `decl` (same pointer every time, not just an equal one).
 */
NamedTypeAST* selfTypeOf(TypeDeclAST* decl, SemaContext& ctx) {
    if (decl->selfType) {
        return decl->selfType;
    }

    NamedTypeAST* self = ctx.arena.makeType<NamedTypeAST>(decl->name);
    self->loc = decl->loc;  // point diagnostics at the declaration itself
    decl->selfType = self;  // TypeDeclAST::selfType is `mutable` — cached
                             // lazily by design, see its own doc comment
                             // in BaseAST.hpp.
    return self;
}