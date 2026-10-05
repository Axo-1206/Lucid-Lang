#include "interp/InterpreterInternal.hpp"

#include "bytecode/FunctionProto.hpp"

#include "core/diagnostics/DiagCode.hpp"

#include "interp/InterpreterError.hpp"
#include "runtime/Panic.hpp"

#include <cassert>

namespace lucid::interp {

// Defined in Dispatch.cpp.
runtime::Value dispatch(InterpreterInternal& interp);

InterpreterInternal::InterpreterInternal(LoadedProgram& program,
                                         const InterpreterConfig& config)
    : m_program(program)
    , m_config(config)
{
}

Frame& InterpreterInternal::pushFrame(
    const bytecode::FunctionProto* proto) {
    if (callDepth() >= m_config.maxCallDepth) {
        throw PanicException(diag::DiagCode::Panic_StackOverflow,
                             "maximum call depth exceeded");
    }
    m_frames.emplace_back(proto, callDepth());
    return m_frames.back();
}

runtime::Value InterpreterInternal::run(uint32_t functionIndex,
                                        const runtime::Value* args,
                                        uint32_t argCount) {
    assert(m_frames.empty());

    const bytecode::FunctionProto* proto =
        m_program.functionAt(functionIndex);
    assert(proto != nullptr);

    Frame& frame = pushFrame(proto);

    for (uint32_t i = 0; i < argCount; ++i) {
        frame.setParameter(i, args[i]);
    }

    return dispatch(*this);
}

} // namespace lucid::interp