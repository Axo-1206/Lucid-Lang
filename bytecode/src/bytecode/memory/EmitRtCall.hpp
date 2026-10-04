#pragma once

#include "contract/Opcode.hpp"
#include "contract/RuntimeOp.hpp"
#include "bytecode/compile/CompilerContext.hpp"

namespace lucid::bytecode::memory {

/// Emit `Ext_RtCall <op>` and update the stack depth tracker. The
/// runtime op's table entry carries its stack effect.
inline void emitRtCall(compile::CompilerContext& ctx,
                       contract::RuntimeOp op) {
    ctx.emitOpcode(contract::Opcode::Ext_RtCall);
    ctx.emitU8(static_cast<uint8_t>(op));
    const contract::RuntimeOpInfo& info = contract::runtimeOpInfo(op);
    ctx.noteStackEffect(info.pops, info.pushes);
}

} // namespace lucid::bytecode::memory