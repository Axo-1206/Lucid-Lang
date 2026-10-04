/**
 * @file runtime/Handle.cpp
 *
 * @responsibility The refcounted host handle implementation.
 *
 * ─── Design: the registry owns the callbacks, the handle carries the
 *                 type index ───────────────────────────────────────────────
 * A HostHandle is a small wrapper: a refcount, a type index into the
 * registry, and a payload pointer. The registry is where the retain /
 * release / equality function pointers live. The handle holds the
 * registry pointer it was created with (borrowed, not owned), so it
 * can find its callbacks on release.
 *
 * ─── Design: the payload is the host's; we never touch it ─────────────────
 * The runtime never dereferences `payload`. It passes it to the host's
 * retain / release / equals callbacks, which do all the work. If the
 * host registered no callbacks, the payload is leaked (or the host
 * manages it elsewhere); that is the host's choice.
 *
 * ─── Design: registry lifetime ────────────────────────────────────────────
 * The registry must outlive every handle created against it. In
 * practice, the host constructs one registry at engine startup and
 * keeps it alive for the process lifetime. A handle that outlives its
 * registry will call a dangling function pointer on release. This is
 * the same lifetime contract as the LoadedProgram and the interpreter.
 */

#include "runtime/Handle.hpp"
#include "runtime/HostRegistry.hpp"   // for the callbacks

#include <cstdlib>

namespace lucid::runtime {

HostHandle* allocHandle(HostRegistry* registry,
                        uint32_t typeIndex,
                        void* payload) {
    HostHandle* h = static_cast<HostHandle*>(std::malloc(sizeof(HostHandle)));
    if (!h) return nullptr;
    h->refcount = 1;
    h->typeIndex = typeIndex;
    h->registry = registry;
    h->payload = payload;
    return h;
}

void retainHandle(HostHandle* h) noexcept {
    if (!h) return;
    ++h->refcount;
    if (h->refcount == 2) {
        // First second-owner transition. Call the host type's retain
        // callback, if one is registered.
        if (h->registry) {
            if (const HostRegistry::TypeEntry* t =
                    h->registry->typeAt(h->typeIndex)) {
                if (t->retain) t->retain(h->payload);
            }
        }
    }
}

void releaseHandle(HostHandle* h) noexcept {
    if (!h) return;
    if (--h->refcount > 0) return;

    // Refcount reached zero. Call the host type's release callback.
    if (h->registry) {
        if (const HostRegistry::TypeEntry* t =
                h->registry->typeAt(h->typeIndex)) {
            if (t->release) t->release(h->payload);
        }
    }
    std::free(h);
}

bool handleEquals(const HostHandle* a, const HostHandle* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->typeIndex != b->typeIndex) return false;
    if (!a->registry) return false;

    if (const HostRegistry::TypeEntry* t =
            a->registry->typeAt(a->typeIndex)) {
        if (t->equals) return t->equals(a->payload, b->payload);
    }
    return false;
}

} // namespace lucid::runtime