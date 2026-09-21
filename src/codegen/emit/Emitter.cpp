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

// ─────────────────────────────────────────────────────────────────────────────
// Scope-Exit Callback Emission
// ─────────────────────────────────────────────────────────────────────────────
//
// Ported from the old `emitScopeExitCallback(const ScopeExitRegistration*,
// CodeGenContext&)` free function. The two-branch structure is unchanged:
//
//   - Plain function-reference callback: `reg->callback` is non-null and
//     resolves to an `llvm::Function*` in the function table. Emit a
//     direct call.
//
//   - Closure callback: `reg->callback` is null, meaning the argument was
//     a closure literal or a closure-typed expression rather than a bare
//     function reference. `reg->callExpr->args[0]` is that expression.
//     Lower it, unpack the fat pointer, and call through
//     `emitClosureCall`.
//
// All `ctx.X` become either `program.X` or a direct method call on
// `*this`; `lowerExpression(arg, ctx)` becomes `emit(arg)`; the old
// `loadIfNeeded` calls are gone because `emit` returns an already-loaded
// `Val`.

void Emitter::emitScopeExitCallback(const ScopeExitRegistration* reg) {
    if (!reg) return;

    // ─── Plain function-reference callback ────────────────────────────────
    if (reg->callback) {
        llvm::Function* callee = program.lookupFunction(reg->callback);

        // Sema (validateScopeExit) guarantees a plain function-reference
        // callback resolves to a real declaration. If it didn't, that's a
        // Sema bug, not something CodeGen should diagnose at runtime.
        assert(callee && "scope_exit callback not found — Sema should "
                         "have caught this");
        if (!callee) {
            return;
        }

        std::vector<llvm::Value*> args;
        args.reserve(reg->args.size());
        for (ExprAST* arg : reg->args) {
            Val argVal = emit(arg);
            if (!argVal.isValid()) {
                return;
            }
            args.push_back(argVal.v);
        }

        program.builder().CreateCall(callee, args);
        return;
    }

    // ─── Closure callback ─────────────────────────────────────────────────
    // `reg->callback` is null; the argument wasn't a plain function
    // reference. `reg->callExpr` is the original `#scope_exit(...)` call,
    // and its first argument is the callee slot.
    assert(reg->callExpr && !reg->callExpr->args.empty() &&
           "scope_exit closure registration missing callee expression");
    if (!reg->callExpr || reg->callExpr->args.empty()) {
        return;
    }

    ExprAST* closureExpr = reg->callExpr->args[0];
    Val closureVal = emit(closureExpr);
    if (!closureVal.isValid()) {
        return;
    }

    // The closure value is the `{ ptr func, ptr env }` fat pointer built
    // in `emitAnonFunc`. Unpack it for `emitClosureCall`.
    llvm::Value* funcPtr = program.builder().CreateExtractValue(
        closureVal.v, 0, "scope_exit_closure_func");
    llvm::Value* envPtr = program.builder().CreateExtractValue(
        closureVal.v, 1, "scope_exit_closure_env");

    std::vector<llvm::Value*> closureArgs;
    closureArgs.reserve(reg->args.size());
    for (ExprAST* arg : reg->args) {
        Val argVal = emit(arg);
        if (!argVal.isValid()) {
            return;
        }
        closureArgs.push_back(argVal.v);
    }

    // `#scope_exit` is a void intrinsic: the callback's declared return
    // type is void, so the indirect call's return type is void too.
    llvm::Type* voidTy = llvm::Type::getVoidTy(program.llvmContext());
    emitClosureCall(funcPtr, envPtr, closureArgs, voidTy);
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime Failure Emission
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitFailure(FailureKind kind, SourceLocation loc) {
    const FailureInfo& info = failureInfo(kind);

    // ─── Interceptable + fallback active: branch to it ────────────────────
    if (info.interceptableByCoalesce) {
        if (llvm::BasicBlock* fallback = func().currentNullCoalesceFallback()) {
            program.builder().CreateBr(fallback);
            return;
        }
    }

    // ─── Otherwise: emit the panic ────────────────────────────────────────
    // `emitPanic` writes the message global, calls `__lucid_panic`, and
    // terminates the block with `unreachable`.
    emitPanic(info.runtimeKind, loc);
}

bool Emitter::insideNullCoalesce() const {
    FunctionState* fs = program.currentFunctionState;
    return fs && fs->isInsideNullCoalesce();
}

llvm::BasicBlock* Emitter::nullCoalesceFallbackBlock() const {
    FunctionState* fs = program.currentFunctionState;
    return fs ? fs->currentNullCoalesceFallback() : nullptr;
}

} // namespace codegen