/// @file bytecode/memory/EmitCopy.cpp
/// @brief Emit a deep copy of a value.
///
/// ─── Ownership bookkeeping ────────────────────────────────────────────────
/// The ownership stack is kept in sync with the value stack by
/// CompilerContext::emitOpcode. This file does not push or pop entries
/// for the count.
///
/// The pattern per case:
///   - Ext_Dup auto-pushes one BitCopy entry for the copy.
///   - A runtime copy op (CopyString / CopyArray) auto-pops the
///     duplicate's entry and auto-pushes a BitCopy entry for the
///     fresh value.
///   - The emitter calls markTopAsOwned when the fresh value owns a
///     resource (a fresh heap buffer, a retained handle).
///
/// After emitCopy, the ownership stack has one more entry than
/// before: the copy's entry. The original's entry is untouched.

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
            // Ext_Dup duplicates the top value. Its auto-bookkeeping
            // is special-cased: the original's ownership entry
            // survives, and one BitCopy entry is pushed for the copy.
            // A bit-copied value does not own a resource, so BitCopy
            // is correct and no mark is needed.
            ctx.emitOpcode(Opcode::Ext_Dup);
            return;
        }

        case CopyKind::Retain: {
            // Retain the handle (no stack change; the top of stack
            // stays put), then Dup it so the caller has both the
            // original and the retained copy.
            //
            // Retain is a variable-effect runtime op with
            // pops=0, pushes=0 — no ownership change.
            // Ext_Dup auto-pushes one BitCopy entry for the copy.
            // The copy is a second handle to the same underlying
            // object, so it owns a resource (the caller must release
            // its refcount eventually). Upgrade the entry to Owned.
            emitRtCall(ctx, RuntimeOp::Retain);
            ctx.emitOpcode(Opcode::Ext_Dup);
            ctx.owned().markTopAsOwned();
            return;
        }

        case CopyKind::DeepCopyString: {
            // Dup the original so both survive, then deep-copy the
            // duplicate.
            //
            // Ext_Dup auto-pushes one BitCopy entry for the copy.
            // CopyString is a variable-effect runtime op
            // (pops=1, pushes=1): it consumes the duplicate and
            // produces a fresh string. The auto-bookkeeping pops the
            // duplicate's entry and pushes a new BitCopy for the
            // fresh string. The fresh string owns a heap buffer, so
            // upgrade it to Owned.
            ctx.emitOpcode(Opcode::Ext_Dup);
            emitRtCall(ctx, RuntimeOp::CopyString);
            ctx.owned().markTopAsOwned();
            return;
        }

        case CopyKind::DeepCopyArray: {
            // Same shape as DeepCopyString: the array copy produces
            // a fresh heap buffer that the value owns.
            ctx.emitOpcode(Opcode::Ext_Dup);
            emitRtCall(ctx, RuntimeOp::CopyArray);
            ctx.owned().markTopAsOwned();
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