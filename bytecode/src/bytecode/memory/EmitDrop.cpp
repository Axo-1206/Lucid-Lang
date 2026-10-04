/// @file bytecode/memory/EmitDrop.cpp
/// @brief Emit a drop of a value's resources.

#include "EmitDrop.hpp"

#include "contract/Opcode.hpp"
#include "contract/RuntimeOp.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

namespace lucid::bytecode::memory {

namespace {

void emitRtCall(compile::CompilerContext& ctx, RuntimeOp op) {
    ctx.emitOpcode(Opcode::Ext_RtCall);
    ctx.emitU8(static_cast<uint8_t>(op));
    const RuntimeOpInfo& info = runtimeOpInfo(op);
    ctx.noteStackEffect(info.pops, info.pushes);
}

} // namespace

void emitDrop(compile::CompilerContext& ctx, const ResourcePlan& plan) {
    switch (plan.drop) {
        case DropKind::None:
        case DropKind::Discard: {
            ctx.emitOpcode(Opcode::Ext_Pop);
            ctx.owned().pop();
            return;
        }

        case DropKind::Release: {
            emitRtCall(ctx, RuntimeOp::Release);
            ctx.owned().pop();
            return;
        }

        case DropKind::FreeString: {
            emitRtCall(ctx, RuntimeOp::FreeString);
            ctx.owned().pop();
            return;
        }

        case DropKind::FreeArray: {
            emitRtCall(ctx, RuntimeOp::FreeArray);
            ctx.owned().pop();
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
        ctx.emitOpcode(Opcode::Ext_Pop);
        ctx.owned().pop();
        return;
    }
    emitDrop(ctx, plan);
}

} // namespace lucid::bytecode::memory