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

namespace {

/// Emit the drop of a fixed array of resources. The array value is
/// on top of the stack. Each element is dropped via the recursive
/// emitDrop; the array itself is then discarded (no `FreeArray` —
/// a fixed array has no heap buffer; only its elements own
/// resources).
///
/// The unrolled form: N iterations, each with a constant element
/// index.
///
/// The stack discipline:
///   - Entry: [..., array]
///   - Exit:  [...]
void emitElementWiseDrop(compile::CompilerContext& ctx,
                         const contract::TypeDescriptor& type) {
    AST_ASSERT_MSG(type.isArray(),
        "emitElementWiseDrop: the type is not an array — the "
        "ElementWise plan only applies to fixed arrays");
    AST_ASSERT_MSG(type.arrayKind == ArrayKind::Fixed,
        "emitElementWiseDrop: the array is dynamic — a dynamic "
        "array's drop is FreeArray, not ElementWise");
    AST_ASSERT_MSG(type.component != nullptr,
        "emitElementWiseDrop: a fixed array has no element type — "
        "the type descriptor was not built correctly");

    const uint64_t N = type.fixedSize;
    AST_ASSERT_MSG(N <= 4096,
        "emitElementWiseDrop: a fixed array of more than 4096 "
        "elements would produce an unreasonably large unrolled "
        "sequence — the design decision was to unroll; if a larger "
        "array is needed, revisit that decision");

    const TypeDescriptor& elemType = *type.component;

    // For each element, duplicate the array, read the element, and
    // drop it.
    //
    // We drop in reverse declaration order (element N-1 first, then
    // N-2, ..., then 0) to match the "reverse order" convention for
    // scope drops. For a fixed array of resources, the order matters
    // only if two elements share a resource; reverse order is the
    // conservative choice.
    for (uint64_t i = N; i-- > 0; ) {
        ctx.emitOpcode(Opcode::Ext_Dup);           // duplicate the array
        ctx.emitOpcode(Opcode::Ext_FixedArrayGet); // consume dup, push element
        ctx.emitU32(static_cast<uint32_t>(i));
        // Stack: [..., array, element_i]
        //
        // The element's ownership entry was auto-pushed by
        // Ext_FixedArrayGet (BitCopy). If the element owns a
        // resource, upgrade it so emitDrop fires.
        if (planForType(elemType).ownsResources()) {
            ctx.owned().markTopAsOwned();
        }
        emitDrop(ctx, elemType);
        // Stack: [..., array]
    }

    // The array itself: discard it. It owns no heap buffer; the
    // elements are what owned the resources, and they've all been
    // dropped.
    ctx.emitOpcode(Opcode::Ext_Pop);
    // Stack: [...]
}

} // namespace

void emitDrop(compile::CompilerContext& ctx, const TypeDescriptor& type) {
    const ResourcePlan plan = planForType(type);
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
            // The drop walks the elements: for each element index,
            // load the element, drop it. The aggregate being dropped
            // is on top of the stack; it is consumed at the end.
            emitElementWiseDrop(ctx, type);
            return;
    }

    AST_ASSERT_MSG(false,
        "emitDrop: unhandled DropKind — the emitter is out of sync "
        "with the ResourcePlan enum");
}

void emitDropIfOwned(compile::CompilerContext& ctx,
                     const TypeDescriptor& type) {
    if (ctx.owned().peek() != Ownership::Owned) {
        // The top entry is Moved or BitCopy — the value does not
        // own a live resource (it was transferred, or it never
        // owned anything). Just discard the value from the stack.
        // Ext_Pop auto-pops its ownership entry.
        ctx.emitOpcode(Opcode::Ext_Pop);
        return;
    }
    emitDrop(ctx, type);
}

} // namespace lucid::bytecode::memory