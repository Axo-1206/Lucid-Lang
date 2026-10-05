/**
 * @file interp/Frame.cpp
 *
 * @responsibility The Frame's constructor and destructor.
 *
 * ─── Design: the frame sizes itself from the FunctionProto ────────────────
 * The compiler's SlotAllocator assigned each local and parameter a slot
 * index; the compiler's CompilerContext tracked the maximum operand
 * stack depth. Both numbers end up on the FunctionProto. The Frame
 * reads them and allocates the two vectors.
 *
 * ─── Design: no interpretation of the function's code ─────────────────────
 * Frame's constructor does not touch the code bytes. It allocates the
 * storage the interpreter will use while executing those bytes. The
 * interpreter sets `ip` and fills in parameters before entering the
 * dispatch loop.
 *
 * ─── Design: locals and stack start Uninitialized ─────────────────────────
 * A Value's default constructor produces the Uninitialized tag. The
 * interpreter asserts, in debug builds, that a slot is never read
 * before it is written. This is what makes "uninitialized read" a
 * detectable bug rather than a silent use of a stale value.
 */

#include "interp/Frame.hpp"

#include "bytecode/FunctionProto.hpp"

#include <utility>

namespace lucid::interp {

Frame::Frame(const bytecode::FunctionProto* proto, uint32_t callDepth)
    : m_proto(proto)
    , m_callDepth(callDepth)
{
    // The proto is required. A frame without a function to execute is
    // a caller bug; the interpreter always has one.
    //
    // The compiler guarantees maxStackDepth >= 1 (see
    // FunctionProto::checkInvariants). A function that produces no
    // value still needs room for a scratch slot during emission.

    const uint32_t localCount = proto ? proto->localSlots() : 0;
    const uint32_t stackDepth = proto ? proto->maxStackDepth() : 1;

    // Both vectors are sized once, at construction. They are never
    // resized — a function's local slot count and maximum operand
    // stack depth are properties of the compiled function, not of a
    // particular call. This is what lets the interpreter treat
    // `m_locals` and `m_stack` as fixed-size arrays indexed by the
    // operands in the code stream.
    m_locals.resize(localCount);
    m_stack.resize(stackDepth);

    // The defaults are Uninitialized Values. m_ip and m_sp are zero.
}

} // namespace lucid::interp