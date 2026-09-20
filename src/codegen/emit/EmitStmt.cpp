/// @file codegen/emit/EmitStmt.cpp
/// @brief Statement lowering — the `Emitter::emit(StmtAST*)` entry point
///        and its per-kind dispatch.
///
/// ─── Statements Don't Produce Values ──────────────────────────────────────
/// `void emit(StmtAST*)` — statements have no value, no ownership tag.
/// The only ownership interaction is at expression statements, where an
/// `Owned` temporary with no consumer must be dropped.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/support/Truthiness.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emit — the public dispatch
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emit(StmtAST* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:      return emitBlock(stmt->as<BlockStmtAST>());
        case ASTKind::IfStmt:         return emitIfStmt(stmt->as<IfStmtAST>());
        case ASTKind::SwitchStmt:     return emitSwitchStmt(stmt->as<SwitchStmtAST>());
        case ASTKind::ForStmt:        return emitForStmt(stmt->as<ForStmtAST>());
        case ASTKind::WhileStmt:      return emitWhileStmt(stmt->as<WhileStmtAST>());
        case ASTKind::DoWhileStmt:    return emitDoWhileStmt(stmt->as<DoWhileStmtAST>());
        case ASTKind::ReturnStmt:     return emitReturnStmt(stmt->as<ReturnStmtAST>());
        case ASTKind::BreakStmt:      return emitBreakStmt(stmt->as<BreakStmtAST>());
        case ASTKind::ContinueStmt:   return emitContinueStmt(stmt->as<ContinueStmtAST>());
        case ASTKind::ExprStmt:       return emitExprStmt(stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:       return emitDeclStmt(stmt->as<DeclStmtAST>());
        case ASTKind::AsyncStmt:      return emitAsyncStmt(stmt->as<AsyncStmtAST>());
        case ASTKind::AwaitStmt:      return emitAwaitStmt(stmt->as<AwaitStmtAST>());
        case ASTKind::SpawnStmt:      return emitSpawnStmt(stmt->as<SpawnStmtAST>());
        case ASTKind::JoinStmt:       return emitJoinStmt(stmt->as<JoinStmtAST>());

        default:
            // Sema should have rejected unknown kinds.
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitBlock — a lexical scope
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitBlock(BlockStmtAST* block) {
    assert(block && "emitBlock() with null block");

    // Push a scope for the block's bindings.
    func().pushScope(block);

    for (StmtAST* stmt : block->stmts) {
        emit(stmt);

        // If the current block ended in a terminator (return, break,
        // continue), the remaining statements are unreachable. Sema
        // already warned; the emitter stops emitting them.
        llvm::BasicBlock* cur = program.builder().GetInsertBlock();
        if (cur && cur->getTerminator()) break;
    }

    // Pop the scope. Its cleanup (drops for any still-alive bindings) is
    // emitted by `popScope` — or rather, by the emitter's cleanup path
    // that `popScope` calls into.
    //
    // NOTE: in the transitional shim, `popLiveScope` handles cleanup.
    // Task 5's emitter will move that logic into the emitter's scope
    // management.
}

// ─────────────────────────────────────────────────────────────────────────────
// emitReturnStmt — return with unwinding
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Order of Operations ──────────────────────────────────────────────────
// 1. Emit the return value (before any cleanup).
// 2. Acquire a fresh claim (intoOwned) so the caller gets an Owned value.
// 3. Unwind all scopes to depth 0, emitting drops for every alive binding.
// 4. Emit `ret`.
//
// The order matters: the return value's expression may reference bindings
// that the unwind is about to drop. Evaluating the value before unwinding
// is what makes `return someLocal;` correct.
//
// ─── The Return-Value Retain Rule ─────────────────────────────────────────
// If the return expression is `Borrowed` (a load from a local binding),
// the caller gets a claim that outlives the local's scope. `intoOwned`
// acquires a fresh claim (retains for closures, deep-copies for strings).
// If the return expression is `Owned` (a fresh value), no additional
// claim is needed — the temp claim transfers to the caller.

void Emitter::emitReturnStmt(ReturnStmtAST* stmt) {
    assert(stmt && "emitReturnStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Step 1: Emit the return value ────────────────────────────────────
    Val returnVal;
    if (stmt->value) {
        returnVal = emit(stmt->value);
        if (!returnVal.isValid()) return;

        // ─── Coerce to the declared return type ───────────────────────────
        // `fn → cls` widening, integer widening, pointer casts — all the
        // coercions the old `lowerReturnStmt` did.
        returnVal = coerceTo(returnVal, func().declaredReturnType(), *this);
        if (!returnVal.isValid()) return;

        // ─── Step 2: Acquire a fresh claim ────────────────────────────────
        // The caller owns the returned value. If it was Borrowed, acquire
        // a fresh claim before unwinding.
        returnVal = program.ownership().intoOwned(returnVal, b);
    }

    // ─── Step 3: Unwind all scopes ────────────────────────────────────────
    // Walk every scope from the innermost to the function's outermost,
    // emitting drops for every alive binding. This must run after Step 1
    // (the return value is already computed) and before Step 4 (the ret
    // terminates the block).
    emitUnwindTo(0);

    // ─── Step 4: Emit the ret ─────────────────────────────────────────────
    if (returnVal.isValid()) {
        llvm::Type* retTy = program.types().get(
            func().declaredReturnType());
        if (retTy && returnVal.v->getType() != retTy) {
            returnVal.v = coerceValueToType(returnVal.v, retTy, b);
        }
        b.CreateRet(returnVal.v);
    } else {
        b.CreateRetVoid();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitExprStmt — a discarded temporary
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Discarded-Temporary Rule ─────────────────────────────────────────
// An expression statement like `f();` produces a value whose claim, if
// `Owned`, has no consumer. The statement must drop it — otherwise the
// temp claim leaks.
//
// A `Borrowed` result is not dropped: the source binding still owns the
// claim, and dropping the alias would double-free.

void Emitter::emitExprStmt(ExprStmtAST* stmt) {
    assert(stmt && "emitExprStmt() with null statement");

    Val result = emit(stmt->expr);

    // If the result is a fresh claim with no consumer, drop it.
    if (result.isValid() && result.own == Own::Owned) {
        program.ownership().drop(result.ty, result.v, program.builder());
    }
}

} // namespace codegen