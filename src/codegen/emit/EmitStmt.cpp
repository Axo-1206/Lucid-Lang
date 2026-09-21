/// @file codegen/emit/EmitStmt.cpp
/// @brief Statement lowering — the `Emitter::emit(StmtAST*)` entry point,
///        its per-kind dispatch, and the scope-management primitives.
///
/// ─── Statements Don't Produce Values ──────────────────────────────────────
/// `void emit(StmtAST*)` — statements have no value, no ownership tag. The
/// only ownership interactions are:
///
///   - At expression statements, where an `Owned` temporary with no
///     consumer must be dropped.
///   - At scope exit (fall-through or unwind), where every still-alive
///     binding must be released.
///   - At `return`, where the return value is acquired fresh, then all
///     scopes are unwound, then the `ret` is emitted.
///
/// ─── The Three Cleanup Primitives ─────────────────────────────────────────
/// Cleanup is the part of the emitter most prone to getting wrong. The
/// design funnels all of it through three primitives:
///
///   - `createEntryAlloca(ty, name)` — every alloca the emitter creates
///     goes in the function's entry block, not the current block. A `let`
///     inside a loop body must not allocate a fresh slot per iteration.
///
///   - `emitScopeFallthrough()` — emit drops for the current scope's alive
///     bindings, in reverse declaration order, then clear the alive set.
///     Called at the natural end of a block (`emitBlock`) and at the
///     natural end of a function body (`emitFuncBody`, `emitClosureBody`).
///
///   - `emitUnwindTo(targetDepth)` — emit drops for every scope from the
///     innermost down to (but not including) `targetDepth`, in
///     inner-to-outer order, WITHOUT popping the scopes. Called by
///     `return` (target 0), `break` and `continue` (target = the loop's
///     entry depth).
///
/// The split between fall-through and unwind is what makes the design
/// correct. Fall-through is destructive: the scope is done, cleanup runs
/// once. Unwind is non-destructive: the `return`/`break`/`continue` branch
/// exits the block, and the structurally-paired `popScope` at the block's
/// natural end will pop the already-cleaned scope.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emit — the public dispatch
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emit(StmtAST* stmt) {
    if (!stmt) return;

    // A statement reached after its block already terminated (a return,
    // break, or continue earlier in the same block) is unreachable. Sema
    // already warned; the emitter drops it.
    if (llvm::BasicBlock* cur = program.builder().GetInsertBlock()) {
        if (cur->getTerminator()) return;
    }

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

        // Concurrency statements are implemented in EmitConcurrency.cpp.
        case ASTKind::AsyncStmt:      return emitAsyncStmt(stmt->as<AsyncStmtAST>());
        case ASTKind::AwaitStmt:      return emitAwaitStmt(stmt->as<AwaitStmtAST>());
        case ASTKind::SpawnStmt:      return emitSpawnStmt(stmt->as<SpawnStmtAST>());
        case ASTKind::JoinStmt:       return emitJoinStmt(stmt->as<JoinStmtAST>());

        default:
            // Sema should have rejected unknown kinds. Reaching here is a
            // compiler bug; the safest response is to emit nothing.
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// createEntryAlloca — the entry-block allocation helper
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Why This Exists ──────────────────────────────────────────────────────
// `IRBuilder::CreateAlloca` inserts the alloca at the current insertion
// point. If the current block is inside a loop, the alloca is emitted
// inside the loop, and a fresh stack slot is allocated every iteration.
// The slots accumulate until the function returns — a stack leak that
// scales with iteration count.
//
// The standard fix (used by every production compiler) is to put allocas
// in the function's entry block, which executes exactly once. This helper
// does that: it saves the insertion point, moves to the entry block (just
// after the last existing instruction), creates the alloca, and restores
// the insertion point.
//
// ─── Contract ─────────────────────────────────────────────────────────────
// Requires an active `llvm::Function*` with at least one basic block (the
// entry block). Both are guaranteed inside a function body or closure body.
// Returns null if either is missing, so callers can bail gracefully.

llvm::AllocaInst* Emitter::createEntryAlloca(llvm::Type* ty,
                                              const llvm::Twine& name) {
    if (!ty) return nullptr;

    llvm::IRBuilder<>& b = program.builder();

    llvm::BasicBlock* cur = b.GetInsertBlock();
    if (!cur) return nullptr;
    llvm::Function* fn = cur->getParent();
    if (!fn) return nullptr;

    llvm::BasicBlock& entry = fn->getEntryBlock();

    // Save where we were. `InsertPointGuard` restores on scope exit, but
    // we want the alloca to have a name, and the guard form doesn't play
    // well with the extra local. Use saveIP/restoreIP explicitly.
    llvm::IRBuilderBase::InsertPoint saved = b.saveIP();

    // Move to the start of the entry block (after the last existing
    // instruction), so multiple entry allocas don't reorder each other.
    b.SetInsertPoint(&entry, entry.getFirstInsertionPt());

    llvm::AllocaInst* alloca = b.CreateAlloca(ty, nullptr, name);

    // Restore the insertion point.
    b.restoreIP(saved);

    return alloca;
}

void Emitter::dropScopeAlive(Scope& scope) {
    llvm::IRBuilder<>& b = program.builder();

    // Piece 1: run #scope_exit callbacks first, reverse registration order.
    // They run before the binding drops so a callback can reference a
    // binding that's still alive.
    //
    // #scope_exit callbacks run LIFO, before the bindings they may
    // reference are dropped. Iterate the block's registrations in
    // reverse registration order.
    if (scope.block) {
        auto exits = scope.block->scopeExits;
        for (size_t i = exits.size(); i > 0; --i) {
            emitScopeExitCallback(exits[i - 1]);
        }
    }

    // Piece 2: drop every still-alive binding, reverse declaration order.
    for (auto it = scope.declarationOrder.rbegin();
         it != scope.declarationOrder.rend(); ++it) {
        ValueDeclAST* decl = *it;
        if (!decl || !scope.isAlive(decl)) continue;
        llvm::Value* binding = func().lookupValue(decl);
        if (!binding) continue;
        llvm::Value* value = binding;
        if (binding->getType()->isPointerTy() && !decl->isa<FuncDeclAST>()) {
            llvm::Type* ty = program.types().get(decl->type);
            if (!ty) continue;
            value = b.CreateLoad(ty, binding, "scope_drop_load");
        }
        program.ownership().drop(decl->type, value, b);
    }
    scope.alive.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitScopeFallthrough — destructive cleanup for the current scope
// ─────────────────────────────────────────────────────────────────────────────
//
// Called at the natural end of a block or function body. Emits drops for
// every binding in the current scope's `alive` set, in reverse
// registration order, then clears the set. The scope is NOT popped —
// `popScope` does that, and the two are always called together:
//
//     emitScopeFallthrough();
//     func().popScope();
//
// ─── Reverse Order ────────────────────────────────────────────────────────
// Drops run in reverse declaration order. If `let a = ...` is declared
// before `let b = ...`, and `b` holds a reference into `a`, releasing `b`
// before `a` is correct: `a` is the source, so it must outlive `b`.
//
// ─── Why `alive` and Not `consumed` ───────────────────────────────────────
// `Scope::alive` and `Scope::consumed` are disjoint by construction:
// `markConsumed` removes from `alive` and inserts into `consumed`. So
// iterating `alive` skips everything already moved out. That's the whole
// point of the two-set design.
//
// ─── `Scope::alive` Is an Unordered Set ───────────────────────────────────
// `Scope::alive` is `std::unordered_set<ValueDeclAST*>`. Its iteration
// order is unspecified. For most programs that's fine — bindings that
// don't reference each other can drop in any order. But for correctness
// we need a deterministic order that respects declaration nesting.
//
// The `FunctionState` doesn't currently track declaration order within a
// scope. Two options:
//
//   1. Add a `std::vector<ValueDeclAST*> declarationOrder` to `Scope`,
//      appended by `markAlive`, and iterate that in reverse.
//
//   2. Accept unordered drops and rely on Sema to reject programs where
//      drop order matters (a `let` holding a reference into an earlier
//      `let` in the same scope).
//
// Sema already rejects `&T` fields and `&T` returns, so intra-scope
// reference chains are the only case where order matters, and they're
// rare. For now, use option 2 (unordered) and note the limitation. A
// future revision of `Scope` can add the ordered vector without changing
// this function's call sites.

// ─────────────────────────────────────────────────────────────────────────────
// emitScopeFallthrough — shared helper
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Emit drops for every still-alive binding in a scope, in reverse
///        declaration order, and clear the scope's `alive` set.
///
/// Shared by `emitScopeFallthrough` (called on the current scope) and
/// `emitUnwindTo` (called on each scope in the unwind range). The caller
/// is responsible for checking that the current insertion block isn't
/// already terminated — this helper assumes it can append.
void Emitter::emitScopeFallthrough() {
    if (func().scopeDepth() == 0) return;
    Scope& scope = func().currentScope();
    if (scope.alive.empty()) return;
    if (llvm::BasicBlock* cur = program.builder().GetInsertBlock();
        !cur || cur->getTerminator()) {
        scope.alive.clear();
        return;
    }
    dropScopeAlive(scope);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitUnwindTo — non-destructive cleanup for a stack of scopes
// ─────────────────────────────────────────────────────────────────────────────
//
// Called by `return` (target 0), `break` and `continue` (target = the
// loop's entry scope depth). Walks the scope stack from the innermost
// scope down to (but not including) `targetDepth`, emitting drops for
// every alive binding in each. Scopes are NOT popped — the
// structurally-paired `popScope` at each block's natural end pops them.
//
// ─── Why Non-Destructive ──────────────────────────────────────────────────
// `break` inside a nested block:
//
//     while cond {
//         {
//             let x = ...;
//             break;       // unwinds to the loop's depth
//         }
//         // unreachable
//     }
//
// The inner block's `emitBlock` pushed a scope. After `break`, the
// emitter's `if (cur->getTerminator()) break;` in `emitBlock`'s loop
// stops emitting further statements, but `emitBlock` still runs its
// `emitScopeFallthrough(); func().popScope();` epilogue. If
// `emitUnwindTo` had popped the scope, `emitBlock`'s epilogue would be
// popping the wrong scope.
//
// The solution: `emitUnwindTo` emits the drops (so the resources are
// released at the right point in the IR) and clears the alive sets (so
// the epilogue's `emitScopeFallthrough` is a no-op), but leaves the scope
// stack intact.

void Emitter::emitUnwindTo(size_t targetDepth) {
    std::vector<Scope>& stack = func().scopeStack();
    bool blockTerminated =
        !program.builder().GetInsertBlock()
        || program.builder().GetInsertBlock()->getTerminator();

    for (size_t i = stack.size(); i > targetDepth; --i) {
        Scope& scope = stack[i - 1];
        if (scope.alive.empty()) continue;
        if (blockTerminated) {
            scope.alive.clear();
            continue;
        }
        dropScopeAlive(scope);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitBlock — a lexical scope
// ─────────────────────────────────────────────────────────────────────────────
//
// A block pushes a scope, emits its statements in order, stops at the
// first terminator, emits fall-through cleanup for the scope, and pops it.
//
// ─── Stopping at the First Terminator ─────────────────────────────────────
// If a statement in the block is a `return`, `break`, or `continue`, the
// IR after it is unreachable. Sema already warned. The emitter stops
// emitting further statements to avoid appending to a terminated block,
// which LLVM rejects.

void Emitter::emitBlock(BlockStmtAST* block) {
    assert(block && "emitBlock() with null block");

    // ─── Push the block's scope ───────────────────────────────────────────
    // The `BlockStmtAST*` is stored so `#scope_exit` callbacks (if Sema
    // registered any) can be looked up at cleanup time. Today, the
    // emitter doesn't emit `#scope_exit` callbacks — that's a follow-up.
    func().pushScope(block);

    // ─── Emit the statements ──────────────────────────────────────────────
    for (StmtAST* stmt : block->stmts) {
        emit(stmt);

        // Stop at the first terminator. The remaining statements are
        // unreachable; the emitter never appends to a terminated block.
        if (llvm::BasicBlock* cur = program.builder().GetInsertBlock()) {
            if (cur->getTerminator()) break;
        }
    }

    // ─── Fall-through cleanup and pop ─────────────────────────────────────
    // If the block terminated early (a `return`/`break`/`continue` inside
    // it), that terminator's unwind already cleared the scope's alive set,
    // so `emitScopeFallthrough` is a no-op. If the block fell through,
    // this releases any still-alive bindings.
    emitScopeFallthrough();
    func().popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIfStmt — conditional statement
// ─────────────────────────────────────────────────────────────────────────────
//
// Basic-block shape:
//
//     +--------+  cond false  +-------+
//     | header |------------->| merge |
//     +---+----+              +-------+
//         | cond true             ▲
//         ▼                       |
//     +------+                    |
//     | then |------ br ----------+
//     +------+
//
// With an `else`:
//
//     +--------+  cond false  +------+      +-------+
//     | header |------------->| else |----->| merge |
//     +---+----+              +------+      +-------+
//         | cond true             ▲
//         ▼                       |
//     +------+                    |
//     | then |------ br ----------+
//     +------+
//
// ─── Else Chaining ────────────────────────────────────────────────────────
// `else if` is represented as an `IfStmtAST` in `elseBranch`. The emitter
// recurses into `emitIfStmt` for that case. The recursion terminates
// because the else-if chain is finite and ends in either a block or null.

void Emitter::emitIfStmt(IfStmtAST* stmt) {
    assert(stmt && "emitIfStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Emit and coerce the condition ────────────────────────────────────
    Val cond = emit(stmt->condition);
    if (!cond.isValid()) return;

    llvm::Value* condI1 = emitTruthiness(cond);
    if (!condI1) return;

    // ─── Create the blocks ────────────────────────────────────────────────
    llvm::BasicBlock* thenBlock =
        llvm::BasicBlock::Create(program.llvmContext(), "if.then", fn);
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(program.llvmContext(), "if.merge", fn);
    llvm::BasicBlock* elseBlock = nullptr;

    if (stmt->elseBranch) {
        elseBlock =
            llvm::BasicBlock::Create(program.llvmContext(), "if.else", fn);
    } else {
        elseBlock = mergeBlock;
    }

    b.CreateCondBr(condI1, thenBlock, elseBlock);

    // ─── Then branch ──────────────────────────────────────────────────────
    b.SetInsertPoint(thenBlock);
    if (stmt->thenBranch) {
        emit(stmt->thenBranch);
    }
    // If the branch terminated (a `return`/`break`/`continue` inside it),
    // don't append a fall-through. Otherwise, branch to the merge.
    if (!b.GetInsertBlock()->getTerminator()) {
        b.CreateBr(mergeBlock);
    }

    // ─── Else branch ──────────────────────────────────────────────────────
    if (stmt->elseBranch) {
        b.SetInsertPoint(elseBlock);
        emit(stmt->elseBranch);
        if (!b.GetInsertBlock()->getTerminator()) {
            b.CreateBr(mergeBlock);
        }
    }

    // ─── Merge block ──────────────────────────────────────────────────────
    // If both branches terminated (both returned / broke), the merge block
    // is unreachable. That's valid IR; LLVM's verifier accepts it and the
    // optimiser removes it. We still set the insertion point to it so
    // subsequent statements have a block to append to — but they'll be
    // unreachable, and `emit` checks for a terminator before appending.
    b.SetInsertPoint(mergeBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitSwitchStmt — value dispatch
// ─────────────────────────────────────────────────────────────────────────────
//
// Switch over an integer, bool, char, or enum subject. Each case has a
// list of literal values (or enum variants) and a body block. `default` is
// optional.
//
// ─── Supported Case Values ────────────────────────────────────────────────
// Today, only integer and enum-variant case values are supported. String
// case values and range case values (`case 1..10:`) are Sema-approved but
// not yet lowered. They'd be handled by a chain of comparisons instead of
// an LLVM `switch` instruction. A future revision adds those.
//
// ─── Fall-through ─────────────────────────────────────────────────────────
// Lucid has no fall-through. Each case body ends with a branch to the
// merge block, unless the body itself terminated (a `return`/`break`).

void Emitter::emitSwitchStmt(SwitchStmtAST* stmt) {
    assert(stmt && "emitSwitchStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Emit the subject ─────────────────────────────────────────────────
    Val subject = emit(stmt->subject);
    if (!subject.isValid()) return;

    // Sema guarantees the subject is integer, bool, char, or enum. All of
    // these are LLVM integers. If the subject is a load, `subject.v` is
    // the loaded SSA value; no explicit load is needed here.
    llvm::Value* subjectVal = subject.v;
    if (!subjectVal->getType()->isIntegerTy()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->subject->loc,
            "switch subject must be an integer, bool, char, or enum");
        return;
    }

    // ─── Create the merge block ───────────────────────────────────────────
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(program.llvmContext(), "switch.merge", fn);

    // ─── Create a block per case ──────────────────────────────────────────
    // One body block per case (Lucid has no fall-through). The body block
    // receives control from the switch instruction for every case value
    // that maps to it.
    std::vector<llvm::BasicBlock*> caseBlocks;
    caseBlocks.reserve(stmt->cases.size());
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        caseBlocks.push_back(llvm::BasicBlock::Create(
            program.llvmContext(), "switch.case", fn));
    }

    // ─── Default block ────────────────────────────────────────────────────
    llvm::BasicBlock* defaultBlock = nullptr;
    if (stmt->defaultBody) {
        defaultBlock = llvm::BasicBlock::Create(
            program.llvmContext(), "switch.default", fn);
    } else {
        defaultBlock = mergeBlock;
    }

    // ─── Build the switch instruction ─────────────────────────────────────
    // Collect (value, target block) pairs from every case's value list.
    // Only integer-typed values are supported; a range or string value
    // is diagnosed and skipped.
    llvm::SwitchInst* switchInst = b.CreateSwitch(
        subjectVal, defaultBlock, stmt->cases.size());

    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        SwitchCaseAST* caseStmt = stmt->cases[i];
        if (!caseStmt) continue;

        for (ExprAST* valueExpr : caseStmt->values) {
            if (!valueExpr) continue;

            Val value = emit(valueExpr);
            if (!value.isValid()) continue;

            // The case value must be an integer constant. A load or a
            // runtime computation isn't a valid switch case.
            llvm::ConstantInt* constVal =
                llvm::dyn_cast<llvm::ConstantInt>(value.v);
            if (!constVal) {
                program.diagnostics.errorAt(
                    DiagCode::Sem_InvalidSwitchType, valueExpr->loc,
                    "switch case value must be a compile-time integer constant");
                continue;
            }

            // Sema's type checker guarantees the case value's type matches
            // the subject's type. The assert fires in debug builds if Sema
            // ever lets a mismatched case through — that's the point: fail
            // fast at the source of the bug, not at an `addCase` call two
            // frames down.
            //
            // The `llvm::SwitchInst::addCase` API also asserts this in debug
            // builds, so an out-of-type `ConstantInt` is caught either way.
            // We add our own assert for a clearer message.
            assert(constVal->getType() == subjectVal->getType()
                && "switch case value type differs from subject type — "
                    "Sema should have rejected this");

            switchInst->addCase(constVal, caseBlocks[i]);
        }
    }

    // ─── Emit each case body ──────────────────────────────────────────────
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        SwitchCaseAST* caseStmt = stmt->cases[i];
        if (!caseStmt) continue;

        b.SetInsertPoint(caseBlocks[i]);
        if (caseStmt->body) {
            emit(caseStmt->body);
        }
        if (!b.GetInsertBlock()->getTerminator()) {
            b.CreateBr(mergeBlock);
        }
    }

    // ─── Emit the default body ────────────────────────────────────────────
    if (stmt->defaultBody) {
        b.SetInsertPoint(defaultBlock);
        emit(stmt->defaultBody);
        if (!b.GetInsertBlock()->getTerminator()) {
            b.CreateBr(mergeBlock);
        }
    }

    // ─── Merge block ──────────────────────────────────────────────────────
    b.SetInsertPoint(mergeBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitForStmt — range and collection loops
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Two Shapes ───────────────────────────────────────────────────────────
// A `for` loop is either:
//
//   - Range: `for i in 0..10 { ... }` — the iterable is a `RangeExprAST`.
//     The loop increments an index variable.
//
//   - Collection: `for i, v in arr { ... }` — the iterable is an array or
//     slice. The loop walks the elements.
//
// The two are dispatched on the iterable's kind: a `RangeExprAST` is a
// range loop, anything else is a collection loop.
//
// ─── The Loop Variable Bindings ───────────────────────────────────────────
// `stmt->indexVar` and `stmt->valueVar` are `ParamAST*`. They're `nullptr`
// when the corresponding slot was written as `_` (the discard pattern).
// When non-null, the loop binds the variable to a per-iteration value.
//
// For a range loop, `valueVar` is always `nullptr` (there's no collection
// value). For a collection loop, `indexVar` and `valueVar` are independent
// — either can be discarded.
//
// ─── Where The Loop Variable Allocas Go ───────────────────────────────────
// The index and value allocas go in the entry block, not the loop body.
// Reallocating them per iteration would be a stack leak. The loop writes
// into the same slot every iteration.

void Emitter::emitForStmt(ForStmtAST* stmt) {
    assert(stmt && "emitForStmt() with null statement");
    assert(stmt->iterable && "for loop with no iterable");
    assert(stmt->indexVar && "for loop with no index variable");
    assert(stmt->body && "for loop with no body");

    // ─── Dispatch on iterable kind ────────────────────────────────────────
    bool isRange = stmt->iterable->isa<RangeExprAST>();

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Create the four loop blocks ──────────────────────────────────────
    llvm::BasicBlock* header =
        llvm::BasicBlock::Create(program.llvmContext(), "for.header", fn);
    llvm::BasicBlock* body =
        llvm::BasicBlock::Create(program.llvmContext(), "for.body", fn);
    llvm::BasicBlock* continueBlock =
        llvm::BasicBlock::Create(program.llvmContext(), "for.continue", fn);
    llvm::BasicBlock* exit =
        llvm::BasicBlock::Create(program.llvmContext(), "for.exit", fn);

    // ─── Register the loop for break/continue ─────────────────────────────
    LoopInfo info;
    info.continueTarget = continueBlock;
    info.exit = exit;
    info.scopeDepth = func().scopeDepth();
    func().pushLoop(info);

    // ─── Emit the loop ────────────────────────────────────────────────────
    if (isRange) {
        // ─── Range loop: `for i in lo..hi [..step]` ───────────────────────
        RangeExprAST* range = stmt->iterable->as<RangeExprAST>();

        // Evaluate the bounds.
        Val lo = emit(range->lo);
        Val hi = emit(range->hi);
        if (!lo.isValid() || !hi.isValid()) {
            func().popLoop();
            return;
        }

        // The index variable's type must be an integer. Sema guarantees it.
        llvm::Type* idxTy = program.types().get(stmt->indexVar->type);
        if (!idxTy || !idxTy->isIntegerTy()) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, stmt->indexVar->loc,
                "range loop index must be an integer type");
            func().popLoop();
            return;
        }

        // Coerce the bounds to the index type.
        llvm::Value* loVal = lo.v;
        llvm::Value* hiVal = hi.v;
        if (loVal->getType() != idxTy) {
            loVal = coerceValueToType(loVal, idxTy, b);
        }
        if (hiVal->getType() != idxTy) {
            hiVal = coerceValueToType(hiVal, idxTy, b);
        }

        // Optional step.
        llvm::Value* stepVal = llvm::ConstantInt::get(idxTy, 1);
        if (stmt->step) {
            Val step = emit(stmt->step);
            if (!step.isValid()) {
                func().popLoop();
                return;
            }
            stepVal = step.v;
            if (stepVal->getType() != idxTy) {
                stepVal = coerceValueToType(stepVal, idxTy, b);
            }
        }

        // Allocate the index slot in the entry block.
        llvm::AllocaInst* idxSlot = createEntryAlloca(
            idxTy, program.pool.lookup(stmt->indexVar->name));
        if (!idxSlot) {
            func().popLoop();
            return;
        }
        b.CreateStore(loVal, idxSlot);
        func().storeValue(stmt->indexVar, idxSlot);
        func().markAlive(stmt->indexVar);

        // Branch to the header.
        b.CreateBr(header);
        b.SetInsertPoint(header);

        // Load the current index and check the condition.
        llvm::Value* current = b.CreateLoad(
            idxTy, idxSlot, "for.idx");
        // Compare: for `..` (inclusive) it's `current <= hi`; for `..<`
        // (exclusive) it's `current < hi`. Sema stores `isExclusive` on
        // the range.
        llvm::Value* cond = range->isExclusive
            ? b.CreateICmpSLT(current, hiVal, "for.cond")
            : b.CreateICmpSLE(current, hiVal, "for.cond");
        b.CreateCondBr(cond, body, exit);

        // Body.
        b.SetInsertPoint(body);
        emit(stmt->body);
        if (!b.GetInsertBlock()->getTerminator()) {
            b.CreateBr(continueBlock);
        }

        // Continue block: increment the index, branch back to the header.
        b.SetInsertPoint(continueBlock);
        llvm::Value* current2 = b.CreateLoad(
            idxTy, idxSlot, "for.idx.inc");
        llvm::Value* next = b.CreateAdd(
            current2, stepVal, "for.next");
        b.CreateStore(next, idxSlot);
        b.CreateBr(header);

        // Exit.
        b.SetInsertPoint(exit);
    } else {
        // ─── Collection loop: `for i, v in arr` ───────────────────────────
        //
        // The iterable must be an array or slice. Today, the emitter
        // supports fixed-size arrays and slices. Dynamic arrays are a
        // follow-up once the array lowering stabilizes.
        TypeAST* iterableTy = stmt->iterable->resolvedType;
        ArrayTypeAST* arrayTy = iterableTy && iterableTy->isa<ArrayTypeAST>()
            ? iterableTy->as<ArrayTypeAST>()
            : nullptr;
        if (!arrayTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, stmt->iterable->loc,
                "for loop iterable must be an array or slice");
            func().popLoop();
            return;
        }

        // Evaluate the iterable.
        Val collection = emit(stmt->iterable);
        if (!collection.isValid()) {
            func().popLoop();
            return;
        }
        llvm::Value* collectionVal = collection.v;

        // ─── Get the length and the data pointer ──────────────────────────
        // For a fixed array, the value is an `[N x T]` and the length is
        // the compile-time constant N. The data pointer is a GEP into
        // the first element.
        //
        // For a slice (the `{ ptr, i64, i64 }` shape), the length is
        // field 1 and the data pointer is field 0.
        //
        // The array lowering is in transition, so this code handles the
        // fixed-array case fully and stubs the slice case.
        llvm::Value* lengthVal = nullptr;
        llvm::Value* dataPtr = nullptr;
        llvm::Type* elemTy = program.types().get(arrayTy->element);
        if (!elemTy) {
            func().popLoop();
            return;
        }
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(program.llvmContext());

        if (arrayTy->isFixed()) {
            lengthVal = llvm::ConstantInt::get(i64Ty, arrayTy->size);
            dataPtr = b.CreateConstGEP2_32(
                llvm::ArrayType::get(elemTy, arrayTy->size),
                collectionVal, 0, 0, "for.data");
        } else if (arrayTy->isSlice()) {
            // Slice: { ptr, i64 len, i64 cap }.
            llvm::StructType* sliceTy =
                llvm::dyn_cast<llvm::StructType>(collectionVal->getType());
            if (!sliceTy || sliceTy->getNumElements() != 3) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, stmt->iterable->loc,
                    "slice value has an unexpected shape");
                func().popLoop();
                return;
            }
            dataPtr = b.CreateExtractValue(collectionVal, 0, "for.data");
            lengthVal = b.CreateExtractValue(collectionVal, 1, "for.len");
            if (lengthVal->getType() != i64Ty) {
                lengthVal = coerceValueToType(lengthVal, i64Ty, b);
            }
        } else {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, stmt->iterable->loc,
                "dynamic array iteration is not yet implemented");
            func().popLoop();
            return;
        }

        // ─── Allocate the index slot ──────────────────────────────────────
        llvm::AllocaInst* idxSlot = createEntryAlloca(
            i64Ty, program.pool.lookup(stmt->indexVar->name));
        if (!idxSlot) {
            func().popLoop();
            return;
        }
        b.CreateStore(llvm::ConstantInt::get(i64Ty, 0), idxSlot);
        func().storeValue(stmt->indexVar, idxSlot);
        func().markAlive(stmt->indexVar);

        // ─── Allocate the value slot, if `valueVar` is present ────────────
        llvm::AllocaInst* valSlot = nullptr;
        if (stmt->valueVar) {
            valSlot = createEntryAlloca(
                elemTy, program.pool.lookup(stmt->valueVar->name));
            if (!valSlot) {
                func().popLoop();
                return;
            }
            func().storeValue(stmt->valueVar, valSlot);
            func().markAlive(stmt->valueVar);
        }

        // Branch to the header.
        b.CreateBr(header);
        b.SetInsertPoint(header);

        // Check: index < length.
        llvm::Value* current = b.CreateLoad(
            i64Ty, idxSlot, "for.idx");
        llvm::Value* cond = b.CreateICmpSLT(
            current, lengthVal, "for.cond");
        b.CreateCondBr(cond, body, exit);

        // Body.
        b.SetInsertPoint(body);
        // Load the element.
        llvm::Value* elemPtr = b.CreateGEP(
            elemTy, dataPtr, current, "for.elem.ptr");
        llvm::Value* elem = b.CreateLoad(elemTy, elemPtr, "for.elem");
        if (valSlot) {
            // Copy the element into the value slot. For non-resource
            // elements, this is a bit copy. For resource elements, this
            // is a shallow copy that the loop body must not consume —
            // Sema rejects `for v in arr` when `arr`'s elements are
            // resources (the user must iterate by reference or index).
            b.CreateStore(elem, valSlot);
        }

        emit(stmt->body);
        if (!b.GetInsertBlock()->getTerminator()) {
            b.CreateBr(continueBlock);
        }

        // Continue: increment the index, branch back.
        b.SetInsertPoint(continueBlock);
        llvm::Value* current2 = b.CreateLoad(
            i64Ty, idxSlot, "for.idx.inc");
        llvm::Value* next = b.CreateAdd(
            current2, llvm::ConstantInt::get(i64Ty, 1), "for.next");
        b.CreateStore(next, idxSlot);
        b.CreateBr(header);

        // Exit.
        b.SetInsertPoint(exit);
    }

    func().popLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitWhileStmt — condition-first loop
// ─────────────────────────────────────────────────────────────────────────────
//
// Basic-block shape:
//
//     +--------+  cond false  +------+
//     | header |------------->| exit |
//     +---+----+              +------+
//         | cond true             ▲
//         ▼                       |
//     +------+                    |
//     | body |-- br header ------>+
//     +------+
//
// `continue` jumps to the header (re-evaluates the condition). `break`
// jumps to the exit.

void Emitter::emitWhileStmt(WhileStmtAST* stmt) {
    assert(stmt && "emitWhileStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    llvm::BasicBlock* header =
        llvm::BasicBlock::Create(program.llvmContext(), "while.header", fn);
    llvm::BasicBlock* body =
        llvm::BasicBlock::Create(program.llvmContext(), "while.body", fn);
    llvm::BasicBlock* exit =
        llvm::BasicBlock::Create(program.llvmContext(), "while.exit", fn);

    LoopInfo info;
    info.continueTarget = header;
    info.exit = exit;
    info.scopeDepth = func().scopeDepth();
    func().pushLoop(info);

    b.CreateBr(header);
    b.SetInsertPoint(header);

    // Condition.
    Val cond = emit(stmt->condition);
    if (!cond.isValid()) {
        func().popLoop();
        return;
    }
    llvm::Value* condI1 = emitTruthiness(cond);
    if (!condI1) {
        func().popLoop();
        return;
    }
    b.CreateCondBr(condI1, body, exit);

    // Body.
    b.SetInsertPoint(body);
    if (stmt->body) {
        emit(stmt->body);
    }
    if (!b.GetInsertBlock()->getTerminator()) {
        b.CreateBr(header);
    }

    // Exit.
    b.SetInsertPoint(exit);
    func().popLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitDoWhileStmt — body-first loop
// ─────────────────────────────────────────────────────────────────────────────
//
// Basic-block shape:
//
//     +------+
//     | body |-- br header ----->+
//     +------+                   |
//                                ▼
//                            +--------+  cond true  +------+
//                            | header |----------->| body |
//                            +---+----+            +------+
//                                | cond false
//                                ▼
//                            +------+
//                            | exit |
//                            +------+
//
// `continue` jumps to the header (re-evaluates the condition). `break`
// jumps to the exit. The body executes at least once.

void Emitter::emitDoWhileStmt(DoWhileStmtAST* stmt) {
    assert(stmt && "emitDoWhileStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    llvm::BasicBlock* body =
        llvm::BasicBlock::Create(program.llvmContext(), "dowhile.body", fn);
    llvm::BasicBlock* header =
        llvm::BasicBlock::Create(program.llvmContext(), "dowhile.header", fn);
    llvm::BasicBlock* exit =
        llvm::BasicBlock::Create(program.llvmContext(), "dowhile.exit", fn);

    LoopInfo info;
    // `continue` in a do-while jumps to the condition check, which is
    // the header. Same as while.
    info.continueTarget = header;
    info.exit = exit;
    info.scopeDepth = func().scopeDepth();
    func().pushLoop(info);

    // Enter the body directly.
    b.CreateBr(body);
    b.SetInsertPoint(body);

    if (stmt->body) {
        emit(stmt->body);
    }
    if (!b.GetInsertBlock()->getTerminator()) {
        b.CreateBr(header);
    }

    // Header: evaluate the condition.
    b.SetInsertPoint(header);
    Val cond = emit(stmt->condition);
    if (!cond.isValid()) {
        func().popLoop();
        return;
    }
    llvm::Value* condI1 = emitTruthiness(cond);
    if (!condI1) {
        func().popLoop();
        return;
    }
    b.CreateCondBr(condI1, body, exit);

    // Exit.
    b.SetInsertPoint(exit);
    func().popLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitReturnStmt — return with unwinding
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Order of Operations ──────────────────────────────────────────────────
// 1. Emit the return value (before any cleanup).
// 2. Coerce to the declared return type.
// 3. Acquire a fresh claim (`intoOwned`) so the caller gets an `Owned`.
// 4. Unwind all scopes to depth 0, emitting drops for every alive binding.
// 5. Emit `ret`.
//
// ─── Why Evaluate Before Unwinding ────────────────────────────────────────
// The return expression may reference a local that the unwind is about to
// drop. `return someLocal;` must read `someLocal` **before** the unwind
// releases it. That's what step 1 before step 4 guarantees.
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

        // ─── Step 2: Coerce to the declared return type ───────────────────
        returnVal = coerceTo(returnVal, func().declaredReturnType());
        if (!returnVal.isValid()) return;

        // ─── Step 3: Acquire a fresh claim ────────────────────────────────
        // The caller owns the returned value. If it was `Borrowed`, acquire
        // a fresh claim before unwinding. If it was `Owned`, this is a
        // no-op.
        returnVal = program.ownership().intoOwned(returnVal, b);
    }

    // ─── Step 4: Unwind all scopes to depth 0 ─────────────────────────────
    // This must run after step 1 (the return value is already computed)
    // and before step 5 (the `ret` terminates the block).
    emitUnwindTo(0);

    // ─── Step 5: Emit the `ret` ───────────────────────────────────────────
    if (returnVal.isValid()) {
        llvm::Type* retTy = program.types().get(func().declaredReturnType());
        if (retTy && returnVal.v->getType() != retTy) {
            returnVal.v = coerceValueToType(returnVal.v, retTy, b);
        }
        b.CreateRet(returnVal.v);
    } else {
        b.CreateRetVoid();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitBreakStmt — exit the innermost loop
// ─────────────────────────────────────────────────────────────────────────────
//
// `break` unwinds every scope pushed inside the loop back to (but not
// including) the loop's own scope, then branches to the loop's exit block.
// The loop's own scope (and anything outside the loop) is not touched —
// the loop's exit block sits outside it.

void Emitter::emitBreakStmt(BreakStmtAST* stmt) {
    assert(stmt && "emitBreakStmt() with null statement");

    LoopInfo* loop = func().currentLoop();
    assert(loop && "break outside any loop — Sema should have rejected");

    emitUnwindTo(loop->scopeDepth);
    program.builder().CreateBr(loop->exit);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitContinueStmt — skip to the next iteration
// ─────────────────────────────────────────────────────────────────────────────
//
// Same unwinding as `break`, but branches to `continueTarget` instead of
// `exit`. For a `for`/`while`, `continueTarget` is the increment or
// condition block; for a `do-while`, it's the condition block.

void Emitter::emitContinueStmt(ContinueStmtAST* stmt) {
    assert(stmt && "emitContinueStmt() with null statement");

    LoopInfo* loop = func().currentLoop();
    assert(loop && "continue outside any loop — Sema should have rejected");

    emitUnwindTo(loop->scopeDepth);
    program.builder().CreateBr(loop->continueTarget);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitExprStmt — a discarded temporary
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Discarded-Temporary Rule ─────────────────────────────────────────
// `f();` produces a value whose claim, if `Owned`, has no consumer. The
// statement must drop it — otherwise the temp claim leaks.
//
// A `Borrowed` result is not dropped: the source binding still owns the
// claim, and dropping the alias would double-free.

void Emitter::emitExprStmt(ExprStmtAST* stmt) {
    assert(stmt && "emitExprStmt() with null statement");
    if (!stmt->expr) return;

    Val result = emit(stmt->expr);

    if (result.isValid() && result.own == Own::Owned) {
        program.ownership().drop(result.ty, result.v, program.builder());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitDeclStmt — a local declaration inside a block
// ─────────────────────────────────────────────────────────────────────────────
//
// A declaration statement wraps any `DeclAST*`. The most common case is a
// local `let`/`const` (`VarDeclAST`), which `emitVarDecl` handles. A local
// function declaration (`FuncDeclAST`) goes through `emitFuncDecl`. Local
// structs and enums are also allowed, though rare.
//
// The dispatch is already in `emit(DeclAST*)`, so this just forwards.

void Emitter::emitDeclStmt(DeclStmtAST* stmt) {
    assert(stmt && "emitDeclStmt() with null statement");
    if (!stmt->decl) return;
    emit(stmt->decl);
}

} // namespace codegen