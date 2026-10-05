/**
 * @file interp/Ops/OpsRuntime.cpp
 *
 * @responsibility The Ext_RtCall sub-dispatch. Ext_RtCall is one
 *                 opcode whose operand selects a RuntimeOp; this file
 *                 decodes the operand and calls into the runtime
 *                 library.
 *
 * ─── Design: no plan lookup here ──────────────────────────────────────────
 * The compiler already decided which RuntimeOp to emit for each
 * value's type (via contract::planForType). The interpreter trusts
 * the compiler's choice and executes the op. It does not re-derive
 * the plan.
 *
 * ─── Design: the runtime library's operations are the implementations ─────
 * runtime::retainString, runtime::releaseString, etc. are the actual
 * work. This file is the marshalling layer: pop the operands from the
 * frame's stack, call the runtime function, push the result.
 */

#include "Ops.hpp"
#include "OpHelpers.hpp"

#include "interp/Frame.hpp"
#include "interp/InterpreterInternal.hpp"

#include "contract/Opcode.hpp"
#include "contract/RuntimeOp.hpp"

#include "core/diagnostics/DiagCode.hpp"

#include "interp/InterpreterError.hpp"

#include "runtime/Array.hpp"
#include "runtime/Handle.hpp"
#include "runtime/String.hpp"
#include "runtime/Value.hpp"
#include "runtime/ValueOps.hpp"

#include <cstdint>

namespace lucid::interp {

void opsRuntime(InterpreterInternal& interp, contract::Opcode op) {
    // Only Ext_RtCall reaches this family. (The family mapping in
    // Dispatch.cpp guarantees it.)
    (void)op;

    Frame& f = interp.currentFrame();

    // Read the RuntimeOp operand.
    const uint8_t opByte = readU8(f);
    if (!contract::isRuntimeOp(opByte)) {
        throw PanicException(diag::DiagCode::Bc_UnknownOpcode,
                             "invalid RuntimeOp operand");
    }
    const contract::RuntimeOp rtOp =
        static_cast<contract::RuntimeOp>(opByte);

    using RO = contract::RuntimeOp;
    switch (rtOp) {
        case RO::Retain: {
            // No stack change: the handle stays on the stack, its
            // refcount is incremented. The compiler emits Retain
            // when a value must be shared between two owners (e.g.,
            // storing a handle in a cell while keeping a copy on
            // the stack).
            runtime::Value& v = f.top();
            if (v.tag == runtime::ValueTag::HostHandle) {
                runtime::retainHandle(v.asHostHandle());
            }
            break;
        }

        case RO::Release: {
            // Pop the handle and decrement its refcount.
            const runtime::Value v = f.pop();
            if (v.tag == runtime::ValueTag::HostHandle) {
                runtime::releaseHandle(v.asHostHandle());
            }
            break;
        }

        case RO::CopyString: {
            // Pop the string, push a fresh copy.
            const runtime::Value v = f.pop();
            if (v.tag == runtime::ValueTag::String) {
                runtime::StringObject* copy = runtime::copyString(v.asString());
                f.push(runtime::Value::makeString(copy));
            } else {
                // Defensive: if the value isn't a string, push a nil
                // so the stack shape is preserved. A real mismatch
                // here would be a compiler bug.
                f.push(runtime::Value::makeNil());
            }
            break;
        }

        case RO::FreeString: {
            const runtime::Value v = f.pop();
            if (v.tag == runtime::ValueTag::String) {
                runtime::releaseString(v.asString());
            }
            break;
        }

        case RO::CopyArray: {
            const runtime::Value v = f.pop();
            if (v.tag == runtime::ValueTag::Array) {
                runtime::ArrayObject* copy = runtime::copyArray(v.asArray());
                f.push(runtime::Value::makeArray(copy));
            } else {
                f.push(runtime::Value::makeNil());
            }
            break;
        }

        case RO::FreeArray: {
            const runtime::Value v = f.pop();
            if (v.tag == runtime::ValueTag::Array) {
                runtime::releaseArray(v.asArray());
            }
            break;
        }

        case RO::ConcatString: {
            // Pop two strings (top is the second, below is the first),
            // push their concatenation.
            const runtime::Value b = f.pop();
            const runtime::Value a = f.pop();
            if (a.tag == runtime::ValueTag::String &&
                b.tag == runtime::ValueTag::String) {
                runtime::StringObject* result =
                    runtime::concatStrings(a.asString(), b.asString());
                f.push(runtime::Value::makeString(result));
            } else {
                f.push(runtime::Value::makeNil());
            }
            break;
        }

        case RO::Panic: {
            // The compiler emits this when the script explicitly calls
            // panic(...). The message is on the stack.
            const runtime::Value msg = f.pop();
            std::string message = "panic";
            if (msg.tag == runtime::ValueTag::String) {
                message = std::string(runtime::stringView(msg.asString()));
            }
            throw PanicException(diag::DiagCode::Panic_Generic,
                                 std::move(message));
        }
    }
}

} // namespace lucid::interp