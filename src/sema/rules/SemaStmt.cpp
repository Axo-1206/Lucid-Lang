/// @file SemaStmt.cpp
/// @brief Statement resolution and control-flow analysis.
///
/// ─── The return protocol ──────────────────────────────────────────────────
/// `resolveStmt` and its per-kind helpers return a `bool` that answers
/// one question: "does this statement guarantee that control transfers
/// out of the enclosing block?" A `return`, `break`, `continue`, or a
/// block whose last reachable statement transfers all return `true`.
/// Everything else returns `false`.
///
/// The protocol is used two ways:
///   - Inside a block, to detect unreachable code — a statement after a
///     statement that returns `true` is unreachable, and Sema warns.
///   - At the end of a function body, to check that a value-returning
///     function returns on every path. `resolveFnBody` calls
///     `resolveStmt` on the body block; the return value answers "does
///     the function's last reachable statement transfer?".
///
/// ─── Context management ───────────────────────────────────────────────────
/// Every per-kind resolver that opens a syntactic context pushes a frame
/// through an RAII guard and lets the destructor pop it. The frame kinds
/// are:
///
///   Block       — pushed by `resolveBlock`, holds pending inverse
///                 narrowing for a standalone `if x == nil { return }`.
///   IfStmt      — pushed by `resolveIfStmt`, holds the pending
///                 narrowing set during condition analysis.
///   LoopBody    — pushed by `resolveWhileStmt` and `resolveForStmt`.
///   SwitchBody  — pushed by `resolveSwitchStmt`.
///
/// `ScopedFunction` (in `SemaContext.hpp`) pushes `FuncBody` or
/// `SequenceBody`, but that happens in `resolveFnBody`, not here.
///
/// ─── The sequence suspend points ──────────────────────────────────────────
/// The five `wait*` statements are only legal inside a `@sequence`
/// function's body. The parser produces them anywhere and Sema rejects
/// them outside a sequence. This file resolves them; the sequence
/// rules that need the surrounding signature or the enclosing call
/// sites live in `SequenceChecker` and are invoked from elsewhere.

#include "sema/Sema.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/const_eval/ConstEvaluator.hpp"
#include "sema/support/TypeNarrowHelpers.hpp"
#include "sema/types/SemaType.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include <unordered_set>

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// resolveStmt — dispatcher
// ═════════════════════════════════════════════════════════════════════════════

bool resolveStmt(StmtAST* stmt, SemaContext& ctx) {
    if (!stmt || stmt->hasSyntaxError) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:      return resolveBlock      (stmt->as<BlockStmtAST>(),      ctx);
        case ASTKind::IfStmt:         return resolveIfStmt     (stmt->as<IfStmtAST>(),         ctx);
        case ASTKind::SwitchStmt:     return resolveSwitchStmt (stmt->as<SwitchStmtAST>(),     ctx);
        case ASTKind::WhileStmt:      return resolveWhileStmt  (stmt->as<WhileStmtAST>(),      ctx);
        case ASTKind::ForStmt:        return resolveForStmt    (stmt->as<ForStmtAST>(),        ctx);
        case ASTKind::ReturnStmt:     return resolveReturnStmt (stmt->as<ReturnStmtAST>(),     ctx);
        case ASTKind::BreakStmt:      return resolveBreakStmt  (stmt->as<BreakStmtAST>(),      ctx);
        case ASTKind::ContinueStmt:   return resolveContinueStmt(stmt->as<ContinueStmtAST>(),  ctx);
        case ASTKind::ExprStmt:       return resolveExprStmt   (stmt->as<ExprStmtAST>(),       ctx);
        case ASTKind::VarDeclStmt:    return resolveVarDeclStmt(stmt->as<VarDeclStmtAST>(),    ctx);
        case ASTKind::AssignStmt:     return resolveAssignStmt (stmt->as<AssignStmtAST>(),     ctx);

        // Sequence suspend points
        case ASTKind::WaitStmt:           return resolveWaitStmt          (stmt->as<WaitStmtAST>(),           ctx);
        case ASTKind::WaitFramesStmt:     return resolveWaitFramesStmt    (stmt->as<WaitFramesStmtAST>(),     ctx);
        case ASTKind::WaitUntilStmt:      return resolveWaitUntilStmt     (stmt->as<WaitUntilStmtAST>(),      ctx);
        case ASTKind::WaitForEventStmt:   return resolveWaitForEventStmt  (stmt->as<WaitForEventStmtAST>(),   ctx);
        case ASTKind::WaitForRequestStmt: return resolveWaitForRequestStmt(stmt->as<WaitForRequestStmtAST>(), ctx);

        default:
            // A statement kind that the dispatcher does not handle means
            // a new statement form was added without extending this
            // function — a compiler bug, not a user error.
            AST_ASSERT_MSG(false,
                "resolveStmt: unrecognized StmtAST kind");
            return false;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveBlock
// ═════════════════════════════════════════════════════════════════════════════

bool resolveBlock(BlockStmtAST* block, SemaContext& ctx) {
    if (!block) return false;

    // ─── Push the block context ─────────────────────────────────────────
    //
    // The `Block` context frame carries the block's pending inverse
    // narrowing, set by a preceding standalone `if x == nil { return }`.
    // When the block is entered, this frame is the innermost `Block`,
    // and `hasPendingInverseNarrowing` reports whether there is one to
    // apply.
    ScopedContext blockCtx(ctx, ContextKind::Block, block);

    // ─── Apply pending inverse narrowing ────────────────────────────────
    //
    // A standalone `if x == nil { return }` inside this block, in an
    // earlier statement, has stored its inverse (x is non-nil after the
    // if) on this frame. Apply it before resolving the block's body so
    // subsequent statements see `x` as narrowed.
    //
    // The narrowing level is pushed here and popped before the function
    // returns. It is a separate scope from the block's own `SymbolScope`
    // — the narrowing is flow state, not name binding.
    bool appliedPendingNarrowing = false;
    if (ctx.stack.hasPendingInverseNarrowing()) {
        const NarrowingInfo& pending = ctx.stack.getPendingInverseNarrowing();
        if (pending.hasNarrowing) {
            ctx.stack.pushNarrowingLevel(/*isInverse=*/true);
            for (const auto& [name, type] : pending.narrowings) {
                ctx.stack.narrowVariable(name, type);
            }
            ctx.stack.clearPendingInverseNarrowing();
            appliedPendingNarrowing = true;
        }
    }

    // ─── Push the block's lexical scope ─────────────────────────────────
    //
    // Names declared inside the block — including local `let`s — are
    // visible only until the block's closing brace. The scope is popped
    // by the guard's destructor on any return path.
    SymbolScope scope(ctx);

    // ─── Resolve each statement ─────────────────────────────────────────
    //
    // The first statement that transfers control makes every following
    // statement unreachable. Report one warning per unreachable
    // statement (with a break after the first to avoid a cascade in a
    // long dead tail) and stop resolving the block.
    bool transfers = false;
    for (StmtAST* stmt : block->stmts) {
        if (!stmt) continue;

        if (transfers) {
            ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt,
                                    "unreachable code");
            break;
        }

        transfers = resolveStmt(stmt, ctx);
    }

    // ─── Pop the narrowing level ────────────────────────────────────────
    if (appliedPendingNarrowing) {
        ctx.stack.popNarrowingLevel();
    }

    return transfers;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveIfStmt
// ═════════════════════════════════════════════════════════════════════════════

bool resolveIfStmt(IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Push the if-statement context ──────────────────────────────────
    //
    // The `IfStmt` frame carries the pending narrowing set during
    // condition analysis. `ScopedIfCondition` (below) is the guard that
    // puts the frame in "condition-analysis mode" and reads the pending
    // narrowing back out.
    ScopedContext ifCtx(ctx, ContextKind::IfStmt, stmt);

    const bool hasElse = (stmt->elseBranch != nullptr);

    // ─── Resolve the condition ──────────────────────────────────────────
    //
    // Conditions are `bool` in the new grammar (§6.14). No truthiness.
    // `resolveExprWithTarget` against the singleton `bool` type
    // enforces the rule and produces a well-typed condition node.
    //
    // The narrowing detection that follows reads the condition tree —
    // it wants to see the shape `x != nil` / `x == nil`, so it needs
    // the tree's structure, not just its type. `ScopedIfCondition` sets
    // a flag on the context stack that tells the binary-expression
    // resolver "you are in an if-condition; detect narrowings". See
    // `resolveBinaryExpr` for where the flag is read.
    {
        ScopedIfCondition ifCond(ctx, hasElse);
        TypeAST* condType = resolveExprWithTarget(stmt->condition,
                                                  ctx.getBoolType(), ctx);
        if (!condType || condType->isa<UnknownTypeAST>()) {
            return false;
        }
        // The narrowing analysis runs inside `resolveBinaryExpr` and
        // stores its result on the frame via `setPendingNarrowing`.
        // After `ScopedIfCondition`'s guard scope ends, the flag is
        // cleared but the pending-narrowing data survives on the frame.
    }

    const NarrowingInfo info = ctx.stack.getPendingNarrowing();
    const bool hasNarrowing = info.hasNarrowing;

    // ─── Resolve the then-branch ────────────────────────────────────────
    //
    // If the condition narrowed a variable (e.g. `x != nil`), the
    // then-branch sees the narrowed type. The `ScopedNarrowing` guard
    // pushes a narrowing level with the narrowings applied; the level
    // pops when the guard's destructor fires, which is after the
    // then-branch has been resolved.
    //
    // The `isInverse` flag is `false` for a `!=` condition: the direct
    // narrowing (x is non-nil) applies in the then-branch.
    bool thenTransfers = false;
    {
        if (hasNarrowing && !info.isEquality) {
            ScopedNarrowing narrowing(ctx, info.narrowings, /*isInverse=*/false);
            thenTransfers = resolveStmt(stmt->thenBranch, ctx);
        } else {
            thenTransfers = resolveStmt(stmt->thenBranch, ctx);
        }
    }

    // ─── Resolve the else-branch ────────────────────────────────────────
    //
    // The `isInverse` flag is `true` for an `==` condition: the inverse
    // narrowing (x is non-nil in the else-branch of `x == nil`) applies
    // there.
    //
    // `else if` chains: an else branch that is itself an `IfStmtAST` is
    // resolved by recursing into `resolveIfStmt`, which pushes its own
    // `IfStmt` frame.
    bool elseTransfers = false;
    if (stmt->elseBranch) {
        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseTransfers = resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else if (hasNarrowing && info.isEquality) {
            ScopedNarrowing narrowing(ctx, info.narrowings, /*isInverse=*/true);
            elseTransfers = resolveStmt(stmt->elseBranch, ctx);
        } else {
            elseTransfers = resolveStmt(stmt->elseBranch, ctx);
        }
    }

    // ─── If both branches transfer, the if transfers ────────────────────
    if (thenTransfers && elseTransfers) {
        return true;
    }

    // ─── Pending inverse narrowing for a standalone if ──────────────────
    //
    // `if x == nil { return }` with no else: the then-branch transfers
    // (it returns), so the code after the if sees `x` as non-nil. Store
    // the inverse narrowing on the enclosing block's frame so the block
    // can apply it to the rest of its statements.
    //
    // The condition must be `x == nil` for this to make sense — an
    // `if x != nil { return }` transfers when x is *not* nil, and the
    // code after sees x as `nil`, which is a different (and much less
    // useful) fact. The `isEquality` flag distinguishes the two.
    if (!hasElse && thenTransfers && hasNarrowing && info.isEquality) {
        ctx.stack.setPendingInverseNarrowing(info);
    }

    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveSwitchStmt
// ═════════════════════════════════════════════════════════════════════════════
//
// The new grammar's switch is over a fixed-table row reference (`&T`
// where T is a `@fixed` or `@readonly` table), or over a primitive
// value. The fixed-table case gets a missing-member warning — not an
// error — because the switch's `default` clause is always required, so
// a missing case is never a correctness bug; it is a hint that the
// switch may not have been updated when the table gained a row.
//
// This file holds the *body* of the fixed-table check inline. The check
// is small enough that a separate `SwitchHelpers` file would be more
// ceremony than the logic warrants. The old `SwitchHelpers` existed for
// enum exhaustiveness, which the new grammar does not have.

bool resolveSwitchStmt(SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Resolve the subject ────────────────────────────────────────────
    TypeAST* subjectType = resolveExpr(stmt->subject, ctx);
    if (!subjectType || subjectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Type_InvalidSwitchType, stmt->subject,
                              "switch subject has unknown type");
        return false;
    }

    // ─── Validate the subject's type ────────────────────────────────────
    //
    // The subject must be a primitive (`int`, `bool`, `char`, `string`)
    // or a `&T` for a `@fixed`/`@readonly` table `T`. Anything else —
    // a bare table, a dynamic array, a function type, a `T?` — is a
    // mismatch.
    //
    // The subject type is consulted again below to run the fixed-table
    // check; the validation here is just the "is this legal at all?"
    // gate.
    //
    // (The subject being a `&T` for a *growing* table is legal — the
    // cases just cannot be compile-time constants, and each case value
    // will fail its own constant-expression check when resolved. There
    // is no reason to reject the switch shape itself.)
    const bool subjectIsRowRef = isRowRefType(subjectType);
    const bool subjectIsPrimitive =
        isIntegerType(subjectType) || isBoolType(subjectType) ||
        isCharType(subjectType)    || isStringType(subjectType);

    if (!subjectIsRowRef && !subjectIsPrimitive) {
        ctx.diagnostics.error(DiagCode::Type_InvalidSwitchType, stmt->subject,
                              "switch subject must be a primitive or a "
                              "row reference, got ",
                              typeToString(subjectType, ctx.pool));
        return false;
    }

    // ─── Push the switch context ────────────────────────────────────────
    ScopedContext switchCtx(ctx, ContextKind::SwitchBody, stmt);

    // ─── Resolve each case ──────────────────────────────────────────────
    //
    // A case's body is a block; the case "transfers" if its block
    // transfers. A switch transfers if every case (including `default`)
    // transfers.
    //
    // Each case value is a constant expression (grammar §12.2):
    // a literal, a fixed-table member reference (`Direction.North`), a
    // small arithmetic combination of literals, or a range. The
    // `resolveExprWithTarget` call below resolves each against the
    // subject's type, which enforces "same-type as the subject" and
    // also drives the fixed-row-sugar resolution for `Direction.North`
    // (the field access classifier recognizes a fixed-table member
    // access and resolves it to a compile-time row reference).
    bool allCasesTransfer = true;
    for (SwitchCaseAST* caseClause : stmt->cases) {
        if (!caseClause) continue;

        for (ExprAST* value : caseClause->values) {
            TypeAST* valueType = resolveExprWithTarget(value, subjectType, ctx);
            if (!valueType || valueType->isa<UnknownTypeAST>()) {
                // The resolver already emitted a diagnostic.
                continue;
            }

            // ─── Constant-ness check ────────────────────────────────────
            //
            // Every case value must be a compile-time constant. The
            // constant evaluator tries to fold the expression; if the
            // fold succeeds, `value->isConst` and `value->constValue`
            // are populated, and the value is legal.
            //
            // The error here is `Type_Mismatch` rather than a dedicated
            // code; the message names the rule.
            ConstantValue folded = evaluate(value, ctx);
            if (!folded.isEvaluated() || folded.isError()) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, value,
                                      "case value must be a compile-time "
                                      "constant expression");
                continue;
            }
        }

        if (caseClause->body) {
            if (!resolveBlock(caseClause->body, ctx)) {
                allCasesTransfer = false;
            }
        }
    }

    // ─── Resolve the default clause ─────────────────────────────────────
    //
    // The default clause is always present (grammar §12.2 requires it),
    // but the parser produces a placeholder block on a missing default
    // and reports a syntax error. Resolving a placeholder block is
    // harmless — it is empty — so the code does not need to guard
    // against its presence.
    if (stmt->defaultBody) {
        if (!resolveBlock(stmt->defaultBody, ctx)) {
            allCasesTransfer = false;
        }
    }

    // ─── Missing-member warning for a fixed-table subject ───────────────
    //
    // When the subject's type is `&T` and `T` is a `@fixed` or
    // `@readonly` table, Sema checks the case values against the
    // table's rows. If any row is not named by a case, warn.
    //
    // The check is decided by the case values that resolved to fixed
    // rows — `FieldAccessExprAST` whose object is a table and whose
    // field resolved to a fixed-row sugar. Each such case contributes
    // its row to the covered set. After the loop, compare the covered
    // set to the table's rows.
    //
    // The check is skipped silently if the subject is not a row
    // reference, or if it is a row reference to a growing table (which
    // has no fixed row set to check against).
    if (subjectIsRowRef) {
        checkFixedTableSwitchCoverage(stmt, subjectType, ctx);
    }

    // A switch with a default clause always has a fallback, so it
    // transfers only if every case body transfers *and* the default
    // body transfers. The `allCasesTransfer` flag folds the case check
    // and the default check into one.
    return allCasesTransfer;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFixedTableSwitchCoverage
// ─────────────────────────────────────────────────────────────────────────────
//
// Helper for `resolveSwitchStmt`. Runs only when the subject is a row
// reference. Looks through the switch's case values for fixed-row-sugar
// accesses (the `Direction.North` form), collects the set of rows they
// cover, and warns if any row of the subject's table is missing.
//
// The helper is a no-op when:
//   - the subject is `&T` for a growing table (no fixed row set);
//   - the subject is not a row reference at all (checked by the caller).
//
// It never emits an error, only a warning. The default clause is
// mandatory, so a missing case is never a correctness bug.

void checkFixedTableSwitchCoverage(SwitchStmtAST* stmt,
                                          TypeAST* subjectType,
                                          SemaContext& ctx) {
    if (!stmt || !subjectType) return;
    if (!isRowRefType(subjectType)) return;

    RowRefTypeAST* rowRef = subjectType->as<RowRefTypeAST>();
    if (!rowRef->inner || !rowRef->inner->isa<NamedTypeAST>()) return;

    NamedTypeAST* named = rowRef->inner->as<NamedTypeAST>();
    if (!named->resolvedDecl) return;
    if (!named->resolvedDecl->isa<TableDeclAST>()) return;

    TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();

    // Only fixed-row-set tables have a member list to check against.
    if (!table->hasFixedRowSet()) return;

    // A host-backed table has no rows in the source-language sense;
    // its "rows" are opaque handles the script cannot name. Skip the
    // coverage check for one.
    if (table->isHostBacked) return;

    // ─── Collect covered rows ───────────────────────────────────────────
    //
    // Each case value that resolved to a fixed-row sugar contributes
    // its row's name (the field name of the `FieldAccessExprAST`) to
    // the covered set. A case value that is not a fixed-row sugar
    // (a literal in a primitive switch, or a range) contributes
    // nothing — the missing-member check is about named rows, and a
    // non-row case value cannot name a row.
    //
    // A range in a fixed-table switch is not meaningful (ranges match
    // against integer values, not row identities), so it is ignored
    // here. If a range ever becomes legal in a fixed-table switch, this
    // helper would need to expand the range into individual rows before
    // comparing.
    std::unordered_set<InternedString> covered;
    for (SwitchCaseAST* caseClause : stmt->cases) {
        if (!caseClause) continue;
        for (ExprAST* value : caseClause->values) {
            if (!value) continue;
            if (!value->isa<FieldAccessExprAST>()) continue;

            FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
            if (!field->isFixedRowSugar) continue;

            covered.insert(field->fieldName);
        }
    }

    // ─── Emit the warning if any row is missing ─────────────────────────
    //
    // Iterate the table's rows in declaration order, so the diagnostic
    // names them in the order the source declared them. The warning is
    // emitted once, on the switch statement, listing every missing row.
    std::string missing;
    bool anyMissing = false;
    for (size_t i = 0; i < table->rows.size(); ++i) {
        RowAST* row = table->rows[i];
        if (!row) continue;

        // The "name" of a row for the purposes of this check is the
        // fixed-row sugar name — the identifier the user would write as
        // `T.Member`. That name is the row's *first string column's
        // value* (§7.1's fixed-table sugar rule).
        //
        // The parser stores the row's cells; the first cell's value is
        // the row's name. If the first cell is a string literal and
        // its value is interned, use it. If it is not (a row whose
        // first column is not a string, or whose first cell is a
        // non-literal), the row is not addressable by sugar and is
        // skipped — the user cannot name it in a case, so it cannot be
        // covered or missing.
        if (row->cells.empty()) continue;
        ExprAST* firstCell = row->cells[0];
        if (!firstCell || !firstCell->isa<LiteralExprAST>()) continue;

        LiteralExprAST* lit = firstCell->as<LiteralExprAST>();
        if (lit->kind != LiteralKind::String &&
            lit->kind != LiteralKind::RawString) {
            continue;
        }

        InternedString rowName = lit->value;
        if (covered.count(rowName)) continue;

        if (anyMissing) missing += ", ";
        missing += ctx.pool.lookup(rowName);
        anyMissing = true;
    }

    if (anyMissing) {
        ctx.diagnostics.warning(DiagCode::Warn_SwitchMissingMember, stmt,
                                "switch over '", ctx.pool.lookup(table->name),
                                "' does not cover all rows (missing: ",
                                missing, ")");
        ctx.diagnostics.note(stmt,
                             "a 'default' clause is present, so the switch "
                             "still compiles; the warning is a hint that "
                             "the switch may need updating if the table "
                             "gains rows");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveWhileStmt
// ═════════════════════════════════════════════════════════════════════════════

bool resolveWhileStmt(WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Push the loop context ──────────────────────────────────────────
    //
    // `LoopBody` is what `resolveBreakStmt` and `resolveContinueStmt`
    // check for — a `break` outside a loop and outside a switch is an
    // error.
    ScopedContext loopCtx(ctx, ContextKind::LoopBody, stmt);

    // ─── Resolve the condition ──────────────────────────────────────────
    //
    // A `while` condition is `bool`, same as an `if` condition. The
    // condition is resolved against the singleton `bool` type.
    TypeAST* condType = resolveExprWithTarget(stmt->condition,
                                              ctx.getBoolType(), ctx);
    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Compile-time condition folding ─────────────────────────────────
    //
    // A `while` whose condition folds to `false` at compile time has a
    // body that never executes. Warn and skip the body (there is no
    // point resolving unreachable code, and the `break`/`continue`
    // diagnostics that would fire in the body would be spurious).
    //
    // A `while` whose condition folds to `true` is an infinite loop
    // *unless* the body contains a `break`. Whether the body breaks is
    // not something this pass can cheaply decide (it would require a
    // full control-flow analysis that tracks `break` through nested
    // blocks and switches). The compiler emits no diagnostic for an
    // infinite `while`; the user is assumed to know what they wrote.
    ConstantValue folded = evaluate(stmt->condition, ctx);
    if (folded.isEvaluated() && folded.isBool() && !folded.asBool()) {
        ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt->body,
                                "while loop condition is always false — "
                                "body will never execute");
        return false;
    }

    // ─── Resolve the body ───────────────────────────────────────────────
    //
    // A `while` never transfers control out of the enclosing block:
    // even if its body ends in `return`, the loop might have iterated
    // zero times, so control can reach the statement after the loop.
    // This is the same reasoning an `if` without an `else` uses.
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveForStmt
// ═════════════════════════════════════════════════════════════════════════════
//
// A `for` loop's binding shape depends on the iterable (§12.3):
//
//   Range                one binding — the counter
//   Table / FIND view    one binding — a row reference
//   Table, indexed       two bindings — index (uint) + row reference
//   Column view          one binding — the value
//   Array                one binding — the element
//   Array, indexed       two bindings — index (uint) + element
//
// The check is done in two stages:
//
//   1. Resolve the iterable. Get its type.
//   2. Dispatch on the type to determine the binding shape and the
//      binding types, then check the bindings the source wrote against
//      that shape.

bool resolveForStmt(ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Push the loop context and a scope for the bindings ─────────────
    ScopedContext loopCtx(ctx, ContextKind::LoopBody, stmt);
    SymbolScope bindingScope(ctx);

    // ─── Resolve the iterable ───────────────────────────────────────────
    //
    // A range is not a first-class value, so `resolveExpr` on a
    // `RangeExprAST` is dispatched to `resolveRangeExpr` in
    // `SemaExpr.cpp`, which resolves the bounds against `int` and
    // returns the bounds' type (the "range's type" for the purposes
    // of this dispatch — the loop counter's type).
    TypeAST* iterableType = resolveExpr(stmt->iterable, ctx);
    if (!iterableType || iterableType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Value_InvalidIterator, stmt->iterable,
                              "iterable has unknown type");
        return false;
    }

    // ─── Dispatch on the iterable's type ────────────────────────────────
    //
    // The four shapes map onto four type shapes:
    //   Range                 — the type is a primitive integer (the
    //                           range's bound type)
    //   Table / FIND view     — the type is a table name, or a
    //                           `&T`-returning view
    //   Column view           — the type is the column's element type
    //   Array                 — the type is an array
    //
    // The range case is distinguished by the *node*, not the type: a
    // `RangeExprAST` at the iterable position is what produces the
    // primitive-type result, and the AST check here is what keeps a
    // bare `for i: int in someIntVariable` from being misread as a
    // range loop.
    const bool isRangeIterable = stmt->iterable->isa<RangeExprAST>();

    if (isRangeIterable) {
        resolveRangeForBindings(stmt, iterableType, ctx);
    } else if (iterableType->isa<NamedTypeAST>() ||
               iterableType->isa<RowRefTypeAST>() ||
               isTableType(iterableType, ctx)) {
        resolveTableForBindings(stmt, iterableType, ctx);
    } else if (iterableType->isa<ArrayTypeAST>()) {
        resolveArrayForBindings(stmt, iterableType, ctx);
    } else {
        ctx.diagnostics.error(DiagCode::Value_InvalidIterator, stmt->iterable,
                              "cannot iterate over type ",
                              typeToString(iterableType, ctx.pool));
        return false;
    }

    // ─── Resolve the body ───────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // A `for` loop, like `while`, never transfers: a loop over an
    // empty range / empty array / empty table iterates zero times, and
    // control reaches the statement after the loop.
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveRangeForBindings
// ─────────────────────────────────────────────────────────────────────────────
//
// `for i: int in 0..<10 { ... }`
//
// One binding: the loop counter. Its type must equal the range's bound
// type (which is the primitive the range resolves to). A second binding
// is a mistake.
//
// The return value is always `false`: a binding resolver never transfers
// control. The `bool` return exists so the three binding helpers share a
// signature with the per-kind statement resolvers and with each other;
// it is the uniform protocol across the file, not a value any caller
// reads.

bool resolveRangeForBindings(ForStmtAST* stmt, TypeAST* boundType, SemaContext& ctx) {
    if (!stmt->firstVar) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->iterable,
                              "range loop requires a loop-counter binding");
        return false;
    }

    if (stmt->secondVar) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->secondVar,
                              "range loop takes exactly one binding "
                              "(the counter), got a second binding");
        return false;
    }

    // `_` is legal — the loop body doesn't need the counter. The
    // parser produces a `ParamAST` with an empty name for `_`.
    if (stmt->firstVar->name.isEmpty()) {
        // Discard binding: no type to check.
        return false;
    }

    TypeAST* declared = resolveType(stmt->firstVar->type, ctx);
    if (!declared || declared->isa<UnknownTypeAST>()) {
        stmt->firstVar->type = ctx.getUnknownType();
        return false;
    }

    if (!typesEqual(declared, boundType)) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->firstVar,
                              "range loop counter must have the same type "
                              "as the range's bounds (",
                              typeToString(boundType, ctx.pool), "), got ",
                              typeToString(declared, ctx.pool));
    }

    stmt->firstVar->type = declared;
    stmt->firstVar->resourceKind = classifyResourceKind(declared);
    ctx.insertValue(stmt->firstVar);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveTableForBindings
// ─────────────────────────────────────────────────────────────────────────────
//
// `for r: &Person in Person { ... }`
// `for i: uint, r: &Person in Person { ... }`
// `for r: &Person in Person.FIND(pred) { ... }`
//
// One binding: a row reference (`&T`). Two bindings: index (`uint`) +
// row reference. The row reference's inner table is the iterable's
// table; the source must write exactly `&T`.
//
// The return value is always `false`: a binding resolver never transfers
// control.

bool resolveTableForBindings(ForStmtAST* stmt, TypeAST* iterableType, SemaContext& ctx) {
    // ─── Determine the row reference type ───────────────────────────────
    //
    // Three shapes reach here:
    //   - The bare name `Person` — its resolved type is a `NamedTypeAST`
    //     whose resolvedDecl is a `TableDeclAST`. The row reference type
    //     is `ctx.getRowRefType(iterableType)`.
    //   - A `FIND` view — the expression `Person.FIND(pred)` returns a
    //     value whose type is a `NamedTypeAST` (the table itself, since
    //     the view is a live view over the table's rows and the grammar
    //     types it as the table). Same derivation.
    //   - A `&T` — the iterable is already a row reference. This is
    //     unusual but legal (e.g. a variable holding a row reference
    //     that the loop wants to iterate over — impossible in practice,
    //     since a single row reference has no iterable contents, but
    //     the type system does not reject it).
    //
    // The derivation is uniform: the row reference type is what the
    // *binding* should be declared as, and it is derived from the
    // iterable type by the same rules the rest of Sema uses.
    TypeAST* rowRefType = nullptr;
    if (iterableType->isa<RowRefTypeAST>()) {
        rowRefType = iterableType;
    } else if (iterableType->isa<NamedTypeAST>()) {
        rowRefType = ctx.getRowRefType(iterableType);
    } else {
        ctx.diagnostics.error(DiagCode::Value_InvalidIterator, stmt->iterable,
                              "iterating a table requires a table name or "
                              "a row-reference view");
        return false;
    }

    if (stmt->secondVar) {
        // ─── Two bindings: index + row reference ────────────────────────
        //
        // The first binding is the index; the second is the row
        // reference. The source writes `for i: uint, r: &Person in
        // Person`. The grammar fixes the index's type at `uint`; the
        // row reference's type at `&T`.
        //
        // The first binding may be `_` to discard the index; then only
        // the second binding is checked.

        if (!stmt->firstVar->name.isEmpty()) {
            TypeAST* idxType = resolveType(stmt->firstVar->type, ctx);
            if (!idxType || idxType->isa<UnknownTypeAST>()) {
                stmt->firstVar->type = ctx.getUnknownType();
            } else {
                if (!isIntegerType(idxType)) {
                    ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->firstVar,
                                          "table-loop index binding must be "
                                          "an integer, got ",
                                          typeToString(idxType, ctx.pool));
                }
                stmt->firstVar->type = idxType;
                stmt->firstVar->resourceKind = classifyResourceKind(idxType);
                ctx.insertValue(stmt->firstVar);
            }
        }

        if (!stmt->secondVar->name.isEmpty()) {
            TypeAST* rowType = resolveType(stmt->secondVar->type, ctx);
            if (!rowType || rowType->isa<UnknownTypeAST>()) {
                stmt->secondVar->type = ctx.getUnknownType();
            } else if (!typesEqual(rowType, rowRefType)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->secondVar,
                                      "table-loop value binding must be '",
                                      typeToString(rowRefType, ctx.pool),
                                      "', got ",
                                      typeToString(rowType, ctx.pool));
            } else {
                stmt->secondVar->type = rowType;
                stmt->secondVar->resourceKind = classifyResourceKind(rowType);
                ctx.insertValue(stmt->secondVar);
            }
        }
    } else {
        // ─── One binding: row reference ─────────────────────────────────
        if (stmt->firstVar->name.isEmpty()) {
            return false;   // discard binding
        }

        TypeAST* rowType = resolveType(stmt->firstVar->type, ctx);
        if (!rowType || rowType->isa<UnknownTypeAST>()) {
            stmt->firstVar->type = ctx.getUnknownType();
            return false;
        }

        if (!typesEqual(rowType, rowRefType)) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->firstVar,
                                  "table-loop binding must be '",
                                  typeToString(rowRefType, ctx.pool),
                                  "', got ",
                                  typeToString(rowType, ctx.pool));
            return false;
        }

        stmt->firstVar->type = rowType;
        stmt->firstVar->resourceKind = classifyResourceKind(rowType);
        ctx.insertValue(stmt->firstVar);
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveArrayForBindings
// ─────────────────────────────────────────────────────────────────────────────
//
// `for x: int in scores { ... }`
// `for i: uint, x: int in scores { ... }`
//
// One binding: the array's element type. Two bindings: index (`uint`) +
// element. The element type is exactly the array's declared element
// type — no row-reference indirection here, unlike the table case.
//
// The return value is always `false`: a binding resolver never transfers
// control.

bool resolveArrayForBindings(ForStmtAST* stmt, TypeAST* iterableType, SemaContext& ctx) {
    ArrayTypeAST* arrayType = iterableType->as<ArrayTypeAST>();
    TypeAST* elementType = arrayType->element;

    if (stmt->secondVar) {
        // ─── Two bindings: index + element ──────────────────────────────
        if (!stmt->firstVar->name.isEmpty()) {
            TypeAST* idxType = resolveType(stmt->firstVar->type, ctx);
            if (!idxType || idxType->isa<UnknownTypeAST>()) {
                stmt->firstVar->type = ctx.getUnknownType();
            } else {
                if (!isIntegerType(idxType)) {
                    ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->firstVar,
                                          "array-loop index binding must be "
                                          "an integer, got ",
                                          typeToString(idxType, ctx.pool));
                }
                stmt->firstVar->type = idxType;
                stmt->firstVar->resourceKind = classifyResourceKind(idxType);
                ctx.insertValue(stmt->firstVar);
            }
        }

        if (!stmt->secondVar->name.isEmpty()) {
            TypeAST* valueType = resolveType(stmt->secondVar->type, ctx);
            if (!valueType || valueType->isa<UnknownTypeAST>()) {
                stmt->secondVar->type = ctx.getUnknownType();
            } else if (!typesEqual(valueType, elementType)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->secondVar,
                                      "array-loop value binding must be '",
                                      typeToString(elementType, ctx.pool),
                                      "', got ",
                                      typeToString(valueType, ctx.pool));
            } else {
                stmt->secondVar->type = valueType;
                stmt->secondVar->resourceKind = classifyResourceKind(valueType);
                ctx.insertValue(stmt->secondVar);
            }
        }
    } else {
        // ─── One binding: element ───────────────────────────────────────
        if (stmt->firstVar->name.isEmpty()) {
            return false;   // discard binding
        }

        TypeAST* valueType = resolveType(stmt->firstVar->type, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            stmt->firstVar->type = ctx.getUnknownType();
            return false;
        }

        if (!typesEqual(valueType, elementType)) {
            ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->firstVar,
                                  "array-loop binding must be '",
                                  typeToString(elementType, ctx.pool),
                                  "', got ",
                                  typeToString(valueType, ctx.pool));
            return false;
        }

        stmt->firstVar->type = valueType;
        stmt->firstVar->resourceKind = classifyResourceKind(valueType);
        ctx.insertValue(stmt->firstVar);
    }

    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveReturnStmt
// ═════════════════════════════════════════════════════════════════════════════

bool resolveReturnStmt(ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── Must be inside a function ──────────────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Type_MissingReturn, stmt,
                              "'return' outside of a function body");
        return true;
    }

    // ─── Get the enclosing function's return type ───────────────────────
    //
    // A `null` return type means the function returns `unit`; a non-null
    // type is the type the function's `-> T` named. The distinction is
    // preserved by the AST so the JSON dumper can render the source
    // faithfully; Sema treats them identically (a `unit` return type
    // means "return without a value").
    TypeAST* expectedType = ctx.stack.currentReturnType();

    if (stmt->value) {
        // ─── Non-void return ────────────────────────────────────────────
        if (!expectedType || isUnitType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Type_MissingReturn, stmt,
                                  "return value provided in a function "
                                  "with no return type (expected 'unit')");
            return true;
        }

        TypeAST* valueType = resolveExprWithTarget(stmt->value, expectedType, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            return true;
        }
    } else {
        // ─── Bare return ────────────────────────────────────────────────
        //
        // A bare `return;` is legal in a `unit`-returning function. In a
        // value-returning function, it is a mistake: the function's
        // declared return type is not `unit`, so the return must supply
        // a value.
        if (expectedType && !isUnitType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Type_MissingReturn, stmt,
                                  "return statement is missing a value; "
                                  "function returns '",
                                  typeToString(expectedType, ctx.pool), "'");
            return true;
        }
    }

    // A `return` always transfers control out of the enclosing block.
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveBreakStmt / resolveContinueStmt
// ═════════════════════════════════════════════════════════════════════════════

bool resolveBreakStmt(BreakStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // A `break` is legal inside a loop or inside a switch. Labels are
    // not resolved here: the new grammar does not have labeled
    // `break`/`continue` (they are not in the statement grammar, §12's
    // `break_stmt ::= 'break'`). If the AST still has a `label` field,
    // it is always invalid.
    if (!ctx.stack.insideLoop() && !ctx.stack.insideSwitch()) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt,
                              "'break' outside of a loop or switch");
    }

    return true;
}

bool resolveContinueStmt(ContinueStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.stack.insideLoop()) {
        ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt,
                              "'continue' outside of a loop");
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// resolveExprStmt / resolveVarDeclStmt / resolveAssignStmt
// ═════════════════════════════════════════════════════════════════════════════

bool resolveExprStmt(ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;
    if (stmt->expr->hasSyntaxError) return false;

    // The expression statement's value is discarded. Resolve it for
    // its side effects.
    //
    // The grammar (§12.6) restricts an expression statement to a call
    // or a `start` expression. If the parser produced something else
    // — a literal, an identifier, a binary expression — the statement
    // has no side effects and its value is discarded. That is a
    // warning, not an error: some patterns (a discarded call result, a
    // debug-only side effect) look like this.
    TypeAST* exprType = resolveExpr(stmt->expr, ctx);
    if (!exprType || exprType->isa<UnknownTypeAST>()) {
        return false;
    }

    // The warning fires for a statement whose top-level node is not a
    // call and not a `start`. Everything else is an expression whose
    // result the user is deliberately discarding.
    const bool hasEffect =
        stmt->expr->isa<CallExprAST>() ||
        stmt->expr->isa<StartExprAST>();
    if (!hasEffect) {
        ctx.diagnostics.warning(DiagCode::Warn_DiscardedResult, stmt->expr,
                                "expression result is discarded; "
                                "this statement has no effect");
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

bool resolveVarDeclStmt(VarDeclStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->decl) return false;
    if (stmt->decl->hasSyntaxError) return false;

    // ─── Register the local's name ──────────────────────────────────────
    //
    // The contract documented in `SemaDecl.cpp` and `Sema.hpp`: a local
    // `let`/`const` is registered by this function, not by
    // `resolveVarDecl`. `resolveVarDecl` assumes the name is already in
    // scope; the caller is responsible for putting it there.
    //
    // Registration happens *before* the declaration's initializer is
    // resolved, so an initializer that references the declaration's own
    // name resolves to the (still unresolved) declaration and produces
    // an "undefined value" error. That is the correct behavior: a
    // declaration's initializer is not allowed to reference the
    // variable it is declaring (the value does not exist yet).
    ctx.insertValue(stmt->decl);

    // ─── Resolve the declaration ────────────────────────────────────────
    resolveVarDecl(stmt->decl, ctx);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

bool resolveAssignStmt(AssignStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->lhs || !stmt->rhs) return false;
    if (stmt->hasSyntaxError) return false;

    // ─── Resolve the LHS ────────────────────────────────────────────────
    //
    // The LHS must be an lvalue: an identifier, a field access on an
    // lvalue, or an index on an lvalue. `resolveExpr` on the LHS sets
    // `isLValue` as a side effect (see `resolveFieldAccessExpr`,
    // `resolveIndexExpr`, `resolveIdentifierExpr`), and this function
    // checks it.
    TypeAST* lhsType = resolveExpr(stmt->lhs, ctx);
    bool lhsUsable = lhsType && !lhsType->isa<UnknownTypeAST>();

    if (!lhsUsable) {
        // resolveExpr already emitted a diagnostic.
        return false;
    }

    // ─── L-value check ──────────────────────────────────────────────────
    if (!stmt->lhs->isLValue) {
        ctx.diagnostics.error(DiagCode::Mut_NonLValueAssignment, stmt->lhs,
                              "cannot assign to a non-lvalue expression");
        return false;
    }

    // ─── Const-binding check ────────────────────────────────────────────
    //
    // Assigning through a `const`-bound name is illegal. This covers
    // both `const x = ...` (the name itself) and `const p: &Person =
    // ...` (the reference the name holds — mutating the *row* through
    // a const binding is also illegal).
    //
    // The `isConst` flag is set on the expression by the resolvers that
    // produce an lvalue: `resolveIdentifierExpr` sets it from the
    // resolved declaration's `isConst`; `resolveFieldAccessExpr` sets
    // it from the field's `@readonly` and the object's constness.
    if (stmt->lhs->isConst) {
        ctx.diagnostics.error(DiagCode::Mut_ConstAssignment, stmt->lhs,
                              "cannot assign through a const binding");
        return false;
    }

    // ─── Compound-assignment operator check ─────────────────────────────
    //
    // A compound assignment (`x += 1`) performs the operator on the
    // current value, so the LHS must be a type the operator accepts.
    // A plain `=` accepts any type.
    if (stmt->op != AssignOp::Assign) {
        // The `??` operator has no compound form; the parser does not
        // produce one, and there is no `AssignOp` for it.
        //
        // Arithmetic compound operators (`+=`, `-=`, `*=`, `/=`, `%=`)
        // require a numeric LHS. The `+` case additionally accepts a
        // `string` LHS for concatenation (§6.8); the other arithmetic
        // operators do not.
        //
        // Bitwise compound operators (`&=`, `|=`, `^=`, `<<=`, `>>=`)
        // require an integer LHS.
        const bool isAddAssign = (stmt->op == AssignOp::AddAssign);
        const bool isArithAssign =
            (stmt->op == AssignOp::SubAssign ||
             stmt->op == AssignOp::MulAssign ||
             stmt->op == AssignOp::DivAssign ||
             stmt->op == AssignOp::ModAssign);
        const bool isBitwiseAssign =
            (stmt->op == AssignOp::BitAndAssign ||
             stmt->op == AssignOp::BitOrAssign  ||
             stmt->op == AssignOp::BitXorAssign ||
             stmt->op == AssignOp::ShlAssign    ||
             stmt->op == AssignOp::ShrAssign);

        if (isAddAssign) {
            if (!isNumericType(lhsType) && !isStringType(lhsType)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->lhs,
                                      "'+=' requires a numeric or string lvalue, got ",
                                      typeToString(lhsType, ctx.pool));
            }
        } else if (isArithAssign) {
            if (!isNumericType(lhsType)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->lhs,
                                      "compound arithmetic assignment requires "
                                      "a numeric lvalue, got ",
                                      typeToString(lhsType, ctx.pool));
            }
        } else if (isBitwiseAssign) {
            if (!isIntegerType(lhsType)) {
                ctx.diagnostics.error(DiagCode::Type_Mismatch, stmt->lhs,
                                      "compound bitwise assignment requires "
                                      "an integer lvalue, got ",
                                      typeToString(lhsType, ctx.pool));
            }
        }
    }

    // ─── Resolve the RHS against the LHS's type ─────────────────────────
    //
    // `resolveExprWithTarget` checks assignability. If the RHS is not
    // assignable to the LHS's type, the resolver emits the diagnostic.
    TypeAST* rhsType = resolveExprWithTarget(stmt->rhs, lhsType, ctx);
    (void)rhsType;   // The diagnostic, if any, is emitted by the resolver.

    // An assignment is a statement; its result is discarded. It never
    // transfers control.
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Sequence suspend points
// ═════════════════════════════════════════════════════════════════════════════
//
// Each `wait*` statement is only legal inside a `@sequence` function's
// body. The check is `ctx.stack.insideSequence()` — a context-stack
// query, not a symbol-table lookup.
//
// The argument shapes are fixed by §9.2.2:
//
//   wait(seconds)       — one `float` expression
//   waitFrames(n)       — one `uint` expression
//   waitUntil(pred,arg) — one `(T) -> bool` function value and one `T`
//   waitForEvent(expr)  — a compile-time constant (§4.1.1c)
//   waitForRequest(req) — one `&T` where `T` is `@request`-attributed
//
// The value of a `wait*` statement is always `unit`; the statement
// never transfers control (it suspends, then resumes).

bool resolveWaitStmt(WaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    if (!ctx.stack.insideSequence()) {
        ctx.diagnostics.error(DiagCode::Seq_SuspendOutsideSequence, stmt,
                              "'wait' is only legal inside a @sequence "
                              "function body");
        return false;
    }

    // The duration is a `float`. `resolveExprWithTarget` against the
    // singleton float type enforces the rule and produces a well-typed
    // argument.
    resolveExprWithTarget(stmt->seconds, ctx.getPrimitiveType(PrimitiveKind::Float32), ctx);

    return false;
}

bool resolveWaitFramesStmt(WaitFramesStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    if (!ctx.stack.insideSequence()) {
        ctx.diagnostics.error(DiagCode::Seq_SuspendOutsideSequence, stmt,
                              "'waitFrames' is only legal inside a @sequence "
                              "function body");
        return false;
    }

    // The frame count is a `uint` (§9.2.2). The grammar's `uint` is the
    // sized alias for `uint32`.
    resolveExprWithTarget(stmt->frames, ctx.getPrimitiveType(PrimitiveKind::Uint32), ctx);

    return false;
}

bool resolveWaitUntilStmt(WaitUntilStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    if (!ctx.stack.insideSequence()) {
        ctx.diagnostics.error(DiagCode::Seq_SuspendOutsideSequence, stmt,
                              "'waitUntil' is only legal inside a @sequence "
                              "function body");
        return false;
    }

    // `waitUntil(pred, arg)`: `pred` is `(T) -> bool` and `arg` is `T`.
    //
    // The two are resolved in order, with `arg` resolved against the
    // predicate's parameter type. The predicate's type is derived from
    // its shape, not from a pre-declared annotation — `waitUntil` is
    // generic over `T` in the calling convention, not in the source.
    //
    // Resolve `pred` with no target first, to get its signature. Then
    // resolve `arg` against the signature's first parameter type.
    //
    // A `pred` that is a lambda whose parameters are not annotated has
    // an inferred type from `pred`'s own resolution — the lambda's
    // parameter types are inferred from the argument the caller passes.
    // But `waitUntil(pred, arg)` has the argument in the same call, so
    // the two can be resolved as a unit. The order here is: resolve
    // `pred` freely to get a *shape* (a function type), then check
    // `arg` against the shape's parameter type. If `pred`'s parameter
    // type is not concrete (an unannotated lambda), the resolver
    // attempts to fix it from `arg`'s own type.
    TypeAST* predType = resolveExpr(stmt->predicate, ctx);
    if (!predType || predType->isa<UnknownTypeAST>()) {
        return false;
    }
    if (!predType->isa<FunctionTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Seq_WaitUntilArgTypeMismatch, stmt->predicate,
                              "'waitUntil' predicate must be a function value, got ",
                              typeToString(predType, ctx.pool));
        return false;
    }

    FunctionTypeAST* predFn = predType->as<FunctionTypeAST>();
    if (predFn->params.size() != 1) {
        ctx.diagnostics.error(DiagCode::Seq_WaitUntilArgTypeMismatch, stmt->predicate,
                              "'waitUntil' predicate must take one parameter, "
                              "got ", predFn->params.size());
        return false;
    }
    if (!predFn->returnType || !isBoolType(predFn->returnType)) {
        ctx.diagnostics.error(DiagCode::Seq_WaitUntilArgTypeMismatch, stmt->predicate,
                              "'waitUntil' predicate must return 'bool'");
        return false;
    }

    // Resolve the argument against the predicate's parameter type.
    TypeAST* argType = resolveExprWithTarget(stmt->arg, predFn->params[0], ctx);
    (void)argType;

    return false;
}

bool resolveWaitForEventStmt(WaitForEventStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    if (!ctx.stack.insideSequence()) {
        ctx.diagnostics.error(DiagCode::Seq_SuspendOutsideSequence, stmt,
                              "'waitForEvent' is only legal inside a @sequence "
                              "function body");
        return false;
    }

    // The argument is a compile-time constant naming the event. The
    // grammar's §9.2.2 says "a compile-time constant (§4.1.1c)" — the
    // same `const_expr` shape fixed-table rows use.
    //
    // The expression is resolved like any other (to give a diagnostic
    // if the name is unresolved), then the constant evaluator is
    // consulted. If the fold fails, the event is not compile-time
    // knowable and the statement is rejected.
    TypeAST* eventType = resolveExpr(stmt->event, ctx);
    if (!eventType || eventType->isa<UnknownTypeAST>()) {
        return false;
    }

    ConstantValue folded = evaluate(stmt->event, ctx);
    if (!folded.isEvaluated() || folded.isError()) {
        ctx.diagnostics.error(DiagCode::Seq_WaitForEventNotAFixedTable, stmt->event,
                              "'waitForEvent' argument must be a compile-time "
                              "constant naming an event (e.g. a fixed-table "
                              "member)");
        return false;
    }

    return false;
}

bool resolveWaitForRequestStmt(WaitForRequestStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    if (!ctx.stack.insideSequence()) {
        ctx.diagnostics.error(DiagCode::Seq_SuspendOutsideSequence, stmt,
                              "'waitForRequest' is only legal inside a @sequence "
                              "function body");
        return false;
    }

    // The argument must be a row reference (`&T`) whose `T` is declared
    // `@request` (§9.2.3).
    //
    // Resolution:
    //   1. Resolve the argument. Its type must be `&T`.
    //   2. Follow the `T` to its `TableDeclAST`.
    //   3. Confirm `T->isHostBacked` and `T->isRequest`.
    //
    // A non-`&T` argument, an `&T` whose table is not `@request`, and
    // an `&T` whose table is not host-backed are all rejected.
    TypeAST* reqType = resolveExpr(stmt->request, ctx);
    if (!reqType || reqType->isa<UnknownTypeAST>()) {
        return false;
    }

    if (!isRowRefType(reqType)) {
        ctx.diagnostics.error(DiagCode::Seq_WaitForRequestNotARequest, stmt->request,
                              "'waitForRequest' argument must be a row "
                              "reference to an @request host type, got ",
                              typeToString(reqType, ctx.pool));
        return false;
    }

    RowRefTypeAST* rowRef = reqType->as<RowRefTypeAST>();
    if (!rowRef->inner || !rowRef->inner->isa<NamedTypeAST>()) {
        return false;
    }
    NamedTypeAST* named = rowRef->inner->as<NamedTypeAST>();
    if (!named->resolvedDecl || !named->resolvedDecl->isa<TableDeclAST>()) {
        return false;
    }
    TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();
    if (!table->isHostBacked || !table->isRequest) {
        ctx.diagnostics.error(DiagCode::Seq_WaitForRequestNotARequest, stmt->request,
                              "'waitForRequest' argument must be a row "
                              "reference to an @request host type; '",
                              ctx.pool.lookup(table->name),
                              "' is not @request-attributed");
        return false;
    }

    return false;
}

} // namespace lucid::sema