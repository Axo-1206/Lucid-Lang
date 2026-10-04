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

namespace {

/// Emit the copy of a fixed array of resources. The array value is
/// on top of the stack. The lowering produces a second array value
/// above it, with each element copied per its plan.
///
/// The unrolled form: N iterations, each with a constant element
/// index. No loop, no counter.
///
/// The stack discipline:
///   - Entry: [..., original]
///   - Exit:  [..., original, copy]
///
/// The walk reads each element from the original (via Ext_Dup +
/// Ext_FixedArrayGet), recursively copies it, and finally constructs
/// a fresh fixed array from the N copied elements via
/// Ext_NewFixedArray.
void emitElementWiseCopy(compile::CompilerContext& ctx,
                         const contract::TypeDescriptor& type) {

    AST_ASSERT_MSG(type.isArray(),
        "emitElementWiseCopy: the type is not an array — the "
        "ElementWise plan only applies to fixed arrays");
    AST_ASSERT_MSG(type.arrayKind == ArrayKind::Fixed,
        "emitElementWiseCopy: the array is dynamic — a dynamic "
        "array's copy is DeepCopyArray, not ElementWise");
    AST_ASSERT_MSG(type.component != nullptr,
        "emitElementWiseCopy: a fixed array has no element type — "
        "the type descriptor was not built correctly");

    const uint64_t N = type.fixedSize;
    AST_ASSERT_MSG(N <= 4096,
        "emitElementWiseCopy: a fixed array of more than 4096 "
        "elements would produce an unreasonably large unrolled "
        "sequence — the design decision was to unroll; if a larger "
        "array is needed, revisit that decision");
    AST_ASSERT_MSG(N <= 127,
        "emitElementWiseCopy: N exceeds noteStackEffect's int8_t "
        "range — widen noteStackEffect's signature or split the "
        "construction");

    const TypeDescriptor& elemType = *type.component;

    // Read each element from the original and copy it. The original
    // stays on the stack; each iteration duplicates it, reads one
    // element, and copies the element.
    for (uint64_t i = 0; i < N; ++i) {
        ctx.emitOpcode(Opcode::Ext_Dup);           // duplicate the original
        ctx.emitOpcode(Opcode::Ext_FixedArrayGet); // consume the dup, push element
        ctx.emitU32(static_cast<uint32_t>(i));
        emitCopy(ctx, elemType);                    // recursive: push the element's copy
    }

    // Now the stack is [..., original, copy_elem_0, ..., copy_elem_{N-1}].
    // Construct a fresh fixed array from the N copies.
    ctx.emitOpcode(Opcode::Ext_NewFixedArray);
    ctx.emitU32(static_cast<uint32_t>(N));

    // The constructor's stack effect is variable: it pops N elements
    // and pushes one array. OpcodeInfo carries -1 for both, so the
    // emitter supplies the actual effect.
    ctx.noteStackEffect(static_cast<int8_t>(N), 1);
    // Stack: [..., original, copy]

    // The original's ownership entry: it was on the stack before
    // emitCopy was called, and it's still there. Its state is
    // unchanged.
    //
    // The copy's ownership entry: Ext_NewFixedArray auto-pushed a
    // BitCopy entry (via noteStackEffect). Since a fixed array of
    // resources owns resources, upgrade it to Owned.
    ctx.owned().markTopAsOwned();
}

} // namespace

void emitCopy(compile::CompilerContext& ctx, const contract::TypeDescriptor& type) {
    const ResourcePlan plan = planForType(type);
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
            // The copy walks the elements: for each element index,
            // load the element, copy it, and store it back into a
            // fresh aggregate. The aggregate being copied is on top
            // of the stack; the walk produces a copy below it,
            // element by element.
            //
            // This is the unrolled form: N is known at compile time
            // (the type descriptor's fixedSize), so each iteration
            // emits straight-line code for a constant element index.
            // No loop counter, no branch.
            //
            // Precondition: the value stack top is a fixed array of
            // the given type. The type descriptor carries the element
            // type and the count.
            emitElementWiseCopy(ctx, type);
            return;
    }

    AST_ASSERT_MSG(false,
        "emitCopy: unhandled CopyKind — the emitter is out of sync "
        "with the ResourcePlan enum");
}

} // namespace lucid::bytecode::memory