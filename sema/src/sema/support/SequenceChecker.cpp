/// @file sema/support/SequenceChecker.cpp
/// @brief Implementation of the `@sequence` signature and body checks.
///
/// ─── The two rules in one place ───────────────────────────────────────────
/// §9.2.5 lists five restrictions on `@sequence` functions. Three of
/// them are use-site rules (about calling a sequence), not
/// declaration-site rules; they live in `SemaExpr.cpp`. The two that
/// are declaration-site rules are checked here:
///
///   - a `@sequence` function always returns `unit`;
///   - a `@sequence` function is never host-bound.
///
/// Both are decidable from the function's signature alone. They run in
/// pass 2, alongside the rest of the signature resolution.
///
/// A third check — "does the body actually suspend?" — is a warning
/// rather than a rule. It runs in pass 3, after the body has been
/// resolved, because it needs to see the body's statements.
///
/// ─── Design: the body walk is a single function ───────────────────────────
/// The body warning is a single-pass walk over the body block, counting
/// `Wait*StmtAST` nodes. The walk descends into `if`, `while`, `for`,
/// and `switch` bodies, because a `wait*` inside a nested block still
/// suspends the sequence. The walk does not descend into a nested
/// function — the grammar does not allow nested function declarations
/// (§12.5), so there is nothing to skip.

#include "SequenceChecker.hpp"

#include "sema/types/SemaType.hpp"
#include "sema/types/SemaType.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// checkSequenceSignature (pass 2)
// ═════════════════════════════════════════════════════════════════════════════

bool checkSequenceSignature(FnDeclAST* fn, SemaContext& ctx) {
    if (!fn) return true;
    if (fn->hasSyntaxError) return true;

    // The rules apply only to `@sequence` functions. An ordinary `FN`
    // has no restrictions on its return type or body shape.
    if (!fn->isSequence) return true;

    bool ok = true;

    // ─── Rule 1: a @sequence returns `unit` ─────────────────────────────
    //
    // A `@sequence` function is launched with `start` and produces a
    // `&Coroutine` handle at the launch site. The sequence itself
    // cannot return a value: the caller does not wait for it, and the
    // sequence's completion is signalled through the handle, not
    // through a return value.
    //
    // The check is on `fn->returnType`. A null return type means the
    // function returns `unit`; a non-null one is the type the arrow
    // named. Both an explicit `-> unit` and no arrow at all are legal;
    // any other type is not.
    if (fn->returnType && !isUnitType(fn->returnType)) {
        ctx.diagnostics.error(DiagCode::Seq_SequenceReturnsValue, fn,
                              "@sequence function '",
                              ctx.pool.lookup(fn->name),
                              "' declares a return type of '",
                              typeToString(fn->returnType, ctx.pool),
                              "' — a sequence always returns 'unit'");
        ctx.diagnostics.note(fn,
                             "a sequence cannot return a value; if a "
                             "caller needs a result, have the sequence "
                             "write it into a table row, or call an "
                             "ordinary FN as the sequence's last step");
        ok = false;
    }

    // ─── Rule 2: a @sequence is not host-bound ──────────────────────────
    //
    // A `host(...)` body says "the implementation of this function is
    // on the C++ side". A sequence's implementation is a compiler-
    // generated state machine — the compiler has to lower the body to
    // a step function that can suspend and resume. There is no way for
    // a native function to participate in that lowering; the
    // restriction is that a sequence is always Lucid-bodied.
    if (fn->isHostBound) {
        ctx.diagnostics.error(DiagCode::Seq_SequenceHasHostBody, fn,
                              "@sequence function '",
                              ctx.pool.lookup(fn->name),
                              "' has a host(...) body — a sequence's body "
                              "must be compiled Lucid, because suspension "
                              "requires a compiler-generated state machine");
        ctx.diagnostics.note(fn,
                             "a @sequence cannot be implemented by a "
                             "native function; if the work needs native "
                             "code, call a host-bound FN from inside the "
                             "sequence's body");
        ok = false;
    }

    return ok;
}

// ═════════════════════════════════════════════════════════════════════════════
// checkSequenceBody (pass 3)
// ═════════════════════════════════════════════════════════════════════════════

// ─── The suspend-point detector ─────────────────────────────────────────────
//
// A single boolean: "does this body contain a `wait*` statement?" The
// walk visits every statement in the body and every nested block,
// because a `wait*` inside an `if` or a `for` still suspends the
// sequence.
//
// The walk does not need to classify which `wait*` or count how many.
// The warning fires when the count is zero; any non-zero count means
// the body suspends.
//
// `waitForEvent` and `waitForRequest` are the two push-based
// suspend points (§9.2.3). They register the sequence and do no work
// until the host resumes it — but they are still suspend points, and a
// body that pauses on them is a sequence. Both count.

static bool bodyContainsSuspendPoint(const StmtAST* stmt);

static bool blockContainsSuspendPoint(const BlockStmtAST* block) {
    if (!block) return false;
    for (const StmtAST* stmt : block->stmts) {
        if (bodyContainsSuspendPoint(stmt)) return true;
    }
    return false;
}

static bool bodyContainsSuspendPoint(const StmtAST* stmt) {
    if (!stmt) return false;

    switch (stmt->kind) {
        // ─── The five suspend points ────────────────────────────────────
        case ASTKind::WaitStmt:
        case ASTKind::WaitFramesStmt:
        case ASTKind::WaitUntilStmt:
        case ASTKind::WaitForEventStmt:
        case ASTKind::WaitForRequestStmt:
            return true;

        // ─── Nested blocks ──────────────────────────────────────────────
        case ASTKind::BlockStmt:
            return blockContainsSuspendPoint(stmt->as<BlockStmtAST>());

        // ─── Control-flow wrappers ──────────────────────────────────────
        //
        // A `wait*` inside an `if`, `switch`, `while`, or `for` still
        // suspends the sequence. The walk descends into all of them.
        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            if (ifStmt->thenBranch && bodyContainsSuspendPoint(ifStmt->thenBranch)) {
                return true;
            }
            if (ifStmt->elseBranch && bodyContainsSuspendPoint(ifStmt->elseBranch)) {
                return true;
            }
            return false;
        }

        case ASTKind::SwitchStmt: {
            const SwitchStmtAST* sw = stmt->as<SwitchStmtAST>();
            for (const SwitchCaseAST* caseClause : sw->cases) {
                if (caseClause && caseClause->body &&
                    blockContainsSuspendPoint(caseClause->body)) {
                    return true;
                }
            }
            if (sw->defaultBody && blockContainsSuspendPoint(sw->defaultBody)) {
                return true;
            }
            return false;
        }

        case ASTKind::WhileStmt: {
            const WhileStmtAST* w = stmt->as<WhileStmtAST>();
            return w->body && blockContainsSuspendPoint(w->body);
        }

        case ASTKind::ForStmt: {
            const ForStmtAST* f = stmt->as<ForStmtAST>();
            return f->body && blockContainsSuspendPoint(f->body);
        }

        // ─── Everything else is not a suspend point ─────────────────────
        //
        // An expression statement, an assignment, a local `let`, a
        // `return`, a `break`, a `continue` — none of them suspend. A
        // `wait*` cannot be nested inside another statement except
        // through a control-flow wrapper, which is handled above.
        default:
            return false;
    }
}

bool checkSequenceBody(FnDeclAST* fn, SemaContext& ctx) {
    if (!fn) return true;
    if (fn->hasSyntaxError) return true;
    if (!fn->isSequence) return true;

    // A host-bound sequence is already reported by `checkSequenceSignature`
    // as an error. There is no body to walk; skip the body check so the
    // diagnostic count stays at one error per problem.
    if (fn->isHostBound) return true;

    // A sequence without a body is a compiler bug, not a user error —
    // the parser produces either a body block or a `host(...)` target.
    // `resolveFnBody` asserts on it; the checker trusts the assertion
    // and returns cleanly if it ever sees the shape.
    if (!fn->body) return true;

    if (blockContainsSuspendPoint(fn->body)) {
        return true;
    }

    ctx.diagnostics.warning(DiagCode::Warn_SequenceNeverSuspends, fn,
                            "@sequence function '",
                            ctx.pool.lookup(fn->name),
                            "' never suspends — its body contains no "
                            "'wait', 'waitFrames', 'waitUntil', "
                            "'waitForEvent', or 'waitForRequest'");
    ctx.diagnostics.note(fn,
                         "a @sequence that never suspends behaves like an "
                         "ordinary FN; consider removing the '@sequence' "
                         "attribute");
    return false;
}

} // namespace lucid::sema