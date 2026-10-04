/// @file bytecode/memory/DropSchedule.cpp
/// @brief Orchestrate scope-exit drop emission.

#include "DropSchedule.hpp"
#include "EmitDrop.hpp"

#include "contract/Opcode.hpp"
#include "bytecode/compile/CompilerContext.hpp"
#include "bytecode/compile/Compiler.hpp"
#include "bytecode/compile/SlotAllocator.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

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
    // Drop every open scope's resource-typed locals first, innermost
    // scope first, reverse declaration order within each scope.
    //
    // A return may occur inside a nested block. Every block between
    // the return and the function body must have its resource-typed
    // locals dropped before the function leaves. Walking the open
    // scopes' dropSlots covers exactly those blocks.
    //
    // A scope's dropSlots holds only the slots whose types require a
    // drop — allocateLocal filters at allocation time — so every
    // entry in the list produces an emitted drop. dropSlot still
    // re-checks the plan; the redundancy is cheap and keeps dropSlot
    // safe for any caller.
    const auto scopes = ctx.slots().openScopeDropSlots();
    for (const auto* slots : scopes) {
        for (auto it = slots->rbegin(); it != slots->rend(); ++it) {
            dropSlot(ctx, *it);
        }
    }

    // Drop the parameters after the scopes, reverse declaration
    // order.
    //
    // Parameters are the outermost frame storage. A scope's locals
    // may reference them (a local `let s: string = p` copies from
    // the parameter `p`), so the locals must be dropped before the
    // parameters they might share a resource with.
    //
    // Unlike a scope's dropSlots, paramSlots() holds every parameter
    // slot regardless of type. dropSlot consults the plan and skips
    // a parameter whose type owns nothing, so the loop is
    // unconditional here.
    const auto& params = ctx.slots().paramSlots();
    for (auto it = params.rbegin(); it != params.rend(); ++it) {
        dropSlot(ctx, *it);
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