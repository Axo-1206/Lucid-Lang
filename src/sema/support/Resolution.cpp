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