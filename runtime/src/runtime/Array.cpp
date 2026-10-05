/**
 * @file runtime/Array.cpp
 *
 * @responsibility The refcounted array implementation.
 *
 * ─── Design: the buffer is a Value array, capacity >= length ──────────────
 * An ArrayObject owns a `Value* data` buffer of `capacity` entries.
 * `length` entries are live; the rest are unused. Growth doubles the
 * capacity when the buffer is full.
 *
 * ─── Design: element drops go through dropValue ───────────────────────────
 * When an element is removed, overwritten, or the array is released,
 * the element's ResourcePlan tells us whether to do anything. The
 * plan comes from `elementType`, which the array carries.
 *
 * ─── Design: fixed arrays share the representation ────────────────────────
 * A fixed array ([N, T]) is the same struct with `fixed = true` and
 * `capacity == length`. Mutation functions assert against `fixed`.
 *
 * ─── Design: arrayContains dispatches on the element type ─────────────────
 * Containment is a linear scan with a per-element equality check. The
 * equality rule depends on the element type: bit equality for
 * primitives (with NaN and -0.0 handled specially for floats),
 * content equality for strings, wrapper equality (or a host-registered
 * callback) for host handles, payload equality for row references and
 * function values. The host-handle case is resolved by calling
 * handleEquals, which looks up the type's callback via the handle's
 * own registry pointer — no registry parameter is needed.
 *
 * ─── Note: no SORT here ───────────────────────────────────────────────────
 * The array's SORT method takes a comparator, and the comparator is a
 * Lucid function value — calling it means going through the
 * interpreter's dispatch, which the runtime cannot do. The sort lives
 * in the interpreter (interp/Ops/OpsAggregate.cpp). The runtime
 * provides only the buffer operations it needs (arrayAt, arraySet,
 * arraySwap). See grammar §8.3.
 */

#include "runtime/Array.hpp"
#include "runtime/Handle.hpp"
#include "runtime/String.hpp"
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

/// Allocate a buffer of `capacity` Values. Returns the shared empty
/// buffer for capacity 0. Returns nullptr on allocation failure.
Value* allocateBuffer(uint32_t capacity) {
    if (capacity == 0) return g_emptyBuffer;
    Value* buf = static_cast<Value*>(std::malloc(sizeof(Value) * capacity));
    return buf;
}

/// Grow the array's buffer to hold at least `minCapacity` entries.
/// Doubles until it does. Aborts on allocation failure; the runtime
/// has no recovery for OOM.
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

/// Bitwise equality on (tag, payload). Used for element types whose
/// equality is identity.
inline bool valueBitEquals(const Value& a, const Value& b) noexcept {
    return a.tag == b.tag && a.payload == b.payload;
}

/// True if two numeric Values (same type) are numerically equal.
/// Handles the F32/F64 NaN and signed-zero cases.
inline bool numericEquals(const Value& a, const Value& b) noexcept {
    if (a.isFloat()) {
        const double da = (a.tag == ValueTag::F32) ? a.asF32() : a.asF64();
        const double db = (b.tag == ValueTag::F32) ? b.asF32() : b.asF64();
        return da == db;
    }
    return a.payload == b.payload;
}

/// Equality for two Values of the same declared element type.
/// `elemType` is the array's element TypeDescriptor.
bool valueEqualsForType(const Value& a, const Value& b,
                        const contract::TypeDescriptor& elemType) noexcept {
    using contract::TypeDescriptor;

    switch (elemType.kind) {
        case TypeDescriptor::Kind::Primitive: {
            switch (elemType.primitive) {
                case PrimitiveKind::String:
                    if (a.tag != ValueTag::String ||
                        b.tag != ValueTag::String) return false;
                    return stringEquals(a.asString(), b.asString());

                case PrimitiveKind::Float32:
                case PrimitiveKind::Float64:
                    if (!a.isFloat() || !b.isFloat()) return false;
                    return numericEquals(a, b);

                default:
                    // Bool, Char, all integer widths: (tag, payload)
                    // equality is exactly value equality.
                    return valueBitEquals(a, b);
            }
        }

        case TypeDescriptor::Kind::Named:
            // A Named element type at the runtime layer is a host
            // type (opaque handle). A bare table type is never an
            // element type — the grammar rejects it.
            if (a.tag != ValueTag::HostHandle ||
                b.tag != ValueTag::HostHandle) return false;
            return handleEquals(a.asHostHandle(), b.asHostHandle());

        case TypeDescriptor::Kind::RowRef:
            // Two row references are equal if they name the same row
            // (same slot, same generation). The payload encodes both.
            if (a.tag != ValueTag::RowRef ||
                b.tag != ValueTag::RowRef) return false;
            return a.payload == b.payload;

        case TypeDescriptor::Kind::Function:
            // Function values are compile-time-known code addresses.
            // Two function values are equal if they are the same
            // FunctionRef.
            if (a.tag != ValueTag::Function ||
                b.tag != ValueTag::Function) return false;
            return a.payload == b.payload;

        case TypeDescriptor::Kind::Array:
            // Arrays are compared structurally by the language, but
            // at the runtime layer, containment of an array in
            // another array is by reference (the same ArrayObject).
            if (a.tag != ValueTag::Array ||
                b.tag != ValueTag::Array) return false;
            return a.payload == b.payload;

        case TypeDescriptor::Kind::Nullable:
            // A nullable element type: nil compares equal to nil;
            // non-nil compares per the inner type.
            if (a.isNil() || b.isNil()) {
                return a.isNil() && b.isNil();
            }
            if (!elemType.component) return false;
            return valueEqualsForType(a, b, *elemType.component);

        case TypeDescriptor::Kind::Unknown:
            return valueBitEquals(a, b);
    }
    return valueBitEquals(a, b);
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
    if (!a->data && length > 0) {
        std::free(a);
        return nullptr;
    }

    // Elements start Uninitialized; the caller must fill them before
    // the array is observable. A debug build could assert on read;
    // we rely on the Uninitialized tag being handled by the caller.
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
        ensureCapacity(result, a->length);
        result->length = a->length;
    }

    if (a->length > 0 && a->elementType) {
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
    // not a copy: no retain/release happens.
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
    if (a->length == 0) return false;

    if (!a->elementType) {
        for (uint32_t i = 0; i < a->length; ++i) {
            if (valueBitEquals(a->data[i], v)) return true;
        }
        return false;
    }

    for (uint32_t i = 0; i < a->length; ++i) {
        if (valueEqualsForType(a->data[i], v, *a->elementType)) {
            return true;
        }
    }
    return false;
}

} // namespace lucid::runtime