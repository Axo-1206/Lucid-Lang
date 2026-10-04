/**
 * @file contract/memory/ResourcePlan.hpp
 *
 * @responsibility Classify a TypeDescriptor by how values of that type
 *                 are copied, dropped, and moved. The plan is a pure
 *                 function of the type; it answers what the compiler
 *                 must emit at every point where a value of that type
 *                 changes ownership.
 *
 * ─── Design: three questions per type ─────────────────────────────────────
 * A ResourcePlan answers:
 *
 *   1. Copy: how does a bit-pattern become an owned copy? A primitive
 *      is a bit copy; a string is a heap-buffer deep copy; a host
 *      handle is a refcount retain; an aggregate is element-wise.
 *
 *   2. Drop: when a value goes out of scope, what releases its
 *      resources? Nothing for a primitive; the buffer's free for a
 *      string or array; the host's release function for a handle;
 *      element-wise for an aggregate.
 *
 *   3. Move: when a value is transferred, what happens? Nothing for a
 *      primitive (it's a bit copy); the source becomes dead for a
 *      resource-owning type.
 *
 * ─── Design: the plan is derived from the type, not from a summary ────────
 * The plan is computed by planForType from the TypeDescriptor. It does
 * not read any cached classification; there is no separate "resource
 * kind" input. This makes the plan and the type impossible to disagree:
 * the plan is a total function of the type.
 *
 * ─── Design: the plan does not depend on the code point ───────────────────
 * A ResourcePlan is a property of a type. Whether the current code
 * point requires a copy, a drop, or nothing is the emitter's decision;
 * the plan says how to do whichever the emitter asks for.
 */

#pragma once

#include "TypeDescriptor.hpp"

#include <cstdint>

namespace lucid::contract::memory {

/// @brief How a value of a given type is copied.
enum class CopyKind : uint8_t {
    /// The type owns nothing. A bit copy suffices. `Ext_Dup` or an
    /// equivalent bitwise copy is the whole operation.
    BitCopy,

    /// The type owns a heap-allocated string buffer. A copy is a
    /// deep copy: allocate a new buffer, memcpy the bytes.
    DeepCopyString,

    /// The type owns a heap-allocated array buffer. A copy is a deep
    /// copy: allocate a new buffer, element-wise copy or retain.
    DeepCopyArray,

    /// The type is a host handle. A copy is a refcount retain.
    Retain,

    /// The type is a fixed-size aggregate (a `[N, T]` where T is a
    /// resource). A copy walks the elements and copies each per its
    /// own plan.
    ElementWise,

    /// The type owns nothing but has an opaque representation (a bare
    /// table reference, a function value). A bit copy is correct but
    /// is spelled differently from `BitCopy` for clarity.
    Reference,
};

/// @brief How a value of a given type is dropped.
enum class DropKind : uint8_t {
    /// The type owns nothing. Dropping is a no-op.
    None,

    /// The type owns a heap-allocated string buffer. Dropping frees
    /// the buffer.
    FreeString,

    /// The type owns a heap-allocated array buffer. Dropping frees
    /// the buffer (element-wise for resource-typed elements).
    FreeArray,

    /// The type is a host handle. Dropping releases the refcount.
    Release,

    /// The type is a fixed-size aggregate. Dropping walks the elements
    /// and drops each per its own plan.
    ElementWise,

    /// The type owns nothing but the code point wants to consume the
    /// value from the stack (a discarded result). A `Ext_Pop` is
    /// emitted.
    Discard,
};

/// @brief How a value of a given type is moved.
enum class MoveKind : uint8_t {
    /// The type owns nothing. Moving is a bit copy; the source is
    /// still valid.
    BitMove,

    /// The type owns a resource. Moving transfers ownership; the
    /// source becomes dead and must NOT be dropped at scope exit.
    TransferOwnership,
};

/// @brief The full classification of a type.
struct ResourcePlan {
    CopyKind copy = CopyKind::BitCopy;
    DropKind drop = DropKind::None;
    MoveKind move = MoveKind::BitMove;

    /// True if dropping a value of this type requires any emitted
    /// code. A plan with DropKind::None or DropKind::Discard never
    /// needs an explicit drop for storage release; Discard only needs
    /// a Pop.
    bool needsDropForStorage() const noexcept {
        return drop != DropKind::None && drop != DropKind::Discard;
    }

    /// True if copying a value of this type requires any emitted
    /// code beyond a bit copy.
    bool needsExplicitCopy() const noexcept {
        return copy != CopyKind::BitCopy && copy != CopyKind::Reference;
    }

    /// True if the type owns resources that require release.
    ///
    /// Same predicate as needsDropForStorage. Two names for the same
    /// question: "is there anything to do when this value goes out of
    /// scope?" The name `ownsResources` reads better at some call
    /// sites (a fixed array's element check); `needsDropForStorage`
    /// reads better at others (the drop scheduler). Both are kept.
    bool ownsResources() const noexcept {
        return needsDropForStorage();
    }
};

/// @brief Classify a type. The classification is a pure function of
///        the TypeDescriptor; it does not depend on the code point.
ResourcePlan planForType(const TypeDescriptor& type);

/// @brief True if the plan requires a drop at scope exit.
bool needsScopeExitDrop(const ResourcePlan& plan) noexcept;

} // namespace lucid::contract::memory