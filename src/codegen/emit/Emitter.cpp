/// @file codegen/emit/Emitter.cpp
/// @brief The `Emitter` constructor and the `func()` accessor.
///
/// ─── Why This File Is So Small ────────────────────────────────────────────
/// The emitter's real work lives in `EmitDecl.cpp`, `EmitExpr.cpp`,
/// `EmitStmt.cpp`, `EmitPlace.cpp`, `EmitCall.cpp`, `EmitClosure.cpp`, and
/// `EmitConcurrency.cpp`. Those files implement the per-kind emitters and
/// the four public entry points.
///
/// This file holds only what none of them owns:
///
///   - The constructor, which stores the `ProgramState&` and does nothing
///     else. The emitter has no state of its own — every piece of mutable
///     state it reads or writes lives on `ProgramState` (the module, the
///     builder, the types, the ABI, the ownership engine) or on the
///     currently active `FunctionState` (the scope stack, the loop stack,
///     the value map).
///
///   - `func()`, the accessor that returns the currently active
///     `FunctionState`. Every emitter body that touches the scope stack,
///     the loop stack, or the value map calls it, so it must exist as a
///     single point of truth with a single assertion. Putting it inline
///     in the header would work, but the assertion would be compiled into
///     every translation unit that includes the header; keeping it here
///     means one definition and one place to change the invariant.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Construct an `Emitter` for one program.
///
/// The emitter is a pure client of `ProgramState`: it holds a reference
/// and reads everything else through it. There is no per-emitter state to
/// initialize.
///
/// In the current design, `ProgramState` constructs its `Emitter` in its
/// own constructor body (`emitter_ = std::make_unique<Emitter>(*this)`),
/// so the `ProgramState&` passed here is always a fully-constructed
/// object by the time this constructor runs. Every accessor the emitter
/// uses (`program.builder()`, `program.types()`, `program.abi()`,
/// `program.ownership()`) is valid from this point on.
Emitter::Emitter(ProgramState& program_)
    : program(program_)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// func — the current FunctionState accessor
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The currently active `FunctionState`.
///
/// This is the emitter's single point of access to per-function state:
/// the scope stack, the loop stack, and the value map. Every emitter
/// body that touches any of those calls `func()`, so the assertion here
/// is the first line of defense against an emitter running outside a
/// function body.
///
/// ─── When Is `func()` Valid? ──────────────────────────────────────────────
/// `ProgramState::currentFunctionState` is set by `FunctionState`'s
/// constructor and cleared by its destructor. So `func()` is valid
/// precisely between those two events:
///
///   - Inside `emitFuncBody`, after `FunctionState state(...)` is
///     constructed and before it goes out of scope.
///   - Inside `emitClosureBody`, same.
///   - Inside `ModulePass::emitModuleInit` / `emitModuleFree`, which
///     construct a `FunctionState` for the synthesized init/free
///     function.
///
/// It is NOT valid:
///
///   - At module scope, in the declare pass, or anywhere that emits
///     declarations rather than function bodies. Those paths do not
///     construct a `FunctionState`.
///   - Inside `DropGlue.cpp`'s generated `__drop_<T>` / `__copy_<T>`
///     functions. Those emit into a different function's body but do not
///     construct a `FunctionState` for it — they only temporarily move
///     the builder. If a glue function ever needs the scope stack, the
///     caller must construct a `FunctionState` around it.
///
/// ─── Why an Assertion, Not a Null Check ───────────────────────────────────
/// A null `currentFunctionState` reaching an emitter that needs it is a
/// compiler bug: the emitter was called from the wrong phase. Asserting
/// catches it during development; silently returning a default-constructed
/// state would produce garbage IR and a much harder-to-diagnose failure.
FunctionState& Emitter::func() {
    assert(program.currentFunctionState
           && "Emitter::func() called with no active FunctionState — "
              "this emitter was reached from the wrong phase");
    return *program.currentFunctionState;
}

} // namespace codegen