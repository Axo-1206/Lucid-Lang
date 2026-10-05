/**
 * @file interp/StackTrace.cpp
 *
 * @responsibility Building a Lucid stack trace from the interpreter's
 *                 frame stack. Called when a panic is raised; the
 *                 resulting trace is attached to the runtime::Panic.
 *
 * ─── Design: walk the frame stack, newest-first ───────────────────────────
 * The interpreter's frame stack is ordered outermost-first (the
 * host-called function is at the bottom, the currently executing
 * function at the top). A stack trace is conventionally printed
 * newest-first, so this function walks the stack in reverse.
 *
 * ─── Design: line lookup goes through FunctionProto ───────────────────────
 * Each frame's FunctionProto has a line table (a sorted array of
 * (codeOffset, line, column, filePath) entries). Finding the location
 * for a frame's current `ip` is a binary search; the FunctionProto's
 * `locationAt` does it.
 *
 * ─── Design: no allocation unless there is a panic ────────────────────────
 * This function is only called on the panic path. Allocating the trace
 * (strings for the function names and file paths) is acceptable
 * because panics are rare.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * interp/Frame.hpp, interp/StackTrace.hpp, bytecode/FunctionProto.hpp.
 */

#include "interp/Frame.hpp"

#include "bytecode/FunctionProto.hpp"

#include "interp/StackTrace.hpp"

#include <string>
#include <vector>

namespace lucid::interp {

std::vector<runtime::StackFrame>
captureStackTrace(const std::vector<Frame>& frames,
                  uint32_t skipFrames) {
    std::vector<runtime::StackFrame> trace;

    if (frames.size() <= skipFrames) return trace;
    trace.reserve(frames.size() - skipFrames);

    // Walk newest-first.
    for (size_t i = frames.size(); i-- > skipFrames; ) {
        const Frame& f = frames[i];
        const bytecode::FunctionProto* proto = f.proto();
        if (!proto) continue;

        runtime::StackFrame sf;
        sf.function = proto->name();

        // Look up the source location for the frame's current ip.
        // locationAt returns an optional; a missing entry means the
        // code offset was compiler-generated glue (which has no
        // source line).
        if (auto loc = proto->locationAt(f.ip())) {
            sf.line   = loc->line;
            sf.column = loc->column;
            sf.module = std::string(loc->filePath);
        }
        // A frame with no location entry still appears in the trace,
        // with function name only.

        trace.push_back(std::move(sf));
    }
    return trace;
}

} // namespace lucid::interp