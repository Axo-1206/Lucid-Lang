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
/// `resolveVarDecl` itself does not register. It resolves the
/// initializer against the declared type. Both callers have already
/// registered the name — the only difference is where.

#include "sema/Sema.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/const_eval/ConstEvaluator.hpp"
#include "sema/registry/AttributeValidator.hpp"
#include "sema/types/SemaType.hpp"
#include "sema/support/MangledName.hpp"
#include "sema/support/TableConstraintChecker.hpp"
#include "sema/support/SequenceChecker.hpp"

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
        // declaration, which is a pass-2 concern.
        return;
    }

    ctx.addImportAlias(decl->alias, target, decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerTableName(TableDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    // A table is registered in BOTH namespaces, and the context's
    // `insertTable` performs both writes in one call.
    //
    //   - Type namespace (`types`): the entry that `&Person` and
    //     `let p: Person` resolve against.
    //   - Value namespace (`tables`): the entry that `Person.ADD(...)`,
    //     `Person[i]`, and a bare `Person` used as a value resolve
    //     against.
    //
    // Both maps hold the same `TableDeclAST*`. The two namespaces are
    // still separate: a `let Person = ...` in the same module collides
    // with the table's value-namespace entry, which is the intended
    // behavior — `Person` names the sheet, and a `let` cannot also
    // name a value called `Person`.
    ctx.insertTable(decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerFnName(FnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    ctx.insertFunction(decl);
}

// ─────────────────────────────────────────────────────────────────────────────

void registerVarName(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    ctx.insertVariable(decl);
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
    // See `TableConstraintChecker`.
    //
    // Called here, after both the attribute flags and the column types
    // are available.
    checkTableConstraints(decl, ctx);

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
        // column's type as the unknown singleton; downstream passes
        // will see the unresolved type and skip the column.
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
    // `SequenceBody` from this flag.
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
    for (ParamAST* param : decl->params) {
        resolveParam(param, ctx);
    }

    // ─── 3. Resolve the return type ─────────────────────────────────────
    //
    // A `null` return type means the function returns `unit`; a non-null
    // type means the function returns whatever the arrow said.
    if (decl->returnType) {
        TypeAST* resolved = resolveType(decl->returnType, ctx);
        if (!resolved || resolved->isa<UnknownTypeAST>()) {
            decl->returnType = ctx.getUnknownType();
        } else {
            decl->returnType = resolved;
        }
    }

    // ─── 4. Sequence signature restrictions ─────────────────────────────
    //
    // Two rules from §9.2.5 that can be decided from the signature
    // alone: a `@sequence` returns `unit`, and a `@sequence` is not
    // host-bound.
    checkSequenceSignature(decl, ctx);

    // ─── 5. Mangled name ────────────────────────────────────────────────
    //
    // Every function gets a mangled name, exported or not. The name is
    // what the compiler emits into the bytecode module's function
    // table; an `@export`ed function's mangled name is its source name,
    // and a non-exported function's mangled name is prefixed with the
    // module path.
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void resolveVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    // ─── 1. Attribute validation ────────────────────────────────────────
    //
    // For a top-level `let`/`const`, this is where `@export` and
    // `@deprecated` are decoded. For a local one, the attributes span
    // is empty and validation is a no-op.
    validateAllAttributes(decl, ctx);

    // ─── 2. Resolve the declared type ───────────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType || declaredType->isa<UnknownTypeAST>()) {
        decl->type = ctx.getUnknownType();
        return;
    }
    decl->type = declaredType;

    // ─── 3. Const-binding validation ────────────────────────────────────
    //
    // A `const` binding must have a definite type: not a nullable type
    // (`T?`), not a row reference (`&T`). `let` accepts any type.
    if (decl->isConst) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
            return;
        }
    }

    // ─── 4. Resolve the initializer ─────────────────────────────────────
    //
    // The initializer is resolved against the declared type, so that an
    // empty array literal `[]` takes its element type from the binding's
    // annotation and an integer literal adapts to the concrete numeric
    // type the annotation names.
    //
    // A parser error-recovery node — a `let`/`const` whose source was
    // missing the `= expr` — never reaches here; the `hasSyntaxError`
    // guard at the top of this function returned it early. So `decl->init`
    // is always non-null by the time this runs, and no defensive check
    // is needed.
    TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
    if (!initType || initType->isa<UnknownTypeAST>()) {
        return;
    }

    // ─── 5. Top-level initializer must be a `const_expr` ────────────────
    //
    // The rule (grammar §3.4 and §4.3): a top-level `let`/`const` binding
    // starts at a value the compiler knows. Nothing runs at module load
    // time, so the initializer must be a compile-time constant — the
    // same `const_expr` shape fixed-table rows use (§4.1.1c).
    //
    // A local `let` inside a function body has no such restriction: its
    // initializer evaluates when the function runs, which is real
    // execution, not load-time evaluation.
    //
    // The check is decided by `isModuleLevelDeclaration`, which asks
    // whether this declaration is one of the module's top-level `decls`.
    // It is the same helper `AttributeValidator` uses to enforce
    // `@export`'s module-level-only rule.
    if (isModuleLevelDeclaration(decl, ctx)) {
        ConstantValue val = evaluate(decl->init, ctx);

        if (val.isError()) {
            // The evaluator emitted its own diagnostic (division by
            // zero, integer overflow, ...). Nothing more to add; the
            // declaration is rejected.
            return;
        }

        if (!val.isEvaluated()) {
            // The initializer is not a compile-time constant. Under
            // the new rule, that is a diagnostic, not a silent
            // "unfolded" state.
            ctx.diagnostics.error(DiagCode::Type_TopLevelInitNotConstant,
                                  decl->init,
                                  "the initializer of top-level '",
                                  (decl->isConst ? "const" : "let"), " ",
                                  ctx.pool.lookup(decl->name),
                                  "' must be a compile-time constant");
            ctx.diagnostics.note(decl->init,
                                 "nothing runs at module load time; "
                                 "a top-level binding starts at a value known "
                                 "at compile time. To compute a value at "
                                 "runtime, declare the binding without an "
                                 "initializer's runtime call — initialize it "
                                 "inside an @export'ed function the host calls");
            return;
        }

        // The fold succeeded. Cache the value on the expression; every
        // later pass (the bytecode emitter's static-data writer, a
        // future `.lucb` serializer) reads it from there.
        decl->init->isConst    = true;
        decl->init->constValue = val;
    }

    // ─── 6. Mangled name for @export ────────────────────────────────────
    //
    // Only top-level `let`/`const` can be `@export`ed, and the attribute
    // validator has already rejected `@export` on a local.
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

    // A host-bound function has no body.
    if (decl->isHostBound) return;

    // A function without a body block is a parser error, not a user
    // error. The parser produces a body block or a `host(...)` target.
    if (!decl->body) {
        AST_ASSERT_MSG(false,
            "resolveFnBody: a non-host-bound function has no body — "
            "the parser should have set hasSyntaxError");
        return;
    }

    // ─── 1. Push the function scope ─────────────────────────────────────
    //
    // `ScopedFunction` picks `FuncBody` vs. `SequenceBody` from
    // `decl->isSequence` and pushes a `SymbolScope` for the function's
    // parameters.
    ScopedFunction fnScope(ctx, decl);

    // ─── 2. Resolve the function's parameters ───────────────────────────
    //
    // `resolveParam` inserts each parameter's name into the function's
    // scope and resolves the parameter's type. This is the second
    // registration of these parameters — the first was in pass 2, in
    // the module scope, and it was popped before this function ran.
    for (ParamAST* param : decl->params) {
        resolveParam(param, ctx);
    }

    // ─── 3. Resolve the body block ──────────────────────────────────────
    //
    // `resolveStmt` on a `BlockStmtAST` pushes its own `SymbolScope` and
    // walks the statements, returning `true` if the last reachable
    // statement transfers control out of the block.
    bool bodyTransfers = resolveStmt(decl->body, ctx);

    // ─── 4. Return-path check ───────────────────────────────────────────
    //
    // A function whose return type is not `unit` must return a value on
    // every path that reaches the end of the body.
    bool returnsValue = decl->returnType
                     && !isVoidType(decl->returnType);
    if (returnsValue && !bodyTransfers) {
        ctx.diagnostics.error(DiagCode::Type_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 5. Sequence body check ─────────────────────────────────────────
    //
    // A `@sequence` body should actually suspend; a sequence that never
    // calls a `wait*` is almost certainly a mistake.
    checkSequenceBody(decl, ctx);
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
    // module scope — but the insertion there is transient, popped when
    // pass 2 finishes this function's signature. When called from
    // `resolveFnBody` (pass 3), the current scope is the function's own
    // scope, and the insertion persists for the body's resolution.
    //
    // The context's `insertLocal` asserts `!isAtModuleLevel()`. That is
    // the contract of a parameter: a parameter is always inserted into
    // a scope, never into the module table. During pass 2 the caller
    // (`resolveFnDecl`) must have pushed a scope first — the caller
    // uses a `SymbolScope` guard for the signature, so the assertion
    // holds.
    //
    // A parameter with an empty name is possible in a lambda whose
    // parameter list uses `_` as a discard marker; the parser leaves
    // the name empty in that case. An empty name is not registered.
    if (!param->name.isEmpty()) {
        ctx.insertLocal(param);
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
    // Sema synthesizes the declared type `[T]` — a dynamic array.
    if (param->isVariadic) {
        param->type = ctx.getArrayType(ArrayKind::Dynamic,
                                       /*size=*/0,
                                       param->type);
    }

    // ─── 4. Const-parameter validation ──────────────────────────────────
    //
    // A `const` parameter may not be reassigned and may not have
    // mutation applied through it. The check is on the parameter's
    // *declared type*; the call-site check (a `const` argument cannot
    // be passed to a non-`const` parameter) is a separate rule and is
    // enforced in `resolveCallExpr`.
    if (param->isConst) {
        (void)validateConstType(param->type, param->name, "parameter", ctx);
    }
}

} // namespace lucid::sema