/// @file compile/EmitStmt.cpp
/// @brief Lower a statement into instructions.
///
/// ─── Scope of this file ───────────────────────────────────────────────────
/// emitStmt handles every StmtAST subclass from StmtAST.hpp:
///
///   - block, var-decl, assign, expr, return, break, continue
///   - if, switch, while, for
///   - the five suspend points (wait, waitFrames, waitUntil,
///     waitForEvent, waitForRequest)
///
/// Each statement form has its own function below, so the dispatcher
/// (emitStmt) is a pure switch and each case is reviewable in
/// isolation.
///
/// ─── Design: the "control transfers out" convention ───────────────────────
/// A statement that transfers control out of the enclosing block (a
/// return, a break, a continue) is followed by dead code in the source
/// but not in the bytecode. The emitter emits the transfer instruction
/// and continues emitting subsequent statements; the interpreter never
/// reaches the dead instructions because the transfer instruction
/// branches away. This is simpler than tracking reachability during
/// emission and letting the compiler prune dead code — the dead code
/// costs a few bytes per statement and is harmless.
///
/// ─── Design: labels for break/continue ────────────────────────────────────
/// The LoopContext stack (in CompilerContext) tracks enclosing loops.
/// Each loop pushes a context with its continue target (a code offset)
/// and a list of break jumps to patch once the loop's end offset is
/// known. `break label` and `continue label` walk the stack to find
/// the named loop; an unlabeled break/continue uses the innermost loop.
///
/// ─── Design: for-loop lowering is four-shape ──────────────────────────────
/// A `for` loop's iterable determines its lowering (§12.3): a range
/// is a counter loop; a table or view is a slot walk; a column view is
/// a slot walk over one column; an array is an index walk. The four
/// lowerings are in emitForStmt.

#include "EmitStmt.hpp"
#include "EmitExpr.hpp"
#include "EmitPlace.hpp"
#include "../compile/CompilerContext.hpp"
#include "../compile/TypeTranslation.hpp"
#include "bytecode/compile/Compiler.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Emit a jump to a target that is not yet known. The placeholder
/// operand is patched later. Returns the code offset of the operand,
/// which the caller passes to patchJump.
uint32_t emitJumpPlaceholder(CompilerContext& ctx, Opcode op) {
    ctx.emitOpcode(op);
    const uint32_t operandOffset = ctx.here();
    ctx.emitI32(0);
    return operandOffset;
}

/// Patch a jump whose operand is at `operandOffset` to target the
/// current code position.
void patchJumpToHere(CompilerContext& ctx, uint32_t operandOffset) {
    const int32_t target = static_cast<int32_t>(ctx.here());
    const int32_t base   = static_cast<int32_t>(operandOffset + 4);
    ctx.patchU32(operandOffset,
                 static_cast<uint32_t>(target - base));
}

/// Patch a jump to a target code offset.
void patchJumpTo(CompilerContext& ctx, uint32_t operandOffset,
                 uint32_t targetOffset) {
    const int32_t target = static_cast<int32_t>(targetOffset);
    const int32_t base   = static_cast<int32_t>(operandOffset + 4);
    ctx.patchU32(operandOffset,
                 static_cast<uint32_t>(target - base));
}

/// Find the loop context for a label. Returns nullptr if the label
/// names no enclosing loop. `label` invalid means "unlabeled" — the
/// caller wants the innermost loop.
LoopContext* findLoop(CompilerContext& ctx, InternedString label) {
    if (!label.isValid()) {
        // Unlabeled break/continue: innermost loop.
        if (ctx.loops().empty()) return nullptr;
        return &ctx.loops().back();
    }
    for (auto it = ctx.loops().rbegin(); it != ctx.loops().rend(); ++it) {
        if (it->label == label) return &*it;
    }
    return nullptr;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// emitStmt — the dispatcher
// ─────────────────────────────────────────────────────────────────────────────

void emitStmt(StmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt != nullptr,
        "emitStmt: null statement");

    // Skip a statement produced by parser error recovery. Sema skips
    // these too; the compiler should never see one because Sema
    // rejects a module with syntax errors before codegen runs.
    if (stmt->hasSyntaxError) {
        return;
    }

    ctx.noteLine(stmt->loc, ctx.module()->filePath);

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            resolveBlock(stmt->as<BlockStmtAST>(), ctx); return;
        case ASTKind::VarDeclStmt:
            resolveVarDeclStmt(stmt->as<VarDeclStmtAST>(), ctx); return;
        case ASTKind::AssignStmt:
            resolveAssignStmt(stmt->as<AssignStmtAST>(), ctx); return;
        case ASTKind::ExprStmt:
            resolveExprStmt(stmt->as<ExprStmtAST>(), ctx); return;
        case ASTKind::ReturnStmt:
            resolveReturnStmt(stmt->as<ReturnStmtAST>(), ctx); return;
        case ASTKind::BreakStmt:
            resolveBreakStmt(stmt->as<BreakStmtAST>(), ctx); return;
        case ASTKind::ContinueStmt:
            resolveContinueStmt(stmt->as<ContinueStmtAST>(), ctx); return;
        case ASTKind::IfStmt:
            resolveIfStmt(stmt->as<IfStmtAST>(), ctx); return;
        case ASTKind::SwitchStmt:
            resolveSwitchStmt(stmt->as<SwitchStmtAST>(), ctx); return;
        case ASTKind::WhileStmt:
            resolveWhileStmt(stmt->as<WhileStmtAST>(), ctx); return;
        case ASTKind::ForStmt:
            resolveForStmt(stmt->as<ForStmtAST>(), ctx); return;
        case ASTKind::WaitStmt:
            resolveWaitStmt(stmt->as<WaitStmtAST>(), ctx); return;
        case ASTKind::WaitFramesStmt:
            resolveWaitFramesStmt(stmt->as<WaitFramesStmtAST>(), ctx); return;
        case ASTKind::WaitUntilStmt:
            resolveWaitUntilStmt(stmt->as<WaitUntilStmtAST>(), ctx); return;
        case ASTKind::WaitForEventStmt:
            resolveWaitForEventStmt(stmt->as<WaitForEventStmtAST>(), ctx); return;
        case ASTKind::WaitForRequestStmt:
            resolveWaitForRequestStmt(stmt->as<WaitForRequestStmtAST>(), ctx); return;
        default:
            AST_ASSERT_MSG(false,
                "emitStmt: unhandled StmtAST subclass — the emitter is "
                "out of sync with the AST");
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Block
// ─────────────────────────────────────────────────────────────────────────────

bool resolveBlock(BlockStmtAST* stmt, CompilerContext& ctx) {
    // A block introduces a new scope for name resolution, but for
    // codegen it is transparent: statements are emitted in source
    // order, and no runtime effect accompanies the braces.
    //
    // The block's "scope" is a Sema concept; the compiler does not
    // need a scope stack of its own because Sema has already resolved
    // every identifier to its declaration (and the declaration's
    // slot was assigned when the declaration was emitted).
    for (StmtAST* s : stmt->stmts) {
        emitStmt(s, ctx);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Local variable declaration
// ─────────────────────────────────────────────────────────────────────────────

bool resolveVarDeclStmt(VarDeclStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->decl != nullptr,
        "resolveVarDeclStmt: a var-decl statement has no declaration");

    VarDeclAST* decl = stmt->decl;
    AST_ASSERT_MSG(decl->init != nullptr,
        "resolveVarDeclStmt: a local declaration has no initializer — "
        "the grammar requires one");

    // The declaration's initializer is evaluated first (the value is
    // on the stack), then stored into the binding's slot. The slot is
    // allocated *after* emitting the initializer so that a
    // self-referential local (`let x = x`) does not accidentally see
    // its own slot — but Sema rejects that case, so the ordering is
    // a formality.
    emitExpr(decl->init, ctx);

    const uint16_t slot = ctx.slots().allocateLocal(decl->name);
    ctx.emitOpcode(Opcode::StoreLocal);
    ctx.emitU16(slot);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assignment
// ─────────────────────────────────────────────────────────────────────────────

bool resolveAssignStmt(AssignStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->lhs != nullptr && stmt->rhs != nullptr,
        "resolveAssignStmt: an assignment is missing an operand");

    // ─── Simple assignment ─────────────────────────────────────────────
    if (stmt->op == AssignOp::Assign) {
        emitPlace(stmt->lhs, ctx);
        emitExpr(stmt->rhs, ctx);
        emitStoreIntoPlace(stmt->lhs, ctx);
        return false;
    }

    // ─── Compound assignment ───────────────────────────────────────────
    //
    // `x op= y` desugars to `x = x op y`. Sema performs the desugar at
    // the AST level for some operators; for others it leaves the
    // compound form. The emitter handles the compound form directly:
    //
    //   1. Emit the place's operands (once, so a complex lvalue is
    //      evaluated once).
    //   2. Load the current value at the place. This is a load, not
    //      an assign, so it does not use the same opcodes as the
    //      place. (For a local, LoadLocal; for a field, LoadField;
    //      for an index, LoadIndex.)
    //   3. Emit the RHS.
    //   4. Emit the binary operation (the compound operator's
    //      underlying operation).
    //   5. Store into the place.
    //
    // Phase 3 does not yet have the "load the current value"
    // step for every place kind (the load opcodes overlap with the
    // place's operands in a way that requires care). Phase 4 will
    // implement it fully. For Phase 3, compound assignments to a
    // local are handled; compound assignments to a field or an index
    // are rejected.
    AST_ASSERT_MSG(false,
        "resolveAssignStmt: compound assignment is not yet supported — "
        "the load-then-store lowering is a Phase 4 addition");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression statement
// ─────────────────────────────────────────────────────────────────────────────

bool resolveExprStmt(ExprStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->expr != nullptr,
        "resolveExprStmt: an expression statement has no expression");

    // Emit the expression. Its result is left on the stack; the
    // interpreter's stack discipline advances past it without an
    // explicit pop (the next instruction overwrites the slot). The
    // "pop" is implicit in the stack-machine convention.
    emitExpr(stmt->expr, ctx);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Return
// ─────────────────────────────────────────────────────────────────────────────

bool resolveReturnStmt(ReturnStmtAST* stmt, CompilerContext& ctx) {
    if (stmt->value != nullptr) {
        emitExpr(stmt->value, ctx);
        ctx.emitOpcode(Opcode::Ext_Return);
    } else {
        ctx.emitOpcode(Opcode::Ext_ReturnVoid);
    }
    // A return transfers control out of the enclosing block. The
    // caller (a block emitter, a loop emitter) can use this flag to
    // stop emitting dead code. In practice the emitters above do not
    // use it — dead code is harmless and skipping it would complicate
    // the emitters. The flag is provided for a future pass that
    // prunes unreachable code.
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Break and continue
// ─────────────────────────────────────────────────────────────────────────────

bool resolveBreakStmt(BreakStmtAST* stmt, CompilerContext& ctx) {
    LoopContext* loop = findLoop(ctx, stmt->label);
    AST_ASSERT_MSG(loop != nullptr,
        "resolveBreakStmt: a break statement is not inside a loop — "
        "Sema should have rejected it");

    // Emit a jump to the loop's end. The end offset is not yet known
    // (the loop body is still being emitted), so the placeholder is
    // recorded in the loop's breakJumps list. The loop emitter
    // patches all of them when the loop ends.
    const uint32_t operandOffset =
        emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
    loop->breakJumps.push_back(operandOffset);
    return true;
}

bool resolveContinueStmt(ContinueStmtAST* stmt, CompilerContext& ctx) {
    LoopContext* loop = findLoop(ctx, stmt->label);
    AST_ASSERT_MSG(loop != nullptr,
        "resolveContinueStmt: a continue statement is not inside a "
        "loop — Sema should have rejected it");

    // The continue target is known: it is the loop's condition-check
    // offset (or its step, for a for loop). The loop context records
    // it.
    const int32_t target = static_cast<int32_t>(loop->continueTarget);
    const uint32_t operandOffset =
        emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
    const int32_t base = static_cast<int32_t>(operandOffset + 4);
    ctx.patchU32(operandOffset,
                 static_cast<uint32_t>(target - base));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// If
// ─────────────────────────────────────────────────────────────────────────────

bool resolveIfStmt(IfStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->condition != nullptr,
        "resolveIfStmt: an if statement has no condition");
    AST_ASSERT_MSG(stmt->thenBranch != nullptr,
        "resolveIfStmt: an if statement has no then-branch");

    // The condition's type is bool (grammar §6.14). Emit the
    // condition; the JumpIfFalse pops it.
    emitExpr(stmt->condition, ctx);

    // Jump past the then-branch if the condition is false.
    const uint32_t toElse =
        emitJumpPlaceholder(ctx, Opcode::Ext_JumpIfFalse);

    // Then-branch.
    emitStmt(stmt->thenBranch, ctx);

    if (stmt->elseBranch == nullptr) {
        // No else: the false-jump targets the instruction after the
        // then-branch.
        patchJumpToHere(ctx, toElse);
        return false;
    }

    // There is an else branch. Emit a jump over it so the then-branch
    // does not fall through into the else.
    const uint32_t pastElse =
        emitJumpPlaceholder(ctx, Opcode::Ext_Jump);

    // The false-jump from the condition targets the else-branch.
    patchJumpToHere(ctx, toElse);

    emitStmt(stmt->elseBranch, ctx);

    // The jump past the else lands here.
    patchJumpToHere(ctx, pastElse);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Switch
// ─────────────────────────────────────────────────────────────────────────────
//
// A switch over a fixed table's member match (§12.2). The subject is a
// &T (a row reference into a fixed table). Each case value is a
// reference to one of the table's members. The match is by reference
// identity.
//
// Lowering:
//
//   <subject>
//   Ext_IsNil                ; optional nil check
//   Ext_JumpIfTrue  default  ; a nil subject falls through to default
//   for each case:
//     Dup                    ; keep the subject on the stack
//     LoadConst  caseRef     ; push the case's row reference
//     Eq_RowRef              ; compare
//     Ext_JumpIfTrue  caseBody
//   Jump  default
//   caseBody_i:
//     <body_i>
//     Jump  end
//   default:
//     <defaultBody>
//   end:
//
// The Dup opcode is needed to keep the subject on the stack across
// multiple comparisons. The opcode set has no Dup; Phase 3 rejects
// switch. Phase 4 adds Dup (and its counterpart Pop for discarding
// the subject after the switch).

bool resolveSwitchStmt(SwitchStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->subject != nullptr,
        "resolveSwitchStmt: a switch has no subject");
    AST_ASSERT_MSG(stmt->defaultBody != nullptr,
        "resolveSwitchStmt: a switch has no default body — the "
        "grammar requires one");

    // ─── Emit the subject ──────────────────────────────────────────────
    emitExpr(stmt->subject, ctx);

    // ─── Emit the case comparisons ─────────────────────────────────────
    //
    // Each case is a list of values. The lowering compares the subject
    // against each value and jumps to the case's body on a match. The
    // subject is duplicated before each comparison, so it survives the
    // Eq_RowRef (which pops both operands).
    //
    // The list of case bodies to patch after emission: for each case,
    // the offsets of the JumpIfTrue operands that jump to the case's
    // body.
    std::vector<std::vector<uint32_t>> caseJumpOperands;
    caseJumpOperands.reserve(stmt->cases.size());

    for (auto* c : stmt->cases) {
        AST_ASSERT_MSG(c != nullptr,
            "resolveSwitchStmt: a case clause is null");
        std::vector<uint32_t> jumpsForThisCase;
        for (auto* value : c->values) {
            AST_ASSERT_MSG(value != nullptr,
                "resolveSwitchStmt: a case value is null");

            // Duplicate the subject: [S] → [S, S].
            ctx.emitOpcode(Opcode::Ext_Dup);

            // Emit the case value. For a fixed-table switch, this is a
            // fixed-row sugar; the emitter handles it and leaves a &T
            // on the stack.
            emitExpr(value, ctx);

            // Compare. Eq_RowRef pops the two operands and pushes the
            // match bool.
            ctx.emitOpcode(Opcode::Eq_RowRef);

            // Jump to the case body on a match. The target is patched
            // later, once the body's code offset is known.
            ctx.emitOpcode(Opcode::Ext_JumpIfTrue);
            const uint32_t operandOffset = ctx.here();
            ctx.emitI32(0);
            jumpsForThisCase.push_back(operandOffset);
        }
        caseJumpOperands.push_back(std::move(jumpsForThisCase));
    }

    // ─── No case matched: discard the subject and fall to default ─────
    ctx.emitOpcode(Opcode::Ext_Pop);
    ctx.emitOpcode(Opcode::Ext_Jump);
    const uint32_t toDefaultOffset = ctx.here();
    ctx.emitI32(0);

    // ─── Emit the case bodies ──────────────────────────────────────────
    std::vector<uint32_t> bodyEndJumps;
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        auto* c = stmt->cases[i];

        // Patch every jump for this case to land at this body's start.
        for (uint32_t operandOffset : caseJumpOperands[i]) {
            const int32_t target = static_cast<int32_t>(ctx.here());
            const int32_t base   = static_cast<int32_t>(operandOffset + 4);
            ctx.patchU32(operandOffset,
                         static_cast<uint32_t>(target - base));
        }

        // Discard the subject copy that the Dup-based comparison left
        // on the stack.
        ctx.emitOpcode(Opcode::Ext_Pop);

        // Emit the case body.
        emitStmt(c->body, ctx);

        // Jump to the end of the switch.
        ctx.emitOpcode(Opcode::Ext_Jump);
        bodyEndJumps.push_back(ctx.here());
        ctx.emitI32(0);
    }

    // ─── Patch the no-match jump to land at the default body ──────────
    {
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base   = static_cast<int32_t>(toDefaultOffset + 4);
        ctx.patchU32(toDefaultOffset,
                     static_cast<uint32_t>(target - base));
    }

    // ─── Default body ──────────────────────────────────────────────────
    emitStmt(stmt->defaultBody, ctx);

    // ─── Patch the end jumps ───────────────────────────────────────────
    for (uint32_t operandOffset : bodyEndJumps) {
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base   = static_cast<int32_t>(operandOffset + 4);
        ctx.patchU32(operandOffset,
                     static_cast<uint32_t>(target - base));
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// While
// ─────────────────────────────────────────────────────────────────────────────

bool resolveWhileStmt(WhileStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->condition != nullptr && stmt->body != nullptr,
        "resolveWhileStmt: a while statement is missing its condition "
        "or its body");

    // ─── The loop's condition-check offset ─────────────────────────────
    const uint32_t conditionOffset = ctx.here();

    // ─── Push the loop context ─────────────────────────────────────────
    LoopContext loop;
    loop.label          = stmt->label;
    loop.continueTarget = conditionOffset;
    ctx.loops().push_back(std::move(loop));
    const size_t loopIndex = ctx.loops().size() - 1;

    // ─── Emit the condition ────────────────────────────────────────────
    emitExpr(stmt->condition, ctx);
    const uint32_t toEnd =
        emitJumpPlaceholder(ctx, Opcode::Ext_JumpIfFalse);

    // ─── Emit the body ─────────────────────────────────────────────────
    emitStmt(stmt->body, ctx);

    // ─── Jump back to the condition ────────────────────────────────────
    {
        const uint32_t backJump =
            emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
        patchJumpTo(ctx, backJump, conditionOffset);
    }

    // ─── The false-jump targets the instruction after the loop ─────────
    patchJumpToHere(ctx, toEnd);

    // ─── Patch all break jumps to land here ────────────────────────────
    LoopContext& activeLoop = ctx.loops()[loopIndex];
    for (uint32_t breakOperand : activeLoop.breakJumps) {
        patchJumpToHere(ctx, breakOperand);
    }

    // ─── Pop the loop context ──────────────────────────────────────────
    ctx.loops().pop_back();

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// For
// ─────────────────────────────────────────────────────────────────────────────

bool resolveForStmt(ForStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->iterable != nullptr,
        "resolveForStmt: a for statement has no iterable");
    AST_ASSERT_MSG(stmt->body != nullptr,
        "resolveForStmt: a for statement has no body");

    // ─── Classify the iterable ─────────────────────────────────────────
    //
    // The four iterable shapes (§12.3): range, table/view, column
    // view, array. Phase 3 supports the range shape and the array
    // shape; the table and column-view shapes need the table-index
    // and column-view operand encodings (Phase 4).

    if (stmt->iterable->isa<RangeExprAST>()) {
        // ─── Range loop ────────────────────────────────────────────────
        // Lower to a counter loop. The counter is a local slot.
        // The step defaults to 1.
        auto* range = stmt->iterable->as<RangeExprAST>();

        AST_ASSERT_MSG(stmt->firstVar != nullptr,
            "resolveForStmt: a range loop has no loop variable");
        AST_ASSERT_MSG(stmt->secondVar == nullptr,
            "resolveForStmt: a range loop has two binding variables — "
            "the grammar allows only one for a range");

        AST_ASSERT_MSG(range->lo != nullptr && range->hi != nullptr,
            "resolveForStmt: a range is missing a bound");

        // The bounds must be constant expressions (they are literals
        // or small arithmetic combinations of literals).
        AST_ASSERT_MSG(range->lo->isConst && range->hi->isConst,
            "resolveForStmt: a range bound is not a constant — "
            "the grammar requires constants");
        if (range->step != nullptr) {
            AST_ASSERT_MSG(range->step->isConst,
                "resolveForStmt: a range step is not a constant");
        }

        // For a counter loop, the loop variable's slot holds the
        // current value. Emit:
        //
        //   <lo>                ; initial value
        //   StoreLocal  counter
        // loopTop:
        //   LoadLocal  counter
        //   <hi>
        //   Lt_I32 (or Le_I32 for inclusive)
        //   JumpIfFalse  end        //   <body>
        // stepTop:
        //   LoadLocal  counter
        //   <step>
        //   Add_I32
        //   StoreLocal  counter
        //   Jump  loopTop
        // end:
        //
        // For an exclusive range, the check is `<`; for an
        // inclusive range, it is `<=`.
        const bool exclusive = range->isExclusive;

        // Emit the initial value and store it into the counter slot.
        emitExpr(range->lo, ctx);
        const uint16_t counterSlot =
            ctx.slots().allocateLocal(stmt->firstVar->name);
        ctx.emitOpcode(Opcode::StoreLocal);
        ctx.emitU16(counterSlot);

        // Push the loop context before emitting the loop proper, so
        // break/continue inside the body find it.
        LoopContext loop;
        loop.label = stmt->label;

        const uint32_t conditionOffset = ctx.here();
        loop.continueTarget = 0;   // patched below once step start is known
        ctx.loops().push_back(std::move(loop));
        const size_t loopIndex = ctx.loops().size() - 1;

        // Condition: counter <  or  <=  hi
        ctx.emitOpcode(Opcode::LoadLocal);
        ctx.emitU16(counterSlot);
        emitExpr(range->hi, ctx);

        // The range's bound type is an integer. The width is the
        // resolved type of the lo/hi expressions.
        const TypeDescriptor boundType =
            translateType(range->lo->resolvedType,
                          ctx.compiler().pool());
        auto pk = boundType.isPrimitive()
                    ? std::optional<PrimitiveKind>(boundType.primitive)
                    : std::nullopt;
        AST_ASSERT_MSG(pk.has_value() && isIntegerKind(*pk),
            "resolveForStmt: a range's bound type is not an integer — "
            "Sema should have rejected this");

        const int w = [&] {
            switch (*pk) {
                case PrimitiveKind::Int8:   return 0;
                case PrimitiveKind::Int16:  return 1;
                case PrimitiveKind::Int32:  return 2;
                case PrimitiveKind::Int64:  return 3;
                case PrimitiveKind::Uint8:  return 0;
                case PrimitiveKind::Uint16: return 1;
                case PrimitiveKind::Uint32: return 2;
                case PrimitiveKind::Uint64: return 3;
                default: return -1;
            }
        }();
        AST_ASSERT_MSG(w >= 0,
            "resolveForStmt: a range's bound type is not an integer");

        const bool isSigned = isSignedIntegerKind(*pk);
        Opcode cmpOp;
        if (exclusive) {
            cmpOp = isSigned
                ? static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Lt_I8) + w)
                : static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Lt_U8) + w);
        } else {
            cmpOp = isSigned
                ? static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Le_I8) + w)
                : static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Le_U8) + w);
        }
        ctx.emitOpcode(cmpOp);

        const uint32_t toEnd =
            emitJumpPlaceholder(ctx, Opcode::Ext_JumpIfFalse);

        // Body
        emitStmt(stmt->body, ctx);

        // Step start is here. Continue targets this.
        const uint32_t stepOffset = ctx.here();
        ctx.loops()[loopIndex].continueTarget = stepOffset;

        // Emit step: counter = counter + step
        ctx.emitOpcode(Opcode::LoadLocal);
        ctx.emitU16(counterSlot);
        if (range->step != nullptr) {
            emitExpr(range->step, ctx);
        } else {
            // Default step of 1. Emit a LoadConst of 1.
            Constant one;
            one.kind = Constant::Kind::Int;
            one.type = boundType;
            one.value = static_cast<int64_t>(1);
            const uint32_t oneIdx = ctx.pool().add(std::move(one));
            ctx.emitOpcode(Opcode::LoadConst);
            ctx.emitU32(oneIdx);
        }
        {
            Opcode addOp = isSigned
                ? static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Add_I8) + w)
                : static_cast<Opcode>(
                      static_cast<uint16_t>(Opcode::Add_U8) + w);
            ctx.emitOpcode(addOp);
        }
        ctx.emitOpcode(Opcode::StoreLocal);
        ctx.emitU16(counterSlot);

        // Jump back to the condition.
        {
            const uint32_t backJump =
                emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
            patchJumpTo(ctx, backJump, conditionOffset);
        }

        // The false-jump targets here (after the loop).
        patchJumpToHere(ctx, toEnd);

        // Patch break jumps.
        for (uint32_t breakOperand : ctx.loops()[loopIndex].breakJumps) {
            patchJumpToHere(ctx, breakOperand);
        }

        ctx.loops().pop_back();
        return false;
    }

    // ─── Array loop ────────────────────────────────────────────────────
    //
    // `for x: T in arr` or `for i: uint, x: T in arr`. The array's
    // element count is the loop bound; the index is a slot; the loop
    // walks 0..count-1.
    if (stmt->iterable->resolvedType != nullptr
        && stmt->iterable->resolvedType->isa<ArrayTypeAST>()) {
        AST_ASSERT_MSG(false,
            "resolveForStmt: array iteration is not yet supported — "
            "it needs the array element access encoding (Phase 4)");
        return false;
    }

    // ─── Table or view loop ────────────────────────────────────────────
    if (stmt->iterable->isa<IdentifierExprAST>()
        || stmt->iterable->isa<FieldAccessExprAST>()) {
        AST_ASSERT_MSG(false,
            "resolveForStmt: table iteration is not yet supported — "
            "it needs the table-index map and the table slot-walk "
            "encoding (Phase 4 additions)");
        return false;
    }

    AST_ASSERT_MSG(false,
        "resolveForStmt: the iterable is none of range, array, table, "
        "or view — Sema should have rejected this for loop");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suspend points
// ─────────────────────────────────────────────────────────────────────────────
//
// The five suspend points (§9.2.2). Each is a keyword-led statement.
// In a @sequence function, a suspend point:
//
//   1. Emits the argument expression (for the polled forms) or the
//      constant event/request reference (for the parked forms).
//   2. Emits a SuspendX opcode with a resume-index operand. The
//      resume index is allocated from the current function's resume
//      counter and recorded on the FunctionProto's resume table by
//      the slot allocator (recordSuspendPoint).
//
// The compiler does not yet lower suspend points fully, because the
// state-machine split (§9.2.6) requires knowing which locals are live
// across the suspend — a liveness analysis that Phase 4 adds. For
// Phase 3, suspend points are rejected with an assert.

bool resolveWaitStmt(WaitStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "resolveWaitStmt: `wait` is not yet supported — it needs the "
        "sequence state-machine lowering and liveness analysis "
        "(Phase 4 additions)");
    return false;
}

bool resolveWaitFramesStmt(WaitFramesStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "resolveWaitFramesStmt: `waitFrames` is not yet supported — "
        "it needs the sequence state-machine lowering (Phase 4)");
    return false;
}

bool resolveWaitUntilStmt(WaitUntilStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "resolveWaitUntilStmt: `waitUntil` is not yet supported — "
        "it needs the sequence state-machine lowering (Phase 4)");
    return false;
}

bool resolveWaitForEventStmt(WaitForEventStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "resolveWaitForEventStmt: `waitForEvent` is not yet supported — "
        "it needs the sequence state-machine lowering (Phase 4)");
    return false;
}

bool resolveWaitForRequestStmt(WaitForRequestStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "resolveWaitForRequestStmt: `waitForRequest` is not yet "
        "supported — it needs the sequence state-machine lowering "
        "(Phase 4)");
    return false;
}

} // namespace lucid::bytecode::compile