/// @file bytecode/memory/EmitCopy.cpp
/// @brief Emit a deep copy of a value.

#include "EmitCopy.hpp"
#include "EmitRtCall.hpp"

#include "contract/Opcode.hpp"
#include "contract/RuntimeOp.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

namespace lucid::bytecode::memory {

void emitCopy(compile::CompilerContext& ctx, const ResourcePlan& plan) {
    switch (plan.copy) {
        case CopyKind::BitCopy:
        case CopyKind::Reference: {
            ctx.emitOpcode(Opcode::Ext_Dup);
            ctx.owned().pushOwned();
            return;
        }

        case CopyKind::Retain: {
            // Retain the handle (no stack change), then Dup it so
            // the caller has both the original and the retained
            // copy.
            emitRtCall(ctx, RuntimeOp::Retain);
            ctx.emitOpcode(Opcode::Ext_Dup);
            ctx.owned().pushOwned();
            return;
        }

        case CopyKind::DeepCopyString: {
            // Dup the original so both survive, then deep-copy the
            // duplicate.
            ctx.emitOpcode(Opcode::Ext_Dup);
            ctx.owned().pushOwned();
            emitRtCall(ctx, RuntimeOp::CopyString);
            return;
        }

        case CopyKind::DeepCopyArray: {
            ctx.emitOpcode(Opcode::Ext_Dup);
            ctx.owned().pushOwned();
            emitRtCall(ctx, RuntimeOp::CopyArray);
            return;
        }

        case CopyKind::ElementWise:
            // A fixed-size aggregate with resource-typed elements.
            // The lowering walks the elements and copies each. Not
            // yet implemented. (All other CopyKinds are implemented;
            // this is the only gap in EmitCopy.)
            AST_ASSERT_MSG(false,
                "emitCopy: ElementWise copy (a fixed-size aggregate "
                "with resource-typed elements) requires a lowering "
                "that walks the elements. Not yet implemented.");
            return;
    }

    AST_ASSERT_MSG(false,
        "emitCopy: unhandled CopyKind — the emitter is out of sync "
        "with the ResourcePlan enum");
}

} // namespace lucid::bytecode::memory