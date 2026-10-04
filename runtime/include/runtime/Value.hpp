/**
 * @file runtime/Value.hpp
 *
 * @responsibility The runtime value representation. A Value is a
 *                 16-byte tagged union: an 8-byte discriminant and an
 *                 8-byte payload.
 *
 * ─── Design: tagged union, not NaN-boxing ─────────────────────────────────
 * A Value is 16 bytes: { ValueTag tag; uint64_t payload; }. The tag
 * names the type; the payload is reinterpreted according to the tag.
 * Portable to any 64-bit target, debuggable in a watch window, and
 * lets the interpreter's dispatch read one word to decide what it is.
 * NaN-boxing would halve the size but only works for 64-bit and makes
 * aggregates out-of-line anyway.
 *
 * ─── Design: aggregates are heap objects ──────────────────────────────────
 * Strings, arrays, tables, and host handles live on the heap. A Value
 * holds a pointer to the heap object, not the object inline. This
 * keeps Value fixed-size regardless of what it holds, which is what
 * makes the operand stack a simple contiguous array of Values.
 *
 * ─── Design: row references are not pointers ──────────────────────────────
 * A row reference is a {slot, generation} pair (grammar §4.1.1a), not
 * a memory address. A &T Value packs both into the payload: the low 32
 * bits are the slot, the high 32 bits are the generation.
 *
 * ─── Design: nil ──────────────────────────────────────────────────────────
 * nil is a Value with tag Nil. It is a valid value of any &T type and
 * of any T? type (grammar §5.2, §5.3).
 *
 * ─── Why this lives in runtime/ ───────────────────────────────────────────
 * A Value is a runtime representation. The interpreter's operand stack
 * and locals hold Values; the runtime's ArrayObject buffer holds
 * Values; the runtime's dropValue / copyValue operate on Values. Both
 * libraries need the type, and the runtime is the lower of the two.
 *
 * ─── Design: interpreter-owned concepts are forward-declared ─────────────
 * TableObject and FunctionRef are interpreter types. Value holds
 * pointers to them, but the runtime never dereferences those pointers
 * — the runtime's dropValue and copyValue never touch a table or a
 * function. Forward declarations are enough; the interpreter's own
 * .cpp files include the full definitions where they are needed.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * None. Every pointer type is forward-declared. Value is the bottom
 * of the runtime's include graph.
 */

#pragma once

#include <cstdint>
#include <cstring>

// Forward declarations — Value holds pointers, not definitions.
namespace lucid::interp {
    struct TableObject;
    struct FunctionRef;
}

namespace lucid::runtime {

// Forward declarations — same namespace, definitions come later.
struct StringObject;
struct ArrayObject;
struct HostHandle;

// ─────────────────────────────────────────────────────────────────────────────
// ValueTag
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The discriminant of a Value.
///
/// Values are grouped by kind. The numeric values are stable and are
/// part of the .lucb compatibility contract where they appear in
/// serialized constants — they must not be reordered; new tags are
/// added at the end.
enum class ValueTag : uint8_t {
    // Primitives (inline in the payload)
    Nil         = 0x00,  ///< no payload
    Bool        = 0x01,  ///< payload = 0 or 1
    Char        = 0x02,  ///< payload = code point (Unicode scalar value)
    I8          = 0x03,  ///< payload = sign-extended int8
    I16         = 0x04,  ///< payload = sign-extended int16
    I32         = 0x05,  ///< payload = sign-extended int32
    I64         = 0x06,  ///< payload = int64 bits
    U8          = 0x07,  ///< payload = zero-extended uint8
    U16         = 0x08,  ///< payload = zero-extended uint16
    U32         = 0x09,  ///< payload = zero-extended uint32
    U64         = 0x0A,  ///< payload = uint64 bits
    F32         = 0x0B,  ///< payload = low 32 bits are the float bits
    F64         = 0x0C,  ///< payload = double bits

    // References (pointers to heap objects)
    String      = 0x10,  ///< payload = StringObject*
    Array       = 0x11,  ///< payload = ArrayObject*
    Table       = 0x12,  ///< payload = interp::TableObject*
    RowRef      = 0x13,  ///< payload = packed {generation:32 | slot:32}
    Function    = 0x14,  ///< payload = interp::FunctionRef*
    HostHandle  = 0x15,  ///< payload = HostHandle*
    ColumnView  = 0x16,  ///< payload = ColumnView* (transient, §5.6)

    /// Internal: a slot that has been allocated but not yet written.
    /// Never observable to a Lucid program — the interpreter asserts
    /// it is never read. Its presence makes an uninitialized slot
    /// detectable in debug builds.
    Uninitialized = 0x7F,
};

// ─────────────────────────────────────────────────────────────────────────────
// Value
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A runtime value.
///
/// 16 bytes: one tag byte (with 7 bytes of padding for alignment) and
/// an 8-byte payload. The padding is deliberate — it makes the struct
/// trivially copyable and keeps the payload 8-byte aligned without
/// packing tricks, which would slow down every load on some targets.
///
/// A Value is trivially copyable: copying it does not retain anything.
/// Retaining and releasing are explicit, done by the interpreter when
/// the compiled code says to.
struct Value {
    ValueTag tag;
    uint8_t  _pad[7];
    uint64_t payload;

    // ─── Constructors ───────────────────────────────────────────────────

    Value() noexcept : tag(ValueTag::Uninitialized), _pad{}, payload(0) {}

    static Value makeNil() noexcept {
        Value v; v.tag = ValueTag::Nil; v.payload = 0; return v;
    }
    static Value makeBool(bool b) noexcept {
        Value v; v.tag = ValueTag::Bool; v.payload = b ? 1u : 0u; return v;
    }
    static Value makeChar(uint32_t cp) noexcept {
        Value v; v.tag = ValueTag::Char; v.payload = cp; return v;
    }
    static Value makeI8(int8_t x) noexcept {
        Value v; v.tag = ValueTag::I8;
        v.payload = static_cast<uint64_t>(static_cast<int64_t>(x));
        return v;
    }
    static Value makeI16(int16_t x) noexcept {
        Value v; v.tag = ValueTag::I16;
        v.payload = static_cast<uint64_t>(static_cast<int64_t>(x));
        return v;
    }
    static Value makeI32(int32_t x) noexcept {
        Value v; v.tag = ValueTag::I32;
        v.payload = static_cast<uint64_t>(static_cast<int64_t>(x));
        return v;
    }
    static Value makeI64(int64_t x) noexcept {
        Value v; v.tag = ValueTag::I64;
        v.payload = static_cast<uint64_t>(x);
        return v;
    }
    static Value makeU8(uint8_t x) noexcept {
        Value v; v.tag = ValueTag::U8; v.payload = x; return v;
    }
    static Value makeU16(uint16_t x) noexcept {
        Value v; v.tag = ValueTag::U16; v.payload = x; return v;
    }
    static Value makeU32(uint32_t x) noexcept {
        Value v; v.tag = ValueTag::U32; v.payload = x; return v;
    }
    static Value makeU64(uint64_t x) noexcept {
        Value v; v.tag = ValueTag::U64; v.payload = x; return v;
    }
    static Value makeF32(float x) noexcept {
        Value v; v.tag = ValueTag::F32;
        uint32_t bits; std::memcpy(&bits, &x, sizeof bits);
        v.payload = bits; return v;
    }
    static Value makeF64(double x) noexcept {
        Value v; v.tag = ValueTag::F64;
        uint64_t bits; std::memcpy(&bits, &x, sizeof bits);
        v.payload = bits; return v;
    }

    static Value makeString(StringObject* s) noexcept {
        Value v; v.tag = ValueTag::String;
        v.payload = reinterpret_cast<uint64_t>(s);
        return v;
    }
    static Value makeArray(ArrayObject* a) noexcept {
        Value v; v.tag = ValueTag::Array;
        v.payload = reinterpret_cast<uint64_t>(a);
        return v;
    }
    static Value makeTable(interp::TableObject* t) noexcept {
        Value v; v.tag = ValueTag::Table;
        v.payload = reinterpret_cast<uint64_t>(t);
        return v;
    }
    static Value makeRowRef(uint32_t slot, uint32_t generation) noexcept {
        Value v; v.tag = ValueTag::RowRef;
        v.payload = (static_cast<uint64_t>(generation) << 32) | slot;
        return v;
    }
    static Value makeFunction(interp::FunctionRef* f) noexcept {
        Value v; v.tag = ValueTag::Function;
        v.payload = reinterpret_cast<uint64_t>(f);
        return v;
    }
    static Value makeHostHandle(HostHandle* h) noexcept {
        Value v; v.tag = ValueTag::HostHandle;
        v.payload = reinterpret_cast<uint64_t>(h);
        return v;
    }
    static Value makeColumnView(void* cv) noexcept {
        Value v; v.tag = ValueTag::ColumnView;
        v.payload = reinterpret_cast<uint64_t>(cv);
        return v;
    }

    // ─── Accessors (no validation — the caller checked the tag) ─────────

    bool     asBool() const noexcept { return payload != 0; }
    uint32_t asChar() const noexcept { return static_cast<uint32_t>(payload); }
    int8_t   asI8()   const noexcept { return static_cast<int8_t>(payload); }
    int16_t  asI16()  const noexcept { return static_cast<int16_t>(payload); }
    int32_t  asI32()  const noexcept { return static_cast<int32_t>(payload); }
    int64_t  asI64()  const noexcept { return static_cast<int64_t>(payload); }
    uint8_t  asU8()   const noexcept { return static_cast<uint8_t>(payload); }
    uint16_t asU16()  const noexcept { return static_cast<uint16_t>(payload); }
    uint32_t asU32()  const noexcept { return static_cast<uint32_t>(payload); }
    uint64_t asU64()  const noexcept { return payload; }
    float    asF32()  const noexcept {
        uint32_t bits = static_cast<uint32_t>(payload);
        float x; std::memcpy(&x, &bits, sizeof x); return x;
    }
    double   asF64()  const noexcept {
        uint64_t bits = payload;
        double x; std::memcpy(&x, &bits, sizeof x); return x;
    }

    StringObject* asString() const noexcept {
        return reinterpret_cast<StringObject*>(payload);
    }
    ArrayObject* asArray() const noexcept {
        return reinterpret_cast<ArrayObject*>(payload);
    }
    interp::TableObject* asTable() const noexcept {
        return reinterpret_cast<interp::TableObject*>(payload);
    }
    uint32_t rowSlot() const noexcept {
        return static_cast<uint32_t>(payload & 0xFFFFFFFFu);
    }
    uint32_t rowGeneration() const noexcept {
        return static_cast<uint32_t>(payload >> 32);
    }
    interp::FunctionRef* asFunction() const noexcept {
        return reinterpret_cast<interp::FunctionRef*>(payload);
    }
    HostHandle* asHostHandle() const noexcept {
        return reinterpret_cast<HostHandle*>(payload);
    }
    void* asColumnView() const noexcept {
        return reinterpret_cast<void*>(payload);
    }

    // ─── Predicates ─────────────────────────────────────────────────────

    bool isNil() const noexcept { return tag == ValueTag::Nil; }

    bool isNumeric() const noexcept {
        switch (tag) {
            case ValueTag::I8: case ValueTag::I16:
            case ValueTag::I32: case ValueTag::I64:
            case ValueTag::U8: case ValueTag::U16:
            case ValueTag::U32: case ValueTag::U64:
            case ValueTag::F32: case ValueTag::F64:
                return true;
            default: return false;
        }
    }

    bool isInteger() const noexcept {
        switch (tag) {
            case ValueTag::I8: case ValueTag::I16:
            case ValueTag::I32: case ValueTag::I64:
            case ValueTag::U8: case ValueTag::U16:
            case ValueTag::U32: case ValueTag::U64:
                return true;
            default: return false;
        }
    }

    bool isFloat() const noexcept {
        return tag == ValueTag::F32 || tag == ValueTag::F64;
    }

    bool isReference() const noexcept {
        switch (tag) {
            case ValueTag::String: case ValueTag::Array:
            case ValueTag::Table: case ValueTag::RowRef:
            case ValueTag::Function: case ValueTag::HostHandle:
            case ValueTag::ColumnView:
                return true;
            default: return false;
        }
    }
};

static_assert(sizeof(Value) == 16, "Value must be 16 bytes");
static_assert(alignof(Value) == 8, "Value must be 8-byte aligned");

} // namespace lucid::runtime