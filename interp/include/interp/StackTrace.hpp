// interp/include/interp/StackTrace.hpp
#pragma once

#include "runtime/Panic.hpp"

#include <cstdint>
#include <vector>

namespace lucid::interp {

class Frame;

/// @brief Build a stack trace from the given frames.
///
/// The frames are ordered outermost-first (index 0 is the host-called
/// function). The returned trace is ordered newest-first.
///
/// `skipFrames` frames at the top are omitted from the trace — used to
/// hide the interpreter's own bookkeeping frames, if any are pushed.
std::vector<runtime::StackFrame>
captureStackTrace(const std::vector<Frame>& frames,
                  uint32_t skipFrames = 0);

} // namespace lucid::interp