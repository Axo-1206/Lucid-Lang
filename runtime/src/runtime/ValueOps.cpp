/**
 * @file runtime/ValueOps.cpp
 *
 * @responsibility The plan-dispatched dropValue and copyValue. These
 *                 are the runtime's half of the compiler/runtime
 *                 agreement about resource management.
 *
 * ─── Design: the plan is the contract ─────────────────────────────────────
 * The compiler, when it emitted the code that produced the value on
 * the operand stack, consulted planForType on the value's type and
 * emitted the opcodes the plan required. The runtime, when it drops
 * or copies a value that is not driven by a single emitted opcode
 * (an element of an array, a cell of a table row), consults the same
 * planForType on the value's type and executes the plan. One
 * classification, two consumers, no drift.
 *
 * ─── Design: recursion for ElementWise ────────────────────────────────────
 * A fixed array of resources ([N, string]) has DropKind::ElementWise.
 * dropValue walks the elements via the type descriptor stored on the
 * Value itself (for a fixed array, the array object's element type).
 * This is where the runtime's recursion meets the plan's recursion.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Value.hpp, runtime/String.hpp, runtime/Array.hpp,
 * runtime/Handle.hpp, contract/ResourcePlan.hpp.
 */

#include "runtime/ValueOps.hpp"

#include "runtime/Array.hpp"
#include "runtime/Handle.hpp"
#include "runtime/String.hpp"

namespace lucid::runtime {

// ─────────────────────────────────────────────────────────────────────────
// dropValue
// ─────────────────────────────────────────────────────────────────────────

void dropValue(const Value& v, const contract::ResourcePlan& plan) noexcept {
    switch (plan.drop) {
        case contract::DropKind::None:
            // Nothing to drop. A primitive, a row reference, a
            // function value.
            return;

        case contract::DropKind::Discard:
            // Nothing to release; the caller wanted to pop the value
            // off the stack. At the runtime layer, this is a no-op
            // (the caller handles the stack).
            return;

        case contract::DropKind::FreeString:
            // The value should be a String. Defensive: if it is not,
            // there is nothing to free (the plan and the value
            // disagree, which would be a compiler bug, not a runtime
            // one; ignoring is the safe response).
            if (v.tag == ValueTag::String) {
                releaseString(v.asString());
            }
            return;

        case contract::DropKind::FreeArray:
            if (v.tag == ValueTag::Array) {
                releaseArray(v.asArray());
            }
            return;

        case contract::DropKind::Release:
            if (v.tag == ValueTag::HostHandle) {
                releaseHandle(v.asHostHandle());
            }
            return;

        case contract::DropKind::ElementWise: {
            // A fixed array of resources. The Value is an Array with
            // fixed == true. Walk its elements and drop each. The
            // element's plan is computed from the array's element type
            // descriptor, which the array carries.
            if (v.tag != ValueTag::Array) return;
            ArrayObject* arr = v.asArray();
            if (!arr || !arr->elementType) return;

            const contract::ResourcePlan elemPlan =
                contract::planForType(*arr->elementType);
            if (!elemPlan.needsDropForStorage()) return;

            for (uint32_t i = 0; i < arr->length; ++i) {
                dropValue(arr->data[i], elemPlan);
            }
            // Note: we do NOT free the array's buffer here. That is
            // the array's own drop, done by DropKind::FreeArray on
            // the containing value. ElementWise only drops the
            // elements; the buffer's release is separate.
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// copyValue
// ─────────────────────────────────────────────────────────────────────────

Value copyValue(const Value& v, const contract::ResourcePlan& plan) {
    switch (plan.copy) {
        case contract::CopyKind::BitCopy:
        case contract::CopyKind::Reference:
            // No resource; a bit copy is the whole operation.
            return v;

        case contract::CopyKind::DeepCopyString:
            if (v.tag == ValueTag::String) {
                return Value::makeString(copyString(v.asString()));
            }
            return v;

        case contract::CopyKind::DeepCopyArray:
            if (v.tag == ValueTag::Array) {
                return Value::makeArray(copyArray(v.asArray()));
            }
            return v;

        case contract::CopyKind::Retain:
            if (v.tag == ValueTag::HostHandle) {
                retainHandle(v.asHostHandle());
            }
            return v;

        case contract::CopyKind::ElementWise: {
            // A fixed array of resources. Deep-copy every element.
            // The result is a new fixed array of the same length,
            // whose elements are individually copied.
            if (v.tag != ValueTag::Array) return v;
            ArrayObject* src = v.asArray();
            if (!src) return v;

            ArrayObject* dst = allocFixedArray(src->elementType,
                                               src->length);
            if (!dst) return v;  // allocation failure: return the original

            const contract::ResourcePlan elemPlan =
                contract::planForType(*src->elementType);
            for (uint32_t i = 0; i < src->length; ++i) {
                dst->data[i] = copyValue(src->data[i], elemPlan);
            }
            return Value::makeArray(dst);
        }
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────
// bitCopyPlan
// ─────────────────────────────────────────────────────────────────────────

contract::ResourcePlan bitCopyPlan() noexcept {
    contract::ResourcePlan p;
    p.copy = contract::CopyKind::BitCopy;
    p.drop = contract::DropKind::None;
    p.move = contract::MoveKind::BitMove;
    return p;
}

} // namespace lucid::runtime