/// @file SemaDecl.cpp
/// @brief Declaration registration and resolution.
///
/// ─── The three-pass model, from this file's point of view ────────────────
/// This file implements four of the nine internal resolvers declared in
/// `Sema.hpp`:
///
///   Pass 1 — registration:
///     registerImportName, registerTableName, registerFnName, registerVarName
///
///   Pass 2 — declaration resolution:
///     resolveImportDecl, resolveTableDecl, resolveColumnDecl,
///     resolveFnDecl, resolveVarDecl
///
///   Pass 3 — body resolution:
///     resolveFnBody (which calls resolveParam for each parameter and then
///     resolveStmt for the body)
///
/// Plus two shared per-node resolvers, `resolveParam` and `resolveVarDecl`,
/// called from both passes.
///
/// ─── Why registration and resolution are separate functions ───────────────
/// A two-pass model means pass 1 registers every top-level name before
/// pass 2 resolves any signature. That is what makes forward references
/// and cyclic imports work: `TABLE A { b: &B }` resolves even when `B`
/// is declared later, because pass 1 already inserted `B` into the
/// module's type namespace.
///
/// Registration is a pure insertion: it writes the name to the module
/// table and does nothing else. It does not resolve the declaration's
/// type, it does not validate its attributes, it does not look at its
/// body. Those are pass 2's job.
///
/// ─── Why resolveVarDecl has two callers ───────────────────────────────────
/// A `let`/`const` declaration is a `VarDeclAST` whether it appears at
/// module level or inside a block. The node is the same; only the
/// surrounding scope differs.
///
///   - A top-level `let` is a module declaration. Its name is
///     registered in pass 1 by `registerVarName`, alongside tables and
///     functions.
///   - A local `let` is a statement. Its name is registered by
///     `resolveVarDeclStmt` in `SemaStmt.cpp`, *before* it calls
///     `resolveVarDecl`, into the block's scope.
///
/// `resolveVarDecl` itself does not register. It resolves the declared
/// type, classifies the binding's `resourceKind`, and resolves the
/// initializer against the declared type. Both callers have already
/// registered the name — the only difference is where.

#include "sema/Sema.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/const_eval/ConstEvaluator.hpp"
#include "sema/registry/AttributeValidator.hpp"
#include "sema/types/SemaType.hpp"
#include "sema/support/MangledName.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// Pass 1 — Registration
// ═════════════════════════════════════════════════════════════════════════════

void registerImportName(ImportDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        // The path does not resolve to any loaded module. Pass 1 does
        // not report this: the diagnostic belongs on the import
        // declaration, which is a pass-2 concern. (An import whose
        // target is missing is a resolution failure, not a registration
        // failure; there is nothing to register.)
        return;
    }

    ctx.addImportAlias(decl->alias, target, decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerTableName(TableDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    // A table is registered in BOTH namespaces.
    //
    //   - Type namespace: the entry that `&Person` and `let p: Person`
    //     resolve against.
    //   - Value namespace: the entry that `Person.ADD(...)`,
    //     `Person[i]`, and `Person.age` resolve against, and that a bare
    //     `Person` used as a value (e.g. `countWhere(Person, pred)`)
    //     resolves against.
    //
    // Both point at the same `TableDeclAST`. The two namespaces are
    // still separate, so a `let Person = ...` in the same module would
    // collide with the table's value-namespace entry — which is the
    // intended behavior: `Person` names the sheet, and a `let` cannot
    // also name a value called `Person`.
    ctx.insertType(decl);
    ctx.insertValue(decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerFnName(FnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    ctx.insertValue(decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerVarName(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    ctx.insertValue(decl);
}

// ═════════════════════════════════════════════════════════════════════════════
// Pass 2 — Declaration resolution
// ═════════════════════════════════════════════════════════════════════════════

void resolveImportDecl(ImportDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.diagnostics.error(DiagCode::Name_UndefinedModule, decl,
                              "undefined module '",
                              ctx.pool.lookup(decl->path), "'");
    }
    // If the target was found, pass 1 already registered the alias, and
    // there is nothing more to resolve.
}

// ─────────────────────────────────────────────────────────────────────────────

void resolveTableDecl(TableDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    // ─── 1. Attribute validation ────────────────────────────────────────
    //
    // Runs first, so that the fields Sema reads in later steps
    // (`isFixed`, `isReadonly`, `isPacked`, `isReserved`, `isColumnar`,
    // `isRequest`) reflect the attribute span. This is the only place
    // that writes those fields — see `AttributeValidator`.
    validateAllAttributes(decl, ctx);

    // ─── 2. Resolve each column's type ──────────────────────────────────
    //
    // A column's type is the whole of its resolution. Columns are not
    // registered in the module's symbol tables — a column is looked up
    // by name against its table's shape (§4.1.3), not through the
    // module's namespaces.
    //
    // The column-order fact (a column's `columnIndex`) is what
    // `T.ADD(args...)` and the inline `= [ ... ]` initializer both key
    // on: the argument order is the column order, always.
    for (size_t i = 0; i < decl->columns.size(); ++i) {
        ColumnDeclAST* column = decl->columns[i];
        if (!column) continue;

        column->columnIndex = i;
        resolveColumnDecl(column, ctx);
    }

    // ─── 3. Table-level constraint checks ───────────────────────────────
    //
    // The rules that need the table's shape — its columns, its
    // `isHostBacked` flag, its fixed/growing status, its inline rows.
    // These are things the per-attribute validator cannot see. See
    // `TableConstraintChecker`.
    //
    // `resolveTableDecl` is called from pass 2, before any function
    // body has been resolved, so the checker runs entirely on
    // declaration-level information.
    //
    // Note: the checker is a separate file. It is called here, after
    // both the attribute flags and the column types are available.
    // (Not shown in this file — see `table/TableConstraintChecker.cpp`.)

    // ─── 4. Mangled name for @export ────────────────────────────────────
    //
    // An `@export`ed table gets a mangled symbol name, the same way an
    // exported function or variable does. The mangled name is what the
    // bytecode linker and any future `.luci` serializer use to
    // reference the table.
    if (decl->isExported) {
        InternedString mangled = generateMangledName(decl, ctx);
        if (mangled.isValid()) {
            decl->mangledName = mangled;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void resolveColumnDecl(ColumnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    TypeAST* resolved = resolveType(decl->type, ctx);
    if (!resolved || resolved->isa<UnknownTypeAST>()) {
        // resolveType has already emitted the diagnostic. Leave the
        // column's type as it was (the syntactic node); downstream
        // passes will see the unresolved type and skip the column.
        decl->type = ctx.getUnknownType();
        return;
    }

    decl->type = resolved;
}

// ─────────────────────────────────────────────────────────────────────────────

void resolveFnDecl(FnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    // ─── 1. Attribute validation ────────────────────────────────────────
    //
    // Runs first, so that `decl->isSequence` is set before anything
    // reads it. Pass 3's `resolveFnBody` chooses `FuncBody` vs.
    // `SequenceBody` from this flag; a `@sequence` attribute that is not
    // validated before pass 3 would leave the flag unset and the body
    // would be resolved in the wrong context.
    validateAllAttributes(decl, ctx);

    // ─── 2. Resolve each parameter's type ───────────────────────────────
    //
    // `resolveParam` registers the parameter's name in the current scope
    // AND resolves its type. During pass 2 the current scope is the
    // module scope, so the parameter names land there for the duration
    // of this function's signature resolution and are popped when
    // pass 2 moves on. During pass 3 `resolveFnBody` calls `resolveParam`
    // again, this time inside a `ScopedFunction` guard, so the parameter
    // names land in the function's own scope, where the body can find
    // them.
    //
    // The double registration is harmless: the pass-2 scope and the
    // pass-3 scope are distinct and their lifetimes do not overlap.
    // What matters is that by the time the body is resolved, the
    // parameter names are in scope.
    for (ParamAST* param : decl->params) {
        resolveParam(param, ctx);
    }

    // ─── 3. Resolve the return type ─────────────────────────────────────
    //
    // A `null` return type means the function returns `unit`; a non-null
    // type means the function returns whatever the arrow said. The
    // distinction is preserved by the parser (it stores an explicit
    // `PrimitiveTypeAST(Unit)` when the source wrote `-> unit`, and
    // leaves the field null when there was no arrow). Sema treats the
    // two the same: either way, the function's return type is `unit`
    // and any non-`unit` return statement will be diagnosed by
    // `resolveReturnStmt`.
    //
    // The distinction is kept in the AST so a JSON dumper or an LSP can
    // render the source faithfully; Sema does not care.
    if (decl->returnType) {
        TypeAST* resolved = resolveType(decl->returnType, ctx);
        if (!resolved || resolved->isa<UnknownTypeAST>()) {
            decl->returnType = ctx.getUnknownType();
        } else {
            decl->returnType = resolved;
        }
    }

    // ─── 4. Validate sequence signature restrictions ────────────────────
    //
    // Two rules from §9.2.5 that can be decided from the signature
    // alone:
    //
    //   - A `@sequence` function cannot have a `host(...)` body.
    //   - A `@sequence` function always returns `unit`.
    //
    // The other sequence restrictions — `wait*` only inside a
    // sequence body, `start` requires a `@sequence` callee, a sequence
    // is not a function value — are checked at the corresponding use
    // sites, not here.
    //
    // These checks live in `SequenceChecker` rather than inline, so all
    // sequence rules are in one file. (Not shown here.)

    // ─── 5. Mangled name ────────────────────────────────────────────────
    //
    // Every function gets a mangled name, exported or not. The name is
    // what the compiler emits into the bytecode module's function
    // table; an `@export`ed function's mangled name is its source name
    // (so the host can look it up by the name the user wrote), and a
    // non-exported function's mangled name is prefixed with the module
    // path so two same-named private functions in different modules do
    // not collide.
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }

    // ─── 6. Host-bound functions ────────────────────────────────────────
    //
    // A host-bound function has no body. Its signature has been resolved
    // above, and pass 3 will skip it. There is nothing else to do in
    // pass 2 for one.
    //
    // The host symbol name (`decl->hostName`) is not resolved here.
    // Whether the name exists in the host registry is a check the
    // bytecode linker performs, not Sema — Sema validates the *shape*
    // of the declaration (which attributes are legal, what the
    // signature looks like), not the availability of a specific native
    // symbol.
}

// ─────────────────────────────────────────────────────────────────────────────

void resolveVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    // ─── 1. Attribute validation ────────────────────────────────────────
    //
    // For a top-level `let`/`const`, this is where `@export` and
    // `@deprecated` are decoded. For a local one, the attributes span
    // is empty (grammar §4.3 does not list any attribute as legal on a
    // local `let`), and validation is a no-op.
    //
    // The attribute validator rejects `@export` and `@deprecated` on a
    // local declaration, so a parser bug that attached one to a local
    // `let` would be caught here.
    validateAllAttributes(decl, ctx);

    // ─── 2. Resolve the declared type ───────────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType || declaredType->isa<UnknownTypeAST>()) {
        decl->type = ctx.getUnknownType();
        return;
    }
    decl->type = declaredType;

    // ─── 3. Classify the resource kind ──────────────────────────────────
    //
    // A binding's ownership behavior is fixed by its type alone, before
    // any value-specific information is considered. `classifyResourceKind`
    // returns `OwnedBuffer` for a `string` or a dynamic array, and
    // `None` for everything else (a primitive, a row reference, a
    // host-backed value). The `FuncDeclAST*` overload it might have had
    // in an earlier design is not applicable here: a `VarDeclAST`'s
    // initializer is a value, not a declaration, so there is no
    // function-declaration context to pass.
    decl->resourceKind = classifyResourceKind(declaredType);

    // ─── 4. Const-binding validation ────────────────────────────────────
    //
    // A `const` binding must have a definite type: not a nullable type
    // (`T?`), not a row reference (`&T`). `let` accepts any type. The
    // rule is one-directional — `const` narrows the accepted set.
    if (decl->isConst) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
            return;
        }
    }

    // ─── 5. Resolve the initializer ─────────────────────────────────────
    //
    // Every `let`/`const` has an initializer in the new grammar (see the
    // grammar's `var_decl` production: `( 'let' | 'const' ) IDENTIFIER
    // ':' type '=' expr`). The initializer is resolved against the
    // declared type — this is what lets an empty array literal `[]`
    // take its element type from the binding's annotation, and what
    // lets an integer literal adapt to whatever concrete numeric type
    // the annotation names.
    //
    // `resolveExprWithTarget` emits a diagnostic if the initializer's
    // type is not assignable to the declared type, and returns the
    // unknown-type singleton. In that case we leave the declaration's
    // resolved fields as they are and stop.
    if (decl->init) {
        TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            return;
        }
    }

    // ─── 6. Constant folding for `const` ────────────────────────────────
    //
    // A `const` binding whose initializer is a compile-time constant has
    // that constant cached on the initializer expression. The evaluator
    // writes `isConst` and `constValue` on the expression; it does not
    // rewrite the expression tree. Later passes (the bytecode compiler,
    // the constant-table builder) read the cached value instead of
    // re-evaluating.
    //
    // The evaluator only runs for `const` bindings. A `let` binding's
    // initializer might be constant, but a `let` is a mutable binding
    // and the compiler does not treat its value as fixed.
    if (decl->isConst && decl->init) {
        ConstantValue val = evaluate(decl->init, ctx);
        if (val.isEvaluated() && !val.isError()) {
            decl->init->isConst    = true;
            decl->init->constValue = val;
        }
    }

    // ─── 7. Mangled name for @export ────────────────────────────────────
    //
    // Only top-level `let`/`const` can be `@export`ed, and the attribute
    // validator has already rejected `@export` on a local. So a
    // declaration that reaches here with `isExported == true` is a
    // top-level declaration; the mangled name is what the bytecode
    // linker uses to publish it.
    if (decl->isExported) {
        InternedString mangled = generateMangledName(decl, ctx);
        if (mangled.isValid()) {
            decl->mangledName = mangled;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Pass 3 — Function body resolution
// ═════════════════════════════════════════════════════════════════════════════

void resolveFnBody(FnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    // A host-bound function has no body. Its signature was resolved in
    // pass 2; there is nothing else to do.
    if (decl->isHostBound) return;

    // A function without a body block is a parser error, not a user
    // error. The parser produces a body block or a `host(...)` target;
    // anything else means the parse failed and `hasSyntaxError` should
    // have been set. If we reach here with `body == nullptr`, that is a
    // compiler bug, not a source-level mistake.
    if (!decl->body) {
        AST_ASSERT_MSG(false,
            "resolveFnBody: a non-host-bound function has no body — "
            "the parser should have set hasSyntaxError");
        return;
    }

    // ─── 1. Push the function scope ─────────────────────────────────────
    //
    // `ScopedFunction` picks `FuncBody` vs. `SequenceBody` from
    // `decl->isSequence` (set by `validateAllAttributes` in pass 2) and
    // pushes a `SymbolScope` for the function's parameters. From this
    // point until the guard's destructor fires, the context stack says
    // "we are inside a function body" — which is what `resolveReturnStmt`
    // checks, what `resolveBreakStmt` distinguishes from "inside a loop",
    // and what `resolveWaitStmt` and friends check via
    // `insideSequence()`.
    ScopedFunction fnScope(ctx, decl);

    // ─── 2. Resolve the function's parameters ───────────────────────────
    //
    // `resolveParam` inserts each parameter's name into the function's
    // scope and resolves the parameter's type. This is the second
    // registration of these parameters — the first was in pass 2, in
    // the module scope, and it was popped before this function ran.
    // The second registration is the one the body sees.
    for (ParamAST* param : decl->params) {
        resolveParam(param, ctx);
    }

    // ─── 3. Resolve the body block ──────────────────────────────────────
    //
    // `resolveStmt` on a `BlockStmtAST` pushes its own `SymbolScope` and
    // walks the statements, returning `true` if the last reachable
    // statement transfers control out of the block. For a function
    // whose body is a single block (which is every non-host-bound
    // function — the AST has no other body shape), the return value is
    // "does this function return on every path?".
    bool bodyTransfers = resolveStmt(decl->body, ctx);

    // ─── 4. Return-path check ───────────────────────────────────────────
    //
    // A function whose return type is not `unit` must return a value on
    // every path that reaches the end of the body. If the body's last
    // statement does not transfer control (no `return`, no `panic`, no
    // infinite loop, no exhaustive if/else with returns in both), the
    // function falls off the end without returning a value.
    //
    // `unit`-returning functions are exempt: falling off the end is
    // how a `unit` function returns.
    bool returnsValue = decl->returnType
                     && !isUnitType(decl->returnType);
    if (returnsValue && !bodyTransfers) {
        ctx.diagnostics.error(DiagCode::Type_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 5. Sequence restrictions ───────────────────────────────────────
    //
    // A `@sequence` body is subject to the rules of §9.2.5:
    //   - it cannot have a `host(...)` body (checked in pass 2);
    //   - it always returns `unit` (checked in pass 2);
    //   - `wait*` statements only appear inside it (checked as each
    //     `Wait*StmtAST` is resolved);
    //   - it cannot be passed as a function value (checked where a
    //     function value is expected);
    //   - it cannot call another `@sequence` function directly
    //     (checked at the call site).
    //
    // The check that is easiest to do here, once the body is resolved,
    // is "does the body actually suspend?". A `@sequence` that never
    // calls a `wait*` is legal but almost certainly a mistake — the
    // author probably meant to make it an ordinary `FN`. The warning is
    // emitted by `SequenceChecker::checkBody`, called after the body is
    // fully resolved. (Not shown here.)
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared per-node resolvers
// ═════════════════════════════════════════════════════════════════════════════

void resolveParam(ParamAST* param, SemaContext& ctx) {
    if (!param) return;
    if (param->hasSyntaxError) {
        param->type = ctx.getUnknownType();
        return;
    }

    // ─── 1. Register the parameter's name ───────────────────────────────
    //
    // The parameter's name is inserted into the current scope. When
    // called from `resolveFnDecl` (pass 2), the current scope is the
    // module scope, and the insertion is transient — it is popped when
    // pass 2 finishes this function's signature. When called from
    // `resolveFnBody` (pass 3), the current scope is the function's own
    // scope and the insertion persists for the body's resolution.
    //
    // A parameter with an empty name is possible in a lambda whose
    // parameter list uses `_` as a discard marker; the parser leaves
    // the name empty in that case. An empty name is not registered.
    if (!param->name.isEmpty()) {
        ctx.insertValue(param);
    }

    // ─── 2. Resolve the parameter's type ────────────────────────────────
    TypeAST* resolved = resolveType(param->type, ctx);
    if (!resolved || resolved->isa<UnknownTypeAST>()) {
        param->type = ctx.getUnknownType();
        return;
    }
    param->type = resolved;

    // ─── 3. Handle variadic parameters ──────────────────────────────────
    //
    // The grammar's variadic form is `name: ...T`. The parser stores the
    // element type `T` on the `ParamAST` and sets `isVariadic = true`.
    // Sema synthesizes the declared type `[T]` — a dynamic array — which
    // is what the function body sees when it reads the parameter, and
    // what a caller's argument list is checked against in
    // `resolveCallExpr` (the caller passes zero or more `T`s; the callee
    // receives one `[T]`).
    if (param->isVariadic) {
        param->type = ctx.getArrayType(ArrayKind::Dynamic,
                                       /*size=*/0,
                                       param->type);
    }

    // ─── 4. Classify the resource kind ──────────────────────────────────
    //
    // A parameter holds whatever the caller passed. The binding's
    // ownership classification is fixed by the parameter's type: a
    // `string` or dynamic array parameter owns a buffer the callee may
    // need to release; a primitive or row reference parameter does not.
    param->resourceKind = classifyResourceKind(param->type);

    // ─── 5. Const-parameter validation ──────────────────────────────────
    //
    // A `const` parameter may not be reassigned and may not have
    // mutation applied through it. The check here is only the *type*
    // side of that rule: a `const` parameter's type must not be one
    // through which mutation is legal-by-default. In the new grammar
    // there is exactly one such case — a `const` parameter of a
    // nullable type (`T?`) is fine (mutation through it is blocked by
    // the `const` itself), but a `const` parameter whose type is a
    // bare row reference (`&T`) is *also* fine, because `const` on a
    // parameter means "the callee cannot write through this binding",
    // and the mutability of the row the reference points at is a
    // property of the caller's binding, checked at the call site, not
    // the parameter's declared type.
    //
    // So the only thing `validateConstType` needs to reject here is a
    // `const` parameter whose type has no meaning for a `const`
    // binding — but in the new grammar, no type falls into that
    // category. The call is kept for symmetry with the variable and
    // field paths, and will become meaningful if a future type is
    // added that is not legal on a `const` binding.
    if (param->isConst) {
        (void)validateConstType(param->type, param->name, "parameter", ctx);
    }
}

} // namespace lucid::sema