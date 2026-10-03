/**
 * @file runtime/Array.hpp
 *
 * @responsibility A refcounted, growable array of Values. The heap
 *                 object a ValueTag::Array Value points at.
 *
 * ─── Design: dynamic and fixed-size share a representation ────────────────
 * A dynamic array [T] and a fixed-size array [N, T] have the same
 * buffer layout. The difference is policy: a fixed array's length is
 * set at construction and ADD / REMOVE / CLEAR are rejected (by the
 * compiler — Sema does not emit them; the runtime also asserts).
 * Keeping one representation lets Ext_NewArray and Ext_NewFixedArray
 * share code, and lets FreeArray handle both.
 *
 * ─── Design: element type is carried, not inferred ────────────────────────
 * An ArrayObject carries a pointer to its element's TypeDescriptor
 * (from bytecode/TypeDescriptor.hpp, forward-declared here). FreeArray
 * and CopyArray need it: dropping an array of strings releases each
 * element; dropping an array of ints does nothing. The interpreter
 * does not re-derive the element type from a tag — the array knows
 * its own element type.
 *
 * ─── Design: the buffer is a Value array ──────────────────────────────────
 * Elements are Values, not raw bytes. A Value is 16 bytes; the buffer
 * is a contiguous Value[N]. This is what makes indexing an O(1) load
 * with no element-type dispatch. The alternative (a typed byte buffer,
 * one buffer per element type) would complicate every operation for
 * a size saving that does not matter for a scripting language's arrays.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lucid::interp {
    struct Value;
}

namespace lucid::bytecode {
    struct TypeDescriptor;
}

namespace lucid::runtime {

using lucid::interp::Value;

/// @brief A refcounted array of Values.
struct ArrayObject {
    /// Reference count, managed by retainArray / releaseArray.
    uint32_t refcount;

    /// Number of live elements. Always <= capacity.
    uint32_t length;

    /// Allocated capacity in elements. May exceed length.
    uint32_t capacity;

    /// True if this is a fixed-size array ([N, T]). ADD / REMOVE /
    /// CLEAR are not valid on a fixed array; the runtime asserts if
    /// they are called.
    bool fixed;

    /// The element's TypeDescriptor. Used by copyArray and freeArray
    /// to copy or drop elements correctly. Never null.
    const bytecode::TypeDescriptor* elementType;

    /// The element buffer. Never null for a live array; the empty
    /// array points at a shared zero-length buffer.
    Value* data;
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Allocate a new empty dynamic array with the given element
///        type. Refcount 1.
ArrayObject* allocArray(const bytecode::TypeDescriptor* elementType);

/// @brief Allocate a new fixed-size array with the given element type
///        and length. All elements are Uninitialized; the caller must
///        fill them before the array is observable. Refcount 1.
ArrayObject* allocFixedArray(const bytecode::TypeDescriptor* elementType,
                             uint32_t length);

// ─────────────────────────────────────────────────────────────────────────────
// Refcounting
// ─────────────────────────────────────────────────────────────────────────────

void retainArray(ArrayObject* a) noexcept;

/// @brief Decrement the refcount. If it reaches zero, drop every live
///        element (release strings, release host handles, recurse into
///        nested arrays) and free the buffer and the ArrayObject.
void releaseArray(ArrayObject* a) noexcept;

/// @brief Allocate a fresh ArrayObject with the same elements as `a`,
///        refcount 1. Each element is copied according to its type
///        (a string element is retained, a nested array element is
///        deep-copied, a primitive element is bit-copied).
///
/// This is the runtime implementation of RuntimeOp::CopyArray.
ArrayObject* copyArray(const ArrayObject* a);

// ─────────────────────────────────────────────────────────────────────────────
// Mutation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Append a value. Precondition: !fixed. Grows the buffer if
///        needed. The value is moved in (the caller's slot is not
///        dropped; ownership transfers to the array).
///
/// This is the runtime implementation of Ext_ArrayAdd's backing store.
void arrayAdd(ArrayObject* a, Value v);

/// @brief Remove the element at `index`. Precondition: !fixed,
///        index < length. Later elements shift down by one; the
///        removed element is dropped. O(n).
///
/// This is the runtime implementation of Ext_ArrayRemove.
void arrayRemove(ArrayObject* a, uint32_t index);

/// @brief Remove every element and drop each. Precondition: !fixed.
///        Keeps the allocated capacity.
///
/// This is the runtime implementation of Ext_ArrayClear.
void arrayClear(ArrayObject* a) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

inline uint32_t arrayLength(const ArrayObject* a) noexcept { return a->length; }

/// @brief Read an element. Precondition: index < length.
inline const Value& arrayAt(const ArrayObject* a, uint32_t index) noexcept {
    return a->data[index];
}

/// @brief Write an element in place. The old value is dropped. Used
///        by StoreIndex.
void arraySet(ArrayObject* a, uint32_t index, Value v);

/// @brief Linear-scan contains. Uses the element type's equality
///        rule (primitive compare, string compare, row-ref identity,
///        host-type equality).
bool arrayContains(const ArrayObject* a, const Value& v) noexcept;

} // namespace lucid::runtime