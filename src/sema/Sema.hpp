/**
 * @file Sema.hpp
 *
 * @responsibility The complete declaration surface of semantic
 *                 analysis: the driver entry points, the per-node
 *                 resolvers, and every cross-file helper.
 *
 * ─── Design: one header, every cross-file function ────────────────────────
 * A function that is defined in one `.cpp` and called from another has
 * to be declared somewhere both files can see. Rather than split the
 * declarations across `SemaInternal.hpp` and a per-file private header,
 * every cross-file function is declared here. A function that is used
 * by exactly one `.cpp` is `static` in that file and does not appear
 * here.
 *
 * The result is a large header. It is worth it: the alternative is a
 * web of forward declarations at the top of each `.cpp` that have to
 * stay in sync with the definitions, and no single place to look up
 * "what does Sema expose?". One flat list is easier to keep correct.
 *
 * ─── Design: three passes, three driver functions ─────────────────────────
 * Sema runs over a module in three passes:
 *
 *   1. registerModuleDeclarations — insert every top-level name into
 *      the module's symbol table. No types resolved, no bodies
 *      analyzed. This is what makes forward references and cyclic
 *      imports (§3.3) work.
 *
 *   2. resolveModuleDeclarations — resolve each declaration's types,
 *      attributes, and imports. `FN` and `TABLE` declarations get
 *      their signatures and their column types resolved here. Top-level
 *      variable initializers are resolved here too, because a
 *      `let`/`const` has no separate body phase.
 *
 *   3. resolveModuleBodies — resolve every function body. By this
 *      point every name a body can reference has been registered
 *      (pass 1) and every type a body can mention has been resolved
 *      (pass 2).
 *
 * The three passes are separate entry points because the caller runs
 * them across *all* modules before moving to the next pass. The
 * interleaving is what makes cross-module forward references work.
 *
 * ─── Design: RAII guards live in SemaContext.hpp ──────────────────────────
 * `ScopedFunction`, `ScopedContext`, `ScopedNarrowing`,
 * `ScopedIfCondition`, and `SymbolScope` are declared in
 * `SemaContext.hpp`. They are the tools a resolver uses to push and pop
 * context. This header declares the resolvers themselves; those use the
 * guards, they do not define them.
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <vector>

namespace lucid::sema {

struct SemaContext;   // forward declaration; defined in SemaContext.hpp

// ═════════════════════════════════════════════════════════════════════════════
// Module-level driver
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Run all three passes over a module set.
///
/// Runs pass 1 across every module, then pass 2 across every module,
/// then pass 3. The interleaving is what makes cross-module forward
/// references work: pass 2 of module A can reference a name in module B
/// because pass 1 of module B already ran.
///
/// Stops early if `ctx.diagnostics.canContinue()` returns false.
void analyze(const std::vector<ModuleAST*>& modules, SemaContext& ctx);

/// @brief Run all three passes over one module.
///
/// A convenience for a caller that owns exactly one module. For a
/// multi-module session, use `analyze`.
void resolveModule(ModuleAST* module, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Pass 1 — registration
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Pass 1: register every top-level name of `module`.
///
/// For each top-level declaration:
///   - `TABLE X`        → insert `X` into the module's type namespace
///                        AND its value namespace (a table is both a
///                        type and a value — the sheet itself).
///   - `FN f`           → insert `f` into the module's value namespace.
///   - `let`/`const v`  → insert `v` into the module's value namespace.
///   - `import a.b as c`→ register alias `c` for the resolved module.
///
/// Emits diagnostics for duplicate names.
void registerModuleDeclarations(ModuleAST* module, SemaContext& ctx);

/// @brief Register the alias of an `import` directive.
///
/// Emits no diagnostic if the target module does not resolve; that
/// failure is reported in pass 2 by `resolveImportDecl`.
void registerImportName(ImportDeclAST* decl, SemaContext& ctx);

/// @brief Register a `TABLE` name in both namespaces.
void registerTableName(TableDeclAST* decl, SemaContext& ctx);

/// @brief Register a `FN` name in the value namespace.
void registerFnName(FnDeclAST* decl, SemaContext& ctx);

/// @brief Register a top-level `let`/`const` name in the value namespace.
void registerVarName(VarDeclAST* decl, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Pass 2 — declaration resolution
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Pass 2: resolve each declaration's types and attributes.
///
/// Runs after every module has completed pass 1.
void resolveModuleDeclarations(ModuleAST* module, SemaContext& ctx);

/// @brief Resolve an `import` directive. Emits `Name_UndefinedModule` on
///        failure.
void resolveImportDecl(ImportDeclAST* decl, SemaContext& ctx);

/// @brief Resolve a `TABLE` declaration: attributes, column types,
///        column indices, table constraint checks, mangled name.
void resolveTableDecl(TableDeclAST* decl, SemaContext& ctx);

/// @brief Resolve one column's declared type.
void resolveColumnDecl(ColumnDeclAST* decl, SemaContext& ctx);

/// @brief Resolve a `FN` declaration's signature: attributes, parameter
///        types, return type, mangled name. Does NOT resolve the body.
void resolveFnDecl(FnDeclAST* decl, SemaContext& ctx);

/// @brief Resolve a `FN` declaration's body (pass 3).
void resolveFnBody(FnDeclAST* decl, SemaContext& ctx);

/// @brief Resolve a `ParamAST`'s type and register its name in the
///        current scope.
///
/// Called from `resolveFnDecl` (pass 2, signature) and from
/// `resolveFnBody` (pass 3, body). The two call sites register the
/// parameter in two different scopes; the second is the one the body
/// sees.
void resolveParam(ParamAST* param, SemaContext& ctx);

/// @brief Resolve a `VarDeclAST`'s type, resource kind, and initializer.
///
/// Called from `resolveModuleDeclarations` for a top-level `let` (pass 2),
/// and from `resolveVarDeclStmt` in `SemaStmt.cpp` for a local `let`
/// (pass 3). The caller is responsible for having registered the name;
/// this function does not.
void resolveVarDecl(VarDeclAST* decl, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Pass 3 — body resolution (driver) and per-declaration dispatcher
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Pass 3: resolve every function body in `module`.
void resolveModuleBodies(ModuleAST* module, SemaContext& ctx);

/// @brief Resolve one declaration, including its body if it has one.
///
/// A convenience for tooling (the LSP) and tests. The three module
/// passes do not go through this function.
void resolveDecl(DeclAST* decl, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Expressions
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Resolve one expression with no target type.
TypeAST* resolveExpr(ExprAST* expr, SemaContext& ctx);

/// @brief Resolve one expression against an expected type.
///
/// A `nullptr` target is equivalent to `resolveExpr`. An unknown-type
/// target is treated as no target. If the expression's type is not
/// assignable to the target, a `Type_Mismatch` is emitted and the
/// unknown type is returned.
TypeAST* resolveExprWithTarget(ExprAST* expr, TypeAST* targetType,
                               SemaContext& ctx);

// ─── Per-form resolvers ─────────────────────────────────────────────────────
//
// Called from `resolveExprWithTarget`'s dispatch. Each is also callable
// directly by a caller that has already classified the expression —
// `resolveCallExpr` for instance calls `resolveExpr` on its callee, and
// `resolveIndexExpr` calls `resolveExpr` on both target and index.
//
// Every per-form resolver returns the expression's *natural* type. The
// target is consulted only by the forms that need it (literals, array
// literals, lambdas); the wrapper `resolveExprWithTarget` applies the
// final assignability check.

TypeAST* resolveLiteralExpr      (LiteralExprAST*      expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveIdentifierExpr   (IdentifierExprAST*   expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveArrayLiteralExpr (ArrayLiteralExprAST* expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveFieldAccessExpr  (FieldAccessExprAST*  expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveIndexExpr        (IndexExprAST*        expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveCallExpr         (CallExprAST*         expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveLambdaExpr       (LambdaExprAST*       expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveStartExpr        (StartExprAST*        expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveUnaryExpr        (UnaryExprAST*        expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveBinaryExpr       (BinaryExprAST*       expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveParenExpr        (ParenExprAST*        expr, TypeAST* target, SemaContext& ctx);
TypeAST* resolveRangeExpr        (RangeExprAST*        expr, TypeAST* target, SemaContext& ctx);

// ─── Field-access classification ────────────────────────────────────────────
//
// `resolveFieldAccessExpr` classifies an `a.b` access by what `a` is.
// The sub-resolvers handle one case each. They are declared here
// because a future caller (a diagnostic renderer that wants to know
// "is this a column view?") may need them.

TypeAST* resolveModuleMemberAccess(FieldAccessExprAST* expr,
                                   IdentifierExprAST*  objId,
                                   ModuleAST*          module,
                                   TypeAST*            target,
                                   SemaContext&        ctx);

TypeAST* resolveCellAccess(FieldAccessExprAST* expr,
                           RowRefTypeAST*      rowRef,
                           TypeAST*            target,
                           SemaContext&        ctx);

TypeAST* resolveTableMemberAccess(FieldAccessExprAST* expr,
                                  TypeAST*            objectType,
                                  TypeAST*            target,
                                  SemaContext&        ctx);

TypeAST* tryResolveTableMethod(FieldAccessExprAST* expr,
                               TableDeclAST*       table,
                               SemaContext&        ctx);

TypeAST* tryResolveByColumnLookup(FieldAccessExprAST* expr,
                                  TableDeclAST*       table,
                                  SemaContext&        ctx);

TypeAST* resolveArrayMethodAccess(FieldAccessExprAST* expr,
                                  ArrayTypeAST*       arrayType,
                                  TypeAST*            target,
                                  SemaContext&        ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Statements
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Resolve one statement.
///
/// @return true if control transfers out of the enclosing block.
bool resolveStmt(StmtAST* stmt, SemaContext& ctx);

// ─── Per-kind statement resolvers ───────────────────────────────────────────
//
// Called from `resolveStmt`'s dispatch. All return the same bool
// protocol as `resolveStmt`.

bool resolveBlock       (BlockStmtAST*      stmt, SemaContext& ctx);
bool resolveIfStmt      (IfStmtAST*         stmt, SemaContext& ctx);
bool resolveSwitchStmt  (SwitchStmtAST*     stmt, SemaContext& ctx);
bool resolveWhileStmt   (WhileStmtAST*      stmt, SemaContext& ctx);
bool resolveForStmt     (ForStmtAST*        stmt, SemaContext& ctx);
bool resolveReturnStmt  (ReturnStmtAST*     stmt, SemaContext& ctx);
bool resolveBreakStmt   (BreakStmtAST*      stmt, SemaContext& ctx);
bool resolveContinueStmt(ContinueStmtAST*   stmt, SemaContext& ctx);
bool resolveExprStmt    (ExprStmtAST*       stmt, SemaContext& ctx);
bool resolveVarDeclStmt (VarDeclStmtAST*    stmt, SemaContext& ctx);
bool resolveAssignStmt  (AssignStmtAST*     stmt, SemaContext& ctx);

// ─── Sequence suspend points ────────────────────────────────────────────────

bool resolveWaitStmt          (WaitStmtAST*           stmt, SemaContext& ctx);
bool resolveWaitFramesStmt    (WaitFramesStmtAST*     stmt, SemaContext& ctx);
bool resolveWaitUntilStmt     (WaitUntilStmtAST*      stmt, SemaContext& ctx);
bool resolveWaitForEventStmt  (WaitForEventStmtAST*   stmt, SemaContext& ctx);
bool resolveWaitForRequestStmt(WaitForRequestStmtAST* stmt, SemaContext& ctx);

// ─── For-loop binding helpers ───────────────────────────────────────────────

bool resolveRangeForBindings (ForStmtAST* stmt, TypeAST* boundType,    SemaContext& ctx);
bool resolveTableForBindings (ForStmtAST* stmt, TypeAST* iterableType, SemaContext& ctx);
bool resolveArrayForBindings (ForStmtAST* stmt, TypeAST* iterableType, SemaContext& ctx);

// ─── Switch coverage helper ─────────────────────────────────────────────────

void checkFixedTableSwitchCoverage(SwitchStmtAST* stmt,
                                   TypeAST*       subjectType,
                                   SemaContext&   ctx);

} // namespace lucid::sema