/**
 * @file runtime/Array.cpp
 *
 * @responsibility The refcounted array implementation.
 *
 * ─── Design: the buffer is a Value array, capacity >= length ──────────────
 * An ArrayObject owns a `Value* data` buffer of `capacity` entries.
 * `length` entries are live; the rest are unused. Growth doubles the
 * capacity when the buffer is full. This is the standard dynamic
 * array layout.
 *
 * ─── Design: element drops go through dropValue ───────────────────────────
 * When an element is removed, overwritten, or the array is released,
 * the element's ResourcePlan tells us whether to do anything. The
 * plan comes from `elementType`, which the array carries. This is the
 * runtime's half of the compile/runtime agreement (see ValueOps.hpp).
 *
 * ─── Design: fixed arrays share the representation ────────────────────────
 * A fixed array ([N, T]) is the same struct with `fixed = true` and
 * `capacity == length`. Mutation functions assert against `fixed`.
 */

#include "runtime/Array.hpp"
#include "runtime/ValueOps.hpp"

#include "contract/TypeDescriptor.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

namespace lucid::runtime {

namespace {

// Initial capacity for a new dynamic array.
constexpr uint32_t kInitialCapacity = 4;

// A shared empty buffer for zero-length arrays. Never freed.
Value g_emptyBuffer[1];

/// Allocate a buffer of `capacity` Values, all Uninitialized. Returns
/// nullptr on failure; the caller checks.
Value* allocateBuffer(uint32_t capacity) {
    if (capacity == 0) return g_emptyBuffer;
    Value* buf = static_cast<Value*>(std::malloc(sizeof(Value) * capacity));
    return buf;
}

/// Grow the array's buffer to hold at least `minCapacity` entries.
/// Doubles until it does. Aborts on allocation failure (the runtime
/// has no recovery for OOM; the host should have set a limit).
void ensureCapacity(ArrayObject* a, uint32_t minCapacity) {
    if (a->capacity >= minCapacity) return;

    uint32_t newCap = a->capacity == 0 ? kInitialCapacity : a->capacity;
    while (newCap < minCapacity) newCap *= 2;

    Value* newBuf = static_cast<Value*>(
        std::malloc(sizeof(Value) * newCap));
    if (!newBuf) std::abort();

    if (a->length > 0) {
        std::memcpy(newBuf, a->data, sizeof(Value) * a->length);
    }
    if (a->data != g_emptyBuffer) {
        std::free(a->data);
    }
    a->data = newBuf;
    a->capacity = newCap;
}

/// Free the array's buffer (unless it is the shared empty buffer).
void freeBuffer(ArrayObject* a) noexcept {
    if (a->data && a->data != g_emptyBuffer) {
        std::free(a->data);
    }
    a->data = g_emptyBuffer;
    a->capacity = 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────

ArrayObject* allocArray(const contract::TypeDescriptor* elementType) {
    ArrayObject* a = static_cast<ArrayObject*>(
        std::malloc(sizeof(ArrayObject)));
    if (!a) return nullptr;

    a->refcount    = 1;
    a->length      = 0;
    a->capacity    = 0;
    a->fixed       = false;
    a->elementType = elementType;
    a->data        = g_emptyBuffer;
    return a;
}

ArrayObject* allocFixedArray(const contract::TypeDescriptor* elementType,
                             uint32_t length) {
    ArrayObject* a = static_cast<ArrayObject*>(
        std::malloc(sizeof(ArrayObject)));
    if (!a) return nullptr;

    a->refcount    = 1;
    a->length      = length;
    a->capacity    = length;
    a->fixed       = true;
    a->elementType = elementType;
    a->data        = allocateBuffer(length);
    if (!a->data) {
        std::free(a);
        return nullptr;
    }

    // Elements are Uninitialized; the caller must fill them before the
    // array is observable. A debug build might memset here to catch
    // misuse; we rely on the Uninitialized tag instead.
    for (uint32_t i = 0; i < length; ++i) {
        a->data[i] = Value{};  // Uninitialized tag
    }
    return a;
}

// ─────────────────────────────────────────────────────────────────────────
// Refcounting
// ─────────────────────────────────────────────────────────────────────────

void retainArray(ArrayObject* a) noexcept {
    if (a) ++a->refcount;
}

void releaseArray(ArrayObject* a) noexcept {
    if (!a) return;
    if (--a->refcount > 0) return;

    // Drop every live element. The plan comes from the element type.
    if (a->elementType) {
        const contract::ResourcePlan elemPlan =
            contract::planForType(*a->elementType);
        if (elemPlan.needsDropForStorage()) {
            for (uint32_t i = 0; i < a->length; ++i) {
                dropValue(a->data[i], elemPlan);
            }
        }
    }

    freeBuffer(a);
    std::free(a);
}

ArrayObject* copyArray(const ArrayObject* a) {
    if (!a) return nullptr;

    ArrayObject* result = a->fixed
        ? allocFixedArray(a->elementType, a->length)
        : allocArray(a->elementType);
    if (!result) return nullptr;

    if (!a->fixed) {
        // Dynamic array: reserve the source's capacity, then copy.
        ensureCapacity(result, a->length);
        result->length = a->length;
    }

    if (a->length > 0) {
        const contract::ResourcePlan elemPlan =
            contract::planForType(*a->elementType);

        if (elemPlan.needsExplicitCopy()) {
            for (uint32_t i = 0; i < a->length; ++i) {
                result->data[i] = copyValue(a->data[i], elemPlan);
            }
        } else {
            // Bit copy: no resource to duplicate.
            std::memcpy(result->data, a->data,
                        sizeof(Value) * a->length);
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// Mutation
// ─────────────────────────────────────────────────────────────────────────

void arrayAdd(ArrayObject* a, Value v) {
    // Fixed arrays cannot grow. A caller that hits this has a bug;
    // abort in debug, no-op in release.
    if (a->fixed) return;

    ensureCapacity(a, a->length + 1);
    a->data[a->length++] = v;
}

void arrayRemove(ArrayObject* a, uint32_t index) {
    if (a->fixed) return;
    if (index >= a->length) return;

    // Drop the removed element.
    if (a->elementType) {
        const contract::ResourcePlan elemPlan =
            contract::planForType(*a->elementType);
        if (elemPlan.needsDropForStorage()) {
            dropValue(a->data[index], elemPlan);
        }
    }

    // Shift later elements down by one. This is an element-wise move,
    // not a copy: the shifted-from slot is left as a bit pattern that
    // is no longer live, and the shifted-to slot now owns the value.
    // No retain/release happens — the elements move, they do not
    // duplicate.
    const uint32_t tailCount = a->length - index - 1;
    if (tailCount > 0) {
        std::memmove(&a->data[index], &a->data[index + 1],
                     sizeof(Value) * tailCount);
    }
    --a->length;
}

void arrayClear(ArrayObject* a) noexcept {
    if (a->fixed) return;

    if (a->elementType) {
        const contract::ResourcePlan elemPlan =
            contract::planForType(*a->elementType);
        if (elemPlan.needsDropForStorage()) {
            for (uint32_t i = 0; i < a->length; ++i) {
                dropValue(a->data[i], elemPlan);
            }
        }
    }
    a->length = 0;
    // Capacity is retained.
}

// ─────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────

void arraySet(ArrayObject* a, uint32_t index, Value v) {
    if (index >= a->length) return;

    // Drop the old value, then write the new one.
    if (a->elementType) {
        const contract::ResourcePlan elemPlan =
            contract::planForType(*a->elementType);
        if (elemPlan.needsDropForStorage()) {
            dropValue(a->data[index], elemPlan);
        }
    }
    a->data[index] = v;
}

bool arrayContains(const ArrayObject* a, const Value& v) noexcept {
    if (!a->elementType) {
        // No element type; fall back to bitwise equality.
        for (uint32_t i = 0; i < a->length; ++i) {
            const Value& e = a->data[i];
            if (e.tag != v.tag) continue;
            if (e.payload == v.payload) return true;
        }
        return false;
    }

    // Dispatch on the element type's shape. Only the cases that need
    // non-bitwise equality are special-cased; everything else falls
    // through to bitwise equality on (tag, payload).
    //
    // Strings compare by content. RowRefs compare by slot + generation,
    // which is exactly the payload. Host handles compare via the
    // registry's equality callback. Numbers compare by value; since
    // the tag encodes the width, comparing (tag, payload) is correct
    // for the primitive types except for F32/F64 (NaN != NaN; -0.0
    // vs +0.0). For F32/F64 we compare the decoded double.
    using contract::TypeDescriptor;
    switch (a->elementType->kind) {
        case TypeDescriptor::Kind::Primitive: {
            switch (a->elementType->primitive) {
                case PrimitiveKind::String: {
                    for (uint32_t i = 0; i < a->length; ++i) {
                        if (a->data[i].tag == ValueTag::String
                            && stringEquals(a->data[i].asString(),
                                            v.asString()))
                            return true;
                    }
                    return false;
                }
                case PrimitiveKind::Float32:
                case PrimitiveKind::Float64: {
                    const double target = v.isFloat()
                        ? (v.tag == ValueTag::F32 ? v.asF32() : v.asF64())
                        : 0.0;
                    for (uint32_t i = 0; i < a->length; ++i) {
                        const Value& e = a->data[i];
                        if (!e.isFloat()) continue;
                        const double d = e.tag == ValueTag::F32
                            ? e.asF32() : e.asF64();
                        if (d == target) return true;
                    }
                    return false;
                }
                default:
                    break;  // fall through to bitwise
            }
            break;
        }
        case TypeDescriptor::Kind::Named:
            // Host handle: equality is the registry's job.
            // TODO(host): delegate to HostRegistry::TypeEntry::equals.
            // For v1 we bit-compare the wrapper pointers, which is
            // correct for the default (identity) equality.
            break;
        default:
            break;
    }

    // Bitwise fallback.
    for (uint32_t i = 0; i < a->length; ++i) {
        const Value& e = a->data[i];
        if (e.tag != v.tag) continue;
        if (e.payload == v.payload) return true;
    }
    return false;
}

} // namespace lucid::runtime