/// @file emit/EmitStmt.cpp
/// @brief Lower a statement into instructions.

#include "EmitStmt.hpp"
#include "EmitExpr.hpp"
#include "EmitPlace.hpp"

#include "bytecode/compile/Compiler.hpp"
#include "bytecode/compile/CompilerContext.hpp"
#include "bytecode/compile/TypeTranslation.hpp"
#include "bytecode/memory/DropSchedule.hpp"
#include "bytecode/memory/EmitDrop.hpp"

#include "contract/ResourcePlan.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

using memory::DropSchedule;

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

uint32_t emitJumpPlaceholder(CompilerContext& ctx, Opcode op) {
    ctx.emitOpcode(op);
    const uint32_t operandOffset = ctx.here();
    ctx.emitI32(0);
    return operandOffset;
}

void patchJumpToHere(CompilerContext& ctx, uint32_t operandOffset) {
    const int32_t target = static_cast<int32_t>(ctx.here());
    const int32_t base   = static_cast<int32_t>(operandOffset + 4);
    ctx.patchU32(operandOffset,
                 static_cast<uint32_t>(target - base));
}

void patchJumpTo(CompilerContext& ctx, uint32_t operandOffset,
                 uint32_t targetOffset) {
    const int32_t target = static_cast<int32_t>(targetOffset);
    const int32_t base   = static_cast<int32_t>(operandOffset + 4);
    ctx.patchU32(operandOffset,
                 static_cast<uint32_t>(target - base));
}

LoopContext* findLoop(CompilerContext& ctx, InternedString label) {
    if (!label.isValid()) {
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

bool emitStmt(StmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt != nullptr, "emitStmt: null statement");
    if (stmt->hasSyntaxError) return false;

    ctx.noteLine(stmt->loc, ctx.module()->filePath);

    switch (stmt->kind) {
        case ASTKind::BlockStmt:            return emitBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::VarDeclStmt:          return emitVarDeclStmt(stmt->as<VarDeclStmtAST>(), ctx);
        case ASTKind::AssignStmt:           return emitAssignStmt(stmt->as<AssignStmtAST>(), ctx);
        case ASTKind::ExprStmt:             return emitExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::ReturnStmt:           return emitReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:            return emitBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:         return emitContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::IfStmt:               return emitIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:           return emitSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::WhileStmt:            return emitWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::ForStmt:              return emitForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WaitStmt:             return emitWaitStmt(stmt->as<WaitStmtAST>(), ctx);
        case ASTKind::WaitFramesStmt:       return emitWaitFramesStmt(stmt->as<WaitFramesStmtAST>(), ctx);
        case ASTKind::WaitUntilStmt:        return emitWaitUntilStmt(stmt->as<WaitUntilStmtAST>(), ctx);
        case ASTKind::WaitForEventStmt:     return emitWaitForEventStmt(stmt->as<WaitForEventStmtAST>(), ctx);
        case ASTKind::WaitForRequestStmt:   return emitWaitForRequestStmt(stmt->as<WaitForRequestStmtAST>(), ctx);
        default:
            AST_ASSERT_MSG(false,
                "emitStmt: unhandled StmtAST subclass — the emitter is "
                "out of sync with the AST");
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Block
// ─────────────────────────────────────────────────────────────────────────────

bool emitBlock(BlockStmtAST* stmt, CompilerContext& ctx) {
    // A block is a scope: it owns the resource-typed locals it declares.
    // Push a scope on entry; on the fall-through exit, drop them.
    ctx.slots().pushScope();

    // Emit every statement. Once any statement transfers control
    // out (a return, break, or continue), the block as a whole
    // transfers: every later statement is dead code and does not
    // change that answer. We keep emitting the dead code because
    // Phase 3 does not do dead-code elimination, and the emitted
    // opcodes still contribute to the line table and the value-stack
    // accounting.
    bool transfers = false;
    for (StmtAST* s : stmt->stmts) {
        const bool stmtTransfers = emitStmt(s, ctx);
        if (stmtTransfers) transfers = true;
    }

    // Pop the scope and emit drops for its resource-typed locals.
    //
    // If a statement transferred control out, that statement's own
    // drop emission already handled this scope's slots, and these
    // drops are unreachable. The bytecode emits them anyway: Sema
    // is responsible for dead-code elimination, and the bytecode
    // emitter lowers the full body regardless of reachability. The
    // interpreter never executes these drops, so they have no
    // runtime effect.
    const ScopeRecord scope = ctx.slots().popScope();
    DropSchedule::emitScopeDrops(ctx, scope);

    return transfers;
}

// ─────────────────────────────────────────────────────────────────────────────
// Local variable declaration
// ─────────────────────────────────────────────────────────────────────────────

bool emitVarDeclStmt(VarDeclStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->decl != nullptr,
        "emitVarDeclStmt: a var-decl statement has no declaration");

    VarDeclAST* decl = stmt->decl;
    AST_ASSERT_MSG(decl->init != nullptr,
        "emitVarDeclStmt: a local declaration has no initializer — "
        "the grammar requires one");
    AST_ASSERT_MSG(decl->type != nullptr,
        "emitVarDeclStmt: a local declaration has no resolved type — "
        "Sema should have resolved it");

    // Evaluate the initializer. The value ends up on top of the stack,
    // with its ownership entry pushed by emitExpr's auto-bookkeeping
    // (BitCopy, upgraded to Owned if the value owns a resource).
    emitExpr(decl->init, ctx);

    // Translate the declared type and record it on the slot.
    const TypeDescriptor type =
        translateType(decl->type, ctx.compiler().pool());

    const uint16_t slot = ctx.slots().allocateLocal(decl->name, type);

    // Store the value into the slot. StoreLocal auto-pops the entry
    // for the value being stored; the slot now owns it.
    ctx.emitOpcode(Opcode::StoreLocal);
    ctx.emitU16(slot);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assignment
// ─────────────────────────────────────────────────────────────────────────────

bool emitAssignStmt(AssignStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->lhs != nullptr && stmt->rhs != nullptr,
        "emitAssignStmt: an assignment is missing an operand");

    if (stmt->op == AssignOp::Assign) {
        emitPlace(stmt->lhs, ctx);
        emitExpr(stmt->rhs, ctx);
        emitStoreIntoPlace(stmt->lhs, ctx);
        return false;
    }

    // Compound assignment (`x op= y`) is not yet supported. The
    // lowering needs the load-then-store sequence, which is a
    // separate addition.
    AST_ASSERT_MSG(false,
        "emitAssignStmt: compound assignment is not yet supported — "
        "the load-then-store lowering is a Phase 4 addition");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression statement
// ─────────────────────────────────────────────────────────────────────────────

bool emitExprStmt(ExprStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->expr != nullptr,
        "emitExprStmt: an expression statement has no expression");

    emitExpr(stmt->expr, ctx);

    // If the expression's result is a resource-owning value, drop it —
    // it's a discarded temporary. The drop consumes the value from the
    // stack and auto-pops its ownership entry.
    //
    // If the result is not a resource, we still need to pop it off the
    // value stack. Ext_Pop's auto-bookkeeping removes the ownership
    // entry.
    if (stmt->expr->resolvedType != nullptr) {
        const TypeDescriptor type =
            translateType(stmt->expr->resolvedType,
                          ctx.compiler().pool());
        const ResourcePlan plan = planForType(type);
        if (plan.needsDropForStorage()) {
            // The value is on top of the stack and owns a resource.
            // Emit the drop, which consumes it.
            memory::emitDrop(ctx, plan);
        } else {
            // The value owns nothing. Pop it off the stack. Ext_Pop
            // (fixed-effect, pops=1) auto-pops the ownership entry.
            ctx.emitOpcode(Opcode::Ext_Pop);
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Return
// ─────────────────────────────────────────────────────────────────────────────

bool emitReturnStmt(ReturnStmtAST* stmt, CompilerContext& ctx) {
    if (stmt->value != nullptr) {
        // Evaluate the return value. It ends up on top of the stack
        // with its auto-pushed ownership entry.
        emitExpr(stmt->value, ctx);

        // The return value's ownership transfers to the caller. Mark
        // its entry Moved so no drop below this stack position touches
        // it. The entry was pushed by emitExpr's auto-bookkeeping;
        // markTopAsMoved overrides its state in place.
        ctx.owned().markTopAsMoved();

        // Drop every resource-typed slot in every open scope. Each
        // drop is self-balanced: dropSlot's LoadLocal auto-pushes and
        // emitDropIfOwned auto-pops.
        DropSchedule::emitReturnDrops(ctx);

        // Ext_Return (fixed-effect, pops=1) auto-pops the return
        // value's ownership entry.
        ctx.emitOpcode(Opcode::Ext_Return);
    } else {
        // No return value. Drop every resource-typed slot in every
        // open scope.
        DropSchedule::emitReturnDrops(ctx);
        // Ext_ReturnVoid has no effect on either stack.
        ctx.emitOpcode(Opcode::Ext_ReturnVoid);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Break and continue
// ─────────────────────────────────────────────────────────────────────────────

bool emitBreakStmt(BreakStmtAST* stmt, CompilerContext& ctx) {
    LoopContext* loop = findLoop(ctx, stmt->label);
    AST_ASSERT_MSG(loop != nullptr,
        "emitBreakStmt: a break statement is not inside a loop — "
        "Sema should have rejected it");

    // Drop every open scope between the break and the loop body. The
    // loop body's scope is the target; everything inside it is dropped.
    DropSchedule::emitLoopExitDrops(ctx, loop->bodyScopeIndex);

    // Emit a jump to the loop's end. The end offset is not yet known;
    // the loop emitter patches it when the loop finishes.
    const uint32_t operandOffset =
        emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
    loop->breakJumps.push_back(operandOffset);

    return true;
}

bool emitContinueStmt(ContinueStmtAST* stmt, CompilerContext& ctx) {
    LoopContext* loop = findLoop(ctx, stmt->label);
    AST_ASSERT_MSG(loop != nullptr,
        "emitContinueStmt: a continue statement is not inside a "
        "loop — Sema should have rejected it");

    // Drop every open scope between the continue and the loop body.
    DropSchedule::emitLoopExitDrops(ctx, loop->bodyScopeIndex);

    // Jump to the loop's continue target (the condition check for a
    // while, the step for a for).
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

bool emitIfStmt(IfStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->condition != nullptr,
        "emitIfStmt: an if statement has no condition");
    AST_ASSERT_MSG(stmt->thenBranch != nullptr,
        "emitIfStmt: an if statement has no then-branch");

    emitExpr(stmt->condition, ctx);

    const uint32_t toElse =
        emitJumpPlaceholder(ctx, Opcode::Ext_JumpIfFalse);

    const bool thenTransfers = emitStmt(stmt->thenBranch, ctx);

    if (stmt->elseBranch == nullptr) {
        // No else branch: the false path falls through, so the if
        // never transfers control unconditionally. The then-branch's
        // flag is irrelevant — the branch is not taken when the
        // condition is false, and the if falls through to the
        // statement after it.
        patchJumpToHere(ctx, toElse);
        return false;
    }

    const uint32_t pastElse =
        emitJumpPlaceholder(ctx, Opcode::Ext_Jump);

    patchJumpToHere(ctx, toElse);

    const bool elseTransfers = emitStmt(stmt->elseBranch, ctx);

    patchJumpToHere(ctx, pastElse);

    // The if transfers control out only if both branches do. If
    // either branch falls through, the if falls through.
    return thenTransfers && elseTransfers;
}

// ─────────────────────────────────────────────────────────────────────────────
// Switch
// ─────────────────────────────────────────────────────────────────────────────

bool emitSwitchStmt(SwitchStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->subject != nullptr,
        "emitSwitchStmt: a switch has no subject");
    AST_ASSERT_MSG(stmt->defaultBody != nullptr,
        "emitSwitchStmt: a switch has no default body — the grammar "
        "requires one");

    emitExpr(stmt->subject, ctx);

    std::vector<std::vector<uint32_t>> caseJumpOperands;
    caseJumpOperands.reserve(stmt->cases.size());

    for (auto* c : stmt->cases) {
        AST_ASSERT_MSG(c != nullptr,
            "emitSwitchStmt: a case clause is null");
        std::vector<uint32_t> jumpsForThisCase;
        for (auto* value : c->values) {
            AST_ASSERT_MSG(value != nullptr,
                "emitSwitchStmt: a case value is null");

            ctx.emitOpcode(Opcode::Ext_Dup);
            emitExpr(value, ctx);
            ctx.emitOpcode(Opcode::Eq_RowRef);

            ctx.emitOpcode(Opcode::Ext_JumpIfTrue);
            const uint32_t operandOffset = ctx.here();
            ctx.emitI32(0);
            jumpsForThisCase.push_back(operandOffset);
        }
        caseJumpOperands.push_back(std::move(jumpsForThisCase));
    }

    ctx.emitOpcode(Opcode::Ext_Pop);
    ctx.emitOpcode(Opcode::Ext_Jump);
    const uint32_t toDefaultOffset = ctx.here();
    ctx.emitI32(0);

    std::vector<uint32_t> bodyEndJumps;
    std::vector<bool> caseTransfers;
    caseTransfers.reserve(stmt->cases.size());
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        auto* c = stmt->cases[i];
        for (uint32_t operandOffset : caseJumpOperands[i]) {
            const int32_t target = static_cast<int32_t>(ctx.here());
            const int32_t base = static_cast<int32_t>(operandOffset + 4);
            ctx.patchU32(operandOffset,
                         static_cast<uint32_t>(target - base));
        }

        ctx.emitOpcode(Opcode::Ext_Pop);
        const bool bodyTransfers = emitStmt(c->body, ctx);
        caseTransfers.push_back(bodyTransfers);

        ctx.emitOpcode(Opcode::Ext_Jump);
        bodyEndJumps.push_back(ctx.here());
        ctx.emitI32(0);
    }

    {
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base = static_cast<int32_t>(toDefaultOffset + 4);
        ctx.patchU32(toDefaultOffset,
                     static_cast<uint32_t>(target - base));
    }

    const bool defaultTransfers = emitStmt(stmt->defaultBody, ctx);

    for (uint32_t operandOffset : bodyEndJumps) {
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base = static_cast<int32_t>(operandOffset + 4);
        ctx.patchU32(operandOffset,
                     static_cast<uint32_t>(target - base));
    }

    bool allTransfer = defaultTransfers;
    for (bool flag : caseTransfers) allTransfer = allTransfer && flag;
    return allTransfer;
}

// ─────────────────────────────────────────────────────────────────────────────
// While
// ─────────────────────────────────────────────────────────────────────────────

bool emitWhileStmt(WhileStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->condition != nullptr && stmt->body != nullptr,
        "emitWhileStmt: a while statement is missing its condition "
        "or its body");

    const uint32_t conditionOffset = ctx.here();

    // Push a loop context. The body scope index is the size of the
    // scope stack; the body will push its own scope when the block
    // is emitted.
    LoopContext loop;
    loop.label = stmt->label;
    loop.continueTarget = conditionOffset;
    loop.bodyScopeIndex = ctx.slots().openScopeCount();
    ctx.loops().push_back(std::move(loop));
    const size_t loopIndex = ctx.loops().size() - 1;

    emitExpr(stmt->condition, ctx);
    const uint32_t toEnd =
        emitJumpPlaceholder(ctx, Opcode::Ext_JumpIfFalse);

    emitStmt(stmt->body, ctx);

    {
        const uint32_t backJump =
            emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
        patchJumpTo(ctx, backJump, conditionOffset);
    }

    patchJumpToHere(ctx, toEnd);

    for (uint32_t breakOperand : ctx.loops()[loopIndex].breakJumps) {
        patchJumpToHere(ctx, breakOperand);
    }

    ctx.loops().pop_back();

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// For
// ─────────────────────────────────────────────────────────────────────────────

bool emitForStmt(ForStmtAST* stmt, CompilerContext& ctx) {
    AST_ASSERT_MSG(stmt->iterable != nullptr,
        "emitForStmt: a for statement has no iterable");
    AST_ASSERT_MSG(stmt->body != nullptr,
        "emitForStmt: a for statement has no body");

    if (stmt->iterable->isa<RangeExprAST>()) {
        // ─── Range loop ────────────────────────────────────────────────
        auto* range = stmt->iterable->as<RangeExprAST>();

        AST_ASSERT_MSG(stmt->firstVar != nullptr,
            "emitForStmt: a range loop has no loop variable");
        AST_ASSERT_MSG(stmt->secondVar == nullptr,
            "emitForStmt: a range loop has two binding variables — "
            "the grammar allows only one for a range");
        AST_ASSERT_MSG(range->lo != nullptr && range->hi != nullptr,
            "emitForStmt: a range is missing a bound");
        AST_ASSERT_MSG(range->lo->isConst && range->hi->isConst,
            "emitForStmt: a range bound is not a constant — "
            "the grammar requires constants");
        if (range->step != nullptr) {
            AST_ASSERT_MSG(range->step->isConst,
                "emitForStmt: a range step is not a constant");
        }

        const bool exclusive = range->isExclusive;

        // The loop variable's slot. The variable is a new scope: the
        // loop body is wrapped in a scope, and the loop variable lives
        // inside it.
        //
        // Emit the initial value first (outside the scope), then push
        // the scope, then allocate the counter slot inside it.
        emitExpr(range->lo, ctx);

        ctx.slots().pushScope();

        // The counter's type is the range's bound type (an integer).
        const TypeDescriptor counterType =
            translateType(range->lo->resolvedType,
                          ctx.compiler().pool());

        const uint16_t counterSlot =
            ctx.slots().allocateLocal(stmt->firstVar->name, counterType);
        // StoreLocal auto-pops the entry for the counter's initial
        // value, produced by the emitExpr(range->lo) above.
        ctx.emitOpcode(Opcode::StoreLocal);
        ctx.emitU16(counterSlot);

        // Push the loop context before emitting the loop proper.
        LoopContext loop;
        loop.label = stmt->label;

        const uint32_t conditionOffset = ctx.here();
        loop.continueTarget = 0;   // patched once step start is known
        loop.bodyScopeIndex = ctx.slots().openScopeCount() - 1;
        ctx.loops().push_back(std::move(loop));
        const size_t loopIndex = ctx.loops().size() - 1;

        // Condition: counter <  or  <=  hi
        ctx.emitOpcode(Opcode::LoadLocal);
        ctx.emitU16(counterSlot);
        emitExpr(range->hi, ctx);

        const TypeDescriptor boundType =
            translateType(range->lo->resolvedType,
                          ctx.compiler().pool());
        auto pk = boundType.isPrimitive()
                    ? std::optional<PrimitiveKind>(boundType.primitive)
                    : std::nullopt;
        AST_ASSERT_MSG(pk.has_value() && isIntegerKind(*pk),
            "emitForStmt: a range's bound type is not an integer — "
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
            "emitForStmt: a range's bound type is not an integer");

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

        emitStmt(stmt->body, ctx);

        const uint32_t stepOffset = ctx.here();
        ctx.loops()[loopIndex].continueTarget = stepOffset;

        // counter = counter + step
        ctx.emitOpcode(Opcode::LoadLocal);
        ctx.emitU16(counterSlot);
        if (range->step != nullptr) {
            emitExpr(range->step, ctx);
        } else {
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

        {
            const uint32_t backJump =
                emitJumpPlaceholder(ctx, Opcode::Ext_Jump);
            patchJumpTo(ctx, backJump, conditionOffset);
        }

        patchJumpToHere(ctx, toEnd);

        for (uint32_t breakOperand : ctx.loops()[loopIndex].breakJumps) {
            patchJumpToHere(ctx, breakOperand);
        }

        ctx.loops().pop_back();

        // Pop the scope. The counter's type is an integer, so its
        // plan has no drop and the scope has no drop slots.
        const ScopeRecord scope = ctx.slots().popScope();
        DropSchedule::emitScopeDrops(ctx, scope);

        return false;
    }

    // Array and table iteration are not yet supported.
    //
    // The access encodings exist: LoadIndex and LoadRow read an
    // element or a row; Ext_TableCount gives a table's row count.
    // Two things are missing:
    //
    //   1. The lowering logic for the four iterable shapes (array,
    //      array-with-index, table or FIND view, column view).
    //
    //   2. An array-count opcode. Ext_TableCount exists; there is no
    //      Ext_ArrayCount. An array loop needs the array's length.
    AST_ASSERT_MSG(false,
        "emitForStmt: array and table iteration are not yet "
        "supported — the emitter for the non-range iterable shapes "
        "has not been written, and an array-count opcode does not "
        "yet exist");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suspend points
// ─────────────────────────────────────────────────────────────────────────────

bool emitWaitStmt(WaitStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt; (void)ctx;
    AST_ASSERT_MSG(false,
        "emitWaitStmt: `wait` is not yet supported — the sequence "
        "state-machine lowering is not yet designed");
    return false;
}

bool emitWaitFramesStmt(WaitFramesStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt; (void)ctx;
    AST_ASSERT_MSG(false,
        "emitWaitFramesStmt: `waitFrames` is not yet supported — "
        "state-machine lowering is not yet designed");
    return false;
}

bool emitWaitUntilStmt(WaitUntilStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt; (void)ctx;
    AST_ASSERT_MSG(false,
        "emitWaitUntilStmt: `waitUntil` is not yet supported — "
        "state-machine lowering is not yet designed");
    return false;
}

bool emitWaitForEventStmt(WaitForEventStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt; (void)ctx;
    AST_ASSERT_MSG(false,
        "emitWaitForEventStmt: `waitForEvent` is not yet supported — "
        "state-machine lowering is not yet designed");
    return false;
}

bool emitWaitForRequestStmt(WaitForRequestStmtAST* stmt, CompilerContext& ctx) {
    (void)stmt; (void)ctx;
    AST_ASSERT_MSG(false,
        "emitWaitForRequestStmt: `waitForRequest` is not yet "
        "supported — it needs the sequence state-machine lowering "
        "(Phase 4)");
    return false;
}

} // namespace lucid::bytecode::compile