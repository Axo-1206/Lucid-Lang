/// @file bytecode/memory/DropSchedule.cpp
/// @brief Orchestrate scope-exit drop emission.

#include "DropSchedule.hpp"
#include "EmitDrop.hpp"

#include "bytecode/Opcode.hpp"
#include "bytecode/compile/CompilerContext.hpp"
#include "bytecode/compile/Compiler.hpp"
#include "bytecode/compile/SlotAllocator.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode::memory {

namespace {

void dropSlot(compile::CompilerContext& ctx, uint16_t slot) {
    const TypeDescriptor& type = ctx.slots().typeOf(slot);
    const ResourcePlan plan = planForType(type);

    if (!plan.needsDropForStorage()) {
        // The slot owns nothing that needs a drop (a primitive, a
        // reference, a fixed array of non-resources, ...).
        return;
    }

    // Load the slot's value, then drop it.
    ctx.emitOpcode(Opcode::LoadLocal);
    ctx.emitU16(slot);
    ctx.owned().pushOwned();

    emitDropIfOwned(ctx, plan);
}

} // namespace

void DropSchedule::emitScopeDrops(
    compile::CompilerContext& ctx,
    const compile::ScopeRecord& scope) {
    for (auto it = scope.dropSlots.rbegin();
         it != scope.dropSlots.rend(); ++it) {
        dropSlot(ctx, *it);
    }
}

void DropSchedule::emitReturnDrops(compile::CompilerContext& ctx) {
    const auto scopes = ctx.slots().openScopeDropSlots();
    for (const auto* slots : scopes) {
        for (auto it = slots->rbegin(); it != slots->rend(); ++it) {
            dropSlot(ctx, *it);
        }
    }
}

void DropSchedule::emitLoopExitDrops(
    compile::CompilerContext& ctx,
    size_t targetScopeIndex) {
    const auto scopes = ctx.slots().openScopeDropSlots();

    AST_ASSERT_MSG(targetScopeIndex < scopes.size(),
        "DropSchedule::emitLoopExitDrops: target scope index is "
        "out of range — the caller's scope indexing is wrong");

    const size_t innermostTargetIdx = scopes.size() - 1 - targetScopeIndex;

    for (size_t i = 0; i < innermostTargetIdx; ++i) {
        const auto* slots = scopes[i];
        for (auto it = slots->rbegin(); it != slots->rend(); ++it) {
            dropSlot(ctx, *it);
        }
    }
}

} // namespace lucid::bytecode::memory