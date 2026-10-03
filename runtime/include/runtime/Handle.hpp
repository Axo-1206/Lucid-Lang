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
 * the refcount transitions. This is what makes a host type's
 * copy/drop behavior host-defined (grammar §5.1's "Host type —
 * Opaque — Host-defined") without the interpreter knowing anything
 * about the host type.
 *
 * ─── Design: identity is the wrapper, not the payload ─────────────────────
 * Two HostHandles are == if they point at the same wrapper, not if
 * their payloads compare equal. The host can register a custom
 * equality on its type (for @primary columns, for array CONTAINS);
 * the runtime's default is wrapper identity.
 *
 * ─── Design: the payload is a void* the host owns ─────────────────────────
 * The runtime never dereferences the payload. It is the host's
 * responsibility to keep it valid until the release function pointer
 * is called. A host that needs a richer payload (a struct with
 * multiple fields) allocates it on the host side and stores its
 * pointer here.
 */

#pragma once

#include <cstdint>

namespace lucid::runtime {

class HostRegistry;  // forward; defined in HostRegistry.hpp

/// @brief A refcounted opaque host handle.
struct HostHandle {
    /// Reference count, managed by retainHandle / releaseHandle.
    uint32_t refcount;

    /// Index into the HostRegistry's type table. Identifies which host
    /// type this handle belongs to; used to find the type's retain /
    /// release / equality function pointers.
    uint32_t typeIndex;

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

/// @brief Increment the refcount. Also calls the host type's retain
///        function pointer, if it registered one.
void retainHandle(HostHandle* h) noexcept;

/// @brief Decrement the refcount. If it reaches zero, calls the host
///        type's release function pointer (if any) with the payload,
///        then frees the wrapper.
void releaseHandle(HostHandle* h) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Equality
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Compare two handles of the same host type.
///
/// Default: wrapper identity. If the host type registered an equality
/// function pointer, the runtime delegates to it.
///
/// Precondition: a and b have the same typeIndex.
bool handleEquals(const HostHandle* a, const HostHandle* b) noexcept;

} // namespace lucid::runtime