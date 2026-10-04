/**
 * @file runtime/Handle.hpp
 *
 * @responsibility A refcounted opaque host handle. The heap object a
 *                 ValueTag::HostHandle Value points at.
 *
 * ─── Design: the runtime owns the refcount, the host owns the payload ─────
 * A HostHandle wraps whatever opaque value the host registered for a
 * host type (a SpriteRef, a LoadRequest, a Coroutine). The runtime
 * owns the refcount and the lifetime bookkeeping; the host provides
 * two function pointers (retain, release) that the runtime calls when
 * the refcount transitions. The registry is where those pointers
 * live; the handle carries the registry pointer (borrowed) so it can
 * find its callbacks on release.
 *
 * ─── Design: identity is the wrapper, not the payload ─────────────────────
 * Two HostHandles are == if the host type's equality callback says so,
 * or — for a type without a custom equality — if they point at the
 * same wrapper. The runtime's default is wrapper identity.
 *
 * ─── Design: the payload is a void* the host owns ─────────────────────────
 * The runtime never dereferences the payload. It is the host's
 * responsibility to keep it valid until the release function pointer
 * is called.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * None. HostRegistry is forward-declared; the .cpp includes the full
 * definition to call the callbacks.
 */

#pragma once

#include <cstdint>

namespace lucid::runtime {

class HostRegistry;  // forward; defined in HostRegistry.hpp

/// @brief A refcounted opaque host handle.
///
/// The `registry` pointer is borrowed, not owned. It must outlive the
/// handle. In practice the host constructs one registry at engine
/// startup and keeps it alive for the process lifetime, so this is
/// never a concern. A handle that outlives its registry will call a
/// dangling function pointer on release.
struct HostHandle {
    /// Reference count, managed by retainHandle / releaseHandle.
    uint32_t refcount;

    /// Index into the registry's type table. Identifies which host
    /// type this handle belongs to; used to find the type's retain /
    /// release / equality function pointers.
    uint32_t typeIndex;

    /// The registry this handle was created against. Borrowed, not
    /// owned. Used on release and on equality to find the type's
    /// callbacks.
    HostRegistry* registry;

    /// The opaque payload. The runtime never dereferences it. The
    /// host's release function is responsible for whatever cleanup
    /// the payload needs.
    void* payload;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Allocate a new HostHandle wrapping `payload` for the host
///        type at `typeIndex`. Refcount 1. The registry is borrowed,
///        not owned; it must outlive the handle.
HostHandle* allocHandle(HostRegistry* registry,
                        uint32_t typeIndex,
                        void* payload);

// ─────────────────────────────────────────────────────────────────────────────
// Refcounting
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Increment the refcount. On the first second-owner
///        transition, calls the host type's retain callback (if any).
void retainHandle(HostHandle* h) noexcept;

/// @brief Decrement the refcount. If it reaches zero, calls the host
///        type's release callback (if any) with the payload, then
///        frees the wrapper.
void releaseHandle(HostHandle* h) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Equality
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Compare two handles of the same host type.
///
/// If the host type registered an equality callback, delegates to it.
/// Otherwise, wrapper identity.
///
/// Precondition: a and b have the same typeIndex.
bool handleEquals(const HostHandle* a, const HostHandle* b) noexcept;

} // namespace lucid::runtime