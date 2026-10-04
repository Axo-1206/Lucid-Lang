/**
 * @file interp/FunctionRef.hpp
 *
 * @responsibility The runtime representation of a function value — a
 *                 compile-time-known code address (an index into the
 *                 Bytecode's FunctionProto array) plus its signature.
 *
 * ─── Design: a function value is a code address, not a closure ────────────
 * The grammar is explicit: no captures, no currying, no closures
 * (§4.2.5). A function value is a bare code pointer. The interpreter
 * represents it as a FunctionRef*: a small heap object holding the
 * function index (into the LoadedProgram's FunctionProto array) and
 * the signature descriptor, so a call through a table cell can check
 * the argument types at the call site.
 *
 * ─── Design: why heap, not inline ─────────────────────────────────────────
 * A FunctionRef is small (a u32 index + a u32 signature-pool index),
 * but a Value's payload is only 8 bytes and already holds one pointer.
 * Pointing at a FunctionRef lets the LoadedProgram own a stable array
 * of them, indexed by function index — every LoadConst for a function
 * value pushes the same FunctionRef* for the same function. This makes
 * function values cheap to compare (pointer equality).
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * None.
 */

#pragma once

#include <cstdint>

namespace lucid::interp {

/// @brief A reference to a specific function in the loaded program.
struct FunctionRef {
    /// Index into the LoadedProgram's FunctionProto array. For a host
    /// function, this is the host symbol index into the dispatch array.
    uint32_t functionIndex;

    /// Index into the LoadedProgram's signature table. The signature
    /// is a contract::FunctionSignature: parameter types and return
    /// type. Used at a runtime-dispatched call site to check arguments.
    uint32_t signatureIndex;

    /// True if this is a host function (Ext_CallHost) rather than a
    /// Lucid function (Ext_Call).
    bool isHost;
};

} // namespace lucid::interp