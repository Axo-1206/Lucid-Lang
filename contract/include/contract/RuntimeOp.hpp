/**
 * @file contract/RuntimeOp.hpp
 *
 * @responsibility The closed set of runtime operations the interpreter
 *                 implements on behalf of the emitted code. Each
 *                 runtime call is emitted as `Ext_RtCall <RuntimeOp>`,
 *                 where the operand is one of RuntimeOp's values.
 *
 * ─── Design: one opcode, one mechanism ────────────────────────────────────
 * Every runtime operation goes through Ext_RtCall. There are no
 * separate opcodes for retain, release, string copy, or any other
 * runtime operation. The opcode selects which operation; the
 * RuntimeOp enum names them.
 *
 * ─── Design: a closed enum ────────────────────────────────────────────────
 * Runtime operations are the language's own operations. They are not
 * user-extensible (that's the host's job, through HostSymbolTable).
 * A closed enum with fixed values is enough.
 *
 * ─── Design: values are the wire format ───────────────────────────────────
 * The enum's underlying values are what appear in the bytecode as the
 * Ext_RtCall operand. They must not be reordered; new operations are
 * added at the end. This is a stability contract with the .lucb
 * format (see Serialize.hpp's format version).
 */

#pragma once

#include <cstdint>

namespace lucid::contract {

/// @brief One runtime operation the interpreter implements.
enum class RuntimeOp : uint8_t {
    /// Retain a host handle (increment its refcount). No stack change:
    /// the handle stays on the stack.
    Retain = 0x01,

    /// Release a host handle (decrement its refcount). Pops the handle.
    Release = 0x02,

    /// Deep-copy a string. Pops the string, pushes a fresh string
    /// whose buffer is a new allocation.
    CopyString = 0x03,

    /// Free a string's heap buffer. Pops the string.
    FreeString = 0x04,

    /// Deep-copy a dynamic array. Pops the array, pushes a fresh array
    /// whose buffer is a new allocation. Resource-typed elements are
    /// copied element-wise.
    CopyArray = 0x05,

    /// Free a dynamic array's heap buffer. Pops the array. Resource-
    /// typed elements are dropped element-wise first.
    FreeArray = 0x06,

    /// Concatenate two strings. Pops two strings, pushes a fresh
    /// string whose buffer holds the concatenation.
    ConcatString = 0x07,

    /// Raise a panic with a message. Pops the message string. Does not
    /// return; the interpreter unwinds to the host boundary.
    Panic = 0x08,
};

/// @brief Static information about a runtime operation.
struct RuntimeOpInfo {
    RuntimeOp   op;
    const char* name;
    int8_t      pops;
    int8_t      pushes;
};

/// @brief Look up a runtime operation's info. Precondition: the value
///        is a valid RuntimeOp.
const RuntimeOpInfo& runtimeOpInfo(RuntimeOp op) noexcept;

/// @brief True if the byte is a valid RuntimeOp value.
bool isRuntimeOp(uint8_t byte) noexcept;

} // namespace lucid::contract