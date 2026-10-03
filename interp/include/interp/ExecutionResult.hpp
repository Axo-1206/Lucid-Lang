/**
 * @file interp/ExecutionResult.hpp
 *
 * @responsibility What the interpreter returns to the host when a
 *                 function call finishes, panics, or suspends.
 *
 * ─── Design: three outcomes, not a severity scale ─────────────────────────
 * The compiler has a Severity scale (Hint..Fatal, DiagCode.hpp). The
 * interpreter has three outcomes: Completed, Panicked, Suspended. There
 * is no "recoverable error" — the grammar has no try/catch, so an error
 * is always a panic and always unwinds to the host boundary. There is
 * no "warning" — a warning is a compiler concept.
 *
 * ─── Design: Suspended is reserved, not implemented ───────────────────────
 * v1 does not implement @sequence (Sema rejects it, see the grammar
 * §9.2 and the design discussion). The Suspended variant exists in the
 * type so the shape of the API does not change when sequences land, but
 * the interpreter never produces it in v1. A host that receives it in
 * v1 has found a bug.
 */

#pragma once

#include "Value.hpp"
#include "runtime/Panic.hpp"
#include "interp/Value.hpp"

#include <cstdint>
#include <variant>

namespace lucid::interp {

/// @brief A handle to a running sequence. Reserved for v2.
struct SequenceHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
};

/// @brief The interpreter's outcome from one host-initiated call.
struct ExecutionResult {
    /// The function returned normally.
    struct Completed {
        /// The returned value, or a Nil Value for a void function.
        Value result;
    };

    /// The function panicked. The panic carries a DiagCode, a message,
    /// and the Lucid stack at the panic point.
    struct Panicked {
        runtime::Panic panic;
    };

    /// The function suspended. Reserved for v2 — never produced by v1.
    struct Suspended {
        SequenceHandle handle;
    };

    std::variant<Completed, Panicked, Suspended> outcome;

    // ─── Convenience ────────────────────────────────────────────────────

    bool isCompleted() const noexcept {
        return std::holds_alternative<Completed>(outcome);
    }
    bool isPanicked() const noexcept {
        return std::holds_alternative<Panicked>(outcome);
    }
    bool isSuspended() const noexcept {
        return std::holds_alternative<Suspended>(outcome);
    }

    /// Precondition: isCompleted().
    Value result() const { return std::get<Completed>(outcome).result; }

    /// Precondition: isPanicked().
    const runtime::Panic& panic() const {
        return std::get<Panicked>(outcome).panic;
    }

    // ─── Factories ──────────────────────────────────────────────────────

    static ExecutionResult completed(Value v) {
        return ExecutionResult{ Completed{ v } };
    }
    static ExecutionResult panicked(runtime::Panic p) {
        return ExecutionResult{ Panicked{ std::move(p) } };
    }
    static ExecutionResult suspended(SequenceHandle h) {
        return ExecutionResult{ Suspended{ h } };
    }
};

} // namespace lucid::interp