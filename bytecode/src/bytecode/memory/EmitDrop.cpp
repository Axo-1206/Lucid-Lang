/// @file bytecode/memory/EmitDrop.cpp
/// @brief Emit a drop of a value's resources.
///
/// ─── Ownership bookkeeping ────────────────────────────────────────────────
/// The ownership stack is kept in sync with the value stack by
/// CompilerContext::emitOpcode. This file does not push or pop entries
/// for the count.
///
/// emitDrop consumes the value on top of the stack. The opcode it
/// emits (Ext_Pop, Ext_RtCall <Release | FreeString | FreeArray>)
/// auto-pops the value's ownership entry:
///   - Ext_Pop is fixed-effect (pops=1).
///   - Ext_RtCall is variable-effect; emitRtCall calls
///     noteStackEffect(1, 0), which auto-pops.
///
/// emitDropIfOwned peeks the top entry first. If it is Owned, it
/// calls emitDrop (which auto-pops). If it is Moved or BitCopy, it
/// emits Ext_Pop (which auto-pops).

#include "EmitDrop.hpp"
#include "EmitRtCall.hpp"

#include "contract/Opcode.hpp"
#include "contract/RuntimeOp.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

namespace lucid::bytecode::memory {

void emitDrop(compile::CompilerContext& ctx, const ResourcePlan& plan) {
    switch (plan.drop) {
        case DropKind::None:
        case DropKind::Discard: {
            // Ext_Pop (fixed-effect, pops=1) consumes the value and
            // its ownership entry via auto-bookkeeping. No manual
            // pop.
            ctx.emitOpcode(Opcode::Ext_Pop);
            return;
        }

        case DropKind::Release: {
            // Release is a variable-effect runtime op (pops=1,
            // pushes=0). emitRtCall calls noteStackEffect(1, 0),
            // which auto-pops the handle's ownership entry.
            emitRtCall(ctx, RuntimeOp::Release);
            return;
        }

        case DropKind::FreeString: {
            // FreeString is a variable-effect runtime op
            // (pops=1, pushes=0). Auto-pops the string's entry.
            emitRtCall(ctx, RuntimeOp::FreeString);
            return;
        }

        case DropKind::FreeArray: {
            // FreeArray is a variable-effect runtime op
            // (pops=1, pushes=0). Auto-pops the array's entry.
            emitRtCall(ctx, RuntimeOp::FreeArray);
            return;
        }

        case DropKind::ElementWise:
            // A fixed-size aggregate with resource-typed elements.
            // The lowering walks the elements and drops each. Not
            // yet implemented. (All other DropKinds are implemented;
            // this is the only gap in EmitDrop.)
            AST_ASSERT_MSG(false,
                "emitDrop: ElementWise drop (a fixed-size aggregate "
                "with resource-typed elements) requires a lowering "
                "that walks the elements. Not yet implemented.");
            return;
    }

    AST_ASSERT_MSG(false,
        "emitDrop: unhandled DropKind — the emitter is out of sync "
        "with the ResourcePlan enum");
}

void emitDropIfOwned(compile::CompilerContext& ctx,
                     const ResourcePlan& plan) {
    if (ctx.owned().peek() != Ownership::Owned) {
        // The top entry is Moved or BitCopy — the value does not
        // own a live resource (it was transferred, or it never
        // owned anything). Just discard the value from the stack.
        // Ext_Pop auto-pops its ownership entry.
        ctx.emitOpcode(Opcode::Ext_Pop);
        return;
    }
    emitDrop(ctx, plan);
}

} // namespace lucid::bytecode::memory