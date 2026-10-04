/**
 * @file runtime/ValueOps.hpp
 *
 * @responsibility The runtime's plan-dispatched value operations:
 *                 dropValue and copyValue. These are the operations
 *                 that consult a contract::ResourcePlan to decide how
 *                 to release or duplicate a value of a given type.
 *
 * ─── Design: dispatch on the plan, not the type ───────────────────────────
 * The runtime never asks "is this a string?" and then decides to free
 * a string buffer. It asks "what does the plan say to do with a value
 * of this type?" and executes that. The plan is derived from the type
 * by contract::planForType; the compiler uses the same function to
 * decide which opcodes to emit. This is what makes compiler-emitted
 * drops and runtime-executed drops agree.
 *
 * ─── Who uses this ────────────────────────────────────────────────────────
 *   - releaseArray: drops each element before freeing the buffer.
 *   - copyArray:    copies each element into the new buffer.
 *   - arraySet:     drops the old element before the write.
 *   - arrayRemove:  drops the removed element.
 *   - The interpreter's TableObject uses these for cell drops in
 *     removeRow, setCell, and clear.
 *
 * ─── Design: single-value operations ──────────────────────────────────────
 * Both functions take one Value and one ResourcePlan. Recursion for
 * aggregates is handled inside the switch, by looking up the element's
 * plan and calling back into these functions. The caller does not
 * have to walk the structure.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Value.hpp, contract/ResourcePlan.hpp.
 */

#pragma once

#include "contract/ResourcePlan.hpp"
#include "runtime/Value.hpp"

namespace lucid::runtime {

/// @brief Drop a value according to its plan. After this call, the
///        value is dead; it must not be used again.
///
/// The plan is expected to be planForType(the value's type). Passing a
/// mismatched plan is a bug in the caller; the result is undefined.
///
/// For an aggregate (a fixed array of resources, a nested array, a
/// table cell holding a resource), the plan's DropKind::ElementWise
/// causes a recursive walk: the function reads the value's own element
/// type descriptor and calls planForType on each element.
void dropValue(const Value& v, const contract::ResourcePlan& plan) noexcept;

/// @brief Produce a copy of a value according to its plan. The
///        original is unchanged; the caller owns the returned value.
///
/// For a resource type, the copy is deep: a new string buffer, a new
/// array buffer with copied elements, or a retained handle. For a
/// primitive or a reference with no ownership, the copy is a bit copy.
Value copyValue(const Value& v, const contract::ResourcePlan& plan);

/// @brief The default plan for a value that carries no resource
///        ownership. Used by callers that have a value but no type
///        descriptor (rare; test code, mostly).
///
/// Returns a plan with CopyKind::BitCopy, DropKind::None,
/// MoveKind::BitMove.
contract::ResourcePlan bitCopyPlan() noexcept;

} // namespace lucid::runtime