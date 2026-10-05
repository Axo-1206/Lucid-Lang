#pragma once

#include "interp/Frame.hpp"
#include "interp/InterpreterConfig.hpp"
#include "interp/LoadedProgram.hpp"

#include "runtime/Value.hpp"

#include <cstdint>
#include <vector>

namespace lucid::interp {

class InterpreterInternal {
public:
    InterpreterInternal(LoadedProgram& program,
                        const InterpreterConfig& config);

    InterpreterInternal(const InterpreterInternal&) = delete;
    InterpreterInternal& operator=(const InterpreterInternal&) = delete;
    InterpreterInternal(InterpreterInternal&&) = delete;
    InterpreterInternal& operator=(InterpreterInternal&&) = delete;
    ~InterpreterInternal() = default;

    LoadedProgram& program() noexcept { return m_program; }
    const LoadedProgram& program() const noexcept { return m_program; }

    const InterpreterConfig& config() const noexcept { return m_config; }

    std::vector<Frame>& frames() noexcept { return m_frames; }
    const std::vector<Frame>& frames() const noexcept { return m_frames; }

    Frame& currentFrame() noexcept { return m_frames.back(); }
    const Frame& currentFrame() const noexcept { return m_frames.back(); }

    Frame& pushFrame(const bytecode::FunctionProto* proto);
    void popFrame() noexcept { m_frames.pop_back(); }

    uint32_t callDepth() const noexcept {
        return static_cast<uint32_t>(m_frames.size());
    }

    runtime::Value run(uint32_t functionIndex,
                       const runtime::Value* args,
                       uint32_t argCount);

private:
    LoadedProgram& m_program;
    const InterpreterConfig& m_config;
    std::vector<Frame> m_frames;
};

} // namespace lucid::interp