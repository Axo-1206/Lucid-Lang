/// @file bytecode/memory/ResourcePlan.cpp
/// @brief Classify a type by its copy/drop/move behavior.

#include "ResourcePlan.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode::memory {

// ─────────────────────────────────────────────────────────────────────────────
// Classification
// ─────────────────────────────────────────────────────────────────────────────
//
// The classification walks the TypeDescriptor and produces a plan. The
// cases map directly to the TypeDescriptor's Kind:
//
//   Primitive(String)   → DeepCopyString / FreeString / TransferOwnership
//   Primitive(other)    → BitCopy / None / BitMove
//   Named(host type)    → Retain / Release / TransferOwnership
//   Named(table)        → Reference / None / BitMove
//   Array(Dynamic)      → DeepCopyArray / FreeArray / TransferOwnership
//   Array(Fixed)        → ElementWise / ElementWise / TransferOwnership
//                          (or BitCopy / None / BitMove if the element
//                           type owns nothing)
//   RowRef              → BitCopy / None / BitMove
//   Function            → BitCopy / None / BitMove
//   Nullable(inner)     → same as inner; the interpreter checks nil
//                          before applying the plan
//   Unknown             → asserted; never reaches a real compile
//
// ─── Design: no summary input ─────────────────────────────────────────────
// planForType reads only the TypeDescriptor. It does not consult any
// cached classification;
// The plan is a total function of the type, so it cannot disagree with
// the type it describes.

ResourcePlan planForType(const TypeDescriptor& type) {
    ResourcePlan plan;

    // ─── Nullable: same classification as the inner type ───────────────
    //
    // A `T?` where T owns a resource has the same copy/drop behavior
    // as T. A nil value owns nothing at runtime, but the *plan* is the
    // inner type's plan; the interpreter checks for nil before applying
    // it.
    if (type.isNullable()) {
        AST_ASSERT_MSG(type.component != nullptr,
            "planForType: a NullableType has no inner type — the "
            "type descriptor was not built correctly");
        return planForType(*type.component);
    }

    // ─── Primitive ─────────────────────────────────────────────────────
    if (type.isPrimitive()) {
        if (type.primitive == PrimitiveKind::String) {
            plan.copy = CopyKind::DeepCopyString;
            plan.drop = DropKind::FreeString;
            plan.move = MoveKind::TransferOwnership;
            return plan;
        }
        // Every other primitive is a bit-copyable value that owns
        // nothing.
        plan.copy = CopyKind::BitCopy;
        plan.drop = DropKind::None;
        plan.move = MoveKind::BitMove;
        return plan;
    }

    // ─── Named: a table or a host type ─────────────────────────────────
    if (type.isNamed()) {
        if (type.isHostType) {
            // A host type is a refcounted opaque handle.
            plan.copy = CopyKind::Retain;
            plan.drop = DropKind::Release;
            plan.move = MoveKind::TransferOwnership;
            return plan;
        }
        // An ordinary table reference owns nothing.
        plan.copy = CopyKind::Reference;
        plan.drop = DropKind::None;
        plan.move = MoveKind::BitMove;
        return plan;
    }

    // ─── Array ─────────────────────────────────────────────────────────
    if (type.isArray()) {
        if (type.arrayKind == ArrayKind::Dynamic) {
            // A dynamic array owns its heap buffer.
            plan.copy = CopyKind::DeepCopyArray;
            plan.drop = DropKind::FreeArray;
            plan.move = MoveKind::TransferOwnership;
            return plan;
        }

        // A fixed array's elements are inline. Its copy and drop are
        // element-wise, unless no element owns anything — in which
        // case the aggregate is a bit-copyable value with no drops.
        if (type.component != nullptr) {
            ResourcePlan elem = planForType(*type.component);
            if (elem.ownsResources()) {
                plan.copy = CopyKind::ElementWise;
                plan.drop = DropKind::ElementWise;
                plan.move = MoveKind::TransferOwnership;
                return plan;
            }
        }
        plan.copy = CopyKind::BitCopy;
        plan.drop = DropKind::None;
        plan.move = MoveKind::BitMove;
        return plan;
    }

    // ─── RowRef: a reference to a row ──────────────────────────────────
    if (type.isRowRef()) {
        plan.copy = CopyKind::BitCopy;
        plan.drop = DropKind::None;
        plan.move = MoveKind::BitMove;
        return plan;
    }

    // ─── Function: a code address ──────────────────────────────────────
    if (type.isFunction()) {
        plan.copy = CopyKind::BitCopy;
        plan.drop = DropKind::None;
        plan.move = MoveKind::BitMove;
        return plan;
    }

    // ─── Unknown ───────────────────────────────────────────────────────
    //
    // An Unknown type reaching the classifier means Sema did not
    // resolve a type before the compiler ran. Sema rejects those
    // modules before codegen, so this is a Sema bug.
    AST_ASSERT_MSG(false,
        "planForType: an Unknown TypeDescriptor — Sema should have "
        "resolved every type before the compiler ran");
    plan.copy = CopyKind::BitCopy;
    plan.drop = DropKind::None;
    plan.move = MoveKind::BitMove;
    return plan;
}

bool needsScopeExitDrop(const ResourcePlan& plan) noexcept {
    return plan.needsDropForStorage();
}

} // namespace lucid::bytecode::memory