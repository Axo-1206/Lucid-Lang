/**
 * @file interp/OpHelpers.hpp
 *
 * @responsibility Reading operands from a frame's code stream.
 *
 * ─── Design: little-endian, matching the serializer ───────────────────────
 * The compiler's Serialize.cpp writes multi-byte operands
 * little-endian. These helpers read them the same way. A byte-order
 * mismatch would be a cross-platform bug; the format version pins
 * the byte order.
 *
 * ─── Design: each helper advances the frame's ip ──────────────────────────
 * readU8/readU16/readU32/readI32 read at the frame's current ip and
 * advance it past the operand. The handler that calls them does not
 * have to track its own offset.
 */

#pragma once

#include "interp/Frame.hpp"

#include "bytecode/FunctionProto.hpp"
#include "runtime/Exceptions.hpp"

#include <cstdint>

namespace lucid::interp {

inline uint8_t readU8(Frame& f) {
    const uint8_t* code = f.proto()->code().data();
    const uint8_t v = code[f.ip()];
    f.advanceIp(1);
    return v;
}

inline uint16_t readU16(Frame& f) {
    const uint8_t* code = f.proto()->code().data();
    const uint32_t ip = f.ip();
    const uint16_t v = static_cast<uint16_t>(code[ip])
                     | (static_cast<uint16_t>(code[ip + 1]) << 8);
    f.advanceIp(2);
    return v;
}

inline uint32_t readU32(Frame& f) {
    const uint8_t* code = f.proto()->code().data();
    const uint32_t ip = f.ip();
    const uint32_t v = static_cast<uint32_t>(code[ip])
                     | (static_cast<uint32_t>(code[ip + 1]) << 8)
                     | (static_cast<uint32_t>(code[ip + 2]) << 16)
                     | (static_cast<uint32_t>(code[ip + 3]) << 24);
    f.advanceIp(4);
    return v;
}

inline int32_t readI32(Frame& f) {
    return static_cast<int32_t>(readU32(f));
}

/// Run a runtime-library call that may throw a runtime::RuntimeException,
/// converting the exception to a PanicException. The interpreter's
/// opcode handlers wrap every call into a throwing runtime function
/// (TableObject::addRow, TableObject::setCell) with this, so the
/// panic path is uniform.
template <typename Fn>
auto callRuntime(Fn&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const runtime::RuntimeException& e) {
        throw PanicException(e.code(), e.what());
    }
}

} // namespace lucid::interp