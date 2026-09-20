/// @file codegen/emit/EmitCall.cpp
/// @brief Call lowering — function invocation, argument passing, and
///        intrinsic dispatch.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The call-lowering subsystem:
///
///   - `emitCall`           — the `CallExprAST` emitter.
///   - `emitIntrinsic`      — the `IntrinsicCallExprAST` emitter, which
///                            forwards to the intrinsic subsystem.
///   - `emitCallableCall`   — the `fn`/`cls` dispatch on the callee's
///                            `FuncShape`.
///   - `emitClosureCall`    — the fat-pointer indirect call.
///   - `coerceArgument`     — coerce a value to a parameter's type.
///   - `coerceTo`           — coerce a value to a target AST type.
///   - `coerceValueToType`  — coerce an `llvm::Value*` to a target
///                            `llvm::Type*`.
///
/// ─── The Ownership Flow at a Call Site ────────────────────────────────────
/// Argument passing is where ownership interacts with function types. Each
/// argument is:
///
///   1. Emitted (may be `Owned` or `Borrowed`).
///   2. Coerced (`fn → cls` if the parameter is `cls`; integer width; pointer
///      cast).
///   3. Materialized (spilled to an alloca if the parameter crosses by
///      pointer; left as-is otherwise).
///   4. Acquired as `Owned` via `intoOwned` — this is the retain-on-copy
///      rule for closure arguments, and the deep-copy rule for strings.
///
/// The result of the call is `Owned`. Per Rule 3 of the ownership model,
/// the callee transfers the claim of the return value to the caller. For a
/// `void` return, the result is an invalid `Val` (no value, no claim).
///
/// ─── The `fn` / `cls` Dispatch ────────────────────────────────────────────
/// The callee's `FuncShape` determines how the call is emitted:
///
///   - `fn`-shaped: the callee is a bare `ptr` to a function. Cast to the
///     expected `FunctionType` and call directly. No environment.
///
///   - `cls`-shaped: the callee is a `{ ptr fn, ptr env }` fat pointer.
///     Extract both fields, prepend `env` to the argument list, and call
///     `fn` indirectly. The runtime calling convention for closures is
///     `R fn(ptr env, params...)`.
///
/// ─── The Struct-by-Pointer Convention ─────────────────────────────────────
/// The runtime ABI (see `functions.def`, rule 1) passes aggregates by
/// pointer, not by value. The emitter's `materializeArgument` helper is
/// what implements this on the caller side: it takes a `Val` and returns
/// an `llvm::Value*` suitable for the call, spilling aggregates to stack
/// slots and passing the slot pointers.
///
/// Today, Lucid-defined functions still use by-value aggregates (see
/// `Types::functionType`). The runtime's own functions use by-pointer.
/// `materializeArgument` handles the runtime's convention; when
/// `Types::functionType` is updated to pass aggregates by pointer for
/// Lucid functions, `materializeArgument` handles both.
///
/// ─── Coercion: The Three Helpers ──────────────────────────────────────────
/// The three coercion helpers operate at different levels:
///
///   - `coerceTo(Val, TypeAST*)`: the AST-level entry point. Handles the
///     `fn → cls` widening, then delegates to `coerceArgument` for the
///     LLVM-level coercion. Used by `emitVarDecl`, `emitReturnStmt`,
///     `emitCall`'s argument pass, `emitAssign`.
///
///   - `coerceArgument(Val, TypeAST*)`: the argument-passing form. Same
///     behavior as `coerceTo`, but the semantic role is "make this value
///     match the parameter's declared type."
///
///   - `coerceValueToType(llvm::Value*, llvm::Type*, IRBuilder&)`: the
///     LLVM-level fallback. No AST; just "make this value into this LLVM
///     type." Used when the AST types agree but the LLVM types don't
///     (rare), or when the caller has a raw LLVM type and no AST type.

#include "../Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/intrinsic/IntrinsicEmitter.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// materializeArgument — spill aggregates, pass scalars as-is
// ─────────────────────────────────────────────────────────────────────────────
//
// The runtime ABI passes aggregates (structs, slices) by pointer, not by
// value. `materializeArgument` is the caller-side bridge: given a `Val`
// and the LLVM type the call site expects, it returns an `llvm::Value*`
// suitable for the call's argument list.
//
// ─── Rules ────────────────────────────────────────────────────────────────
//   - Scalar (integer, float, pointer): pass the value as-is.
//   - Aggregate (struct, array): spill to a fresh alloca and pass the
//     alloca's pointer.
//   - Vector (SIMD): pass the value as-is (LLVM handles vectors natively).
//
// ─── Why a Helper, Not Inline ─────────────────────────────────────────────
// The spill-to-alloca pattern is used by every runtime call that takes a
// struct, by every Lucid call that takes a struct parameter once
// `Types::functionType` uses the by-pointer convention, and by closure
// calls that pass captured aggregates. Funnelling all of it through one
// helper makes the convention a single source of truth.

namespace {

/// True if this LLVM type must be passed by pointer at the ABI level.
///
/// Aggregates (structs and arrays) cross the ABI by pointer. Scalars,
/// pointers, and vectors cross by value.
bool passesByPointer(llvm::Type* ty) {
    if (!ty) return false;
    return ty->isStructTy() || ty->isArrayTy();
}

} // anonymous namespace

llvm::Value* Emitter::materializeArgument(Val val) {
    if (!val.isValid()) return nullptr;

    llvm::Type* ty = val.v->getType();
    if (!passesByPointer(ty)) {
        // Scalars, pointers, vectors: pass as-is.
        return val.v;
    }

    // Aggregate: spill to a stack slot and pass the pointer.
    //
    // The alloca goes in the entry block, so it's reused across loop
    // iterations. The value written into it each time is whatever the
    // caller passed; the slot is reused, not reallocated.
    llvm::AllocaInst* slot = createEntryAlloca(ty, "arg.spill");
    if (!slot) return nullptr;

    llvm::IRBuilder<>& b = program.builder();
    b.CreateStore(val.v, slot);
    return slot;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitCall — a function call
// ─────────────────────────────────────────────────────────────────────────────
//
// The callee is emitted first. Its resolved AST type must be a
// `FuncTypeAST`; anything else is a Sema bug. The arguments are emitted
// in order, coerced to their parameter types, materialized (spilled if
// aggregates), and acquired as `Owned`.
//
// The call itself dispatches on the callee's `FuncShape`. The result is
// `Owned`; for a `void` return, the result is invalid.

Val Emitter::emitCall(CallExprAST* expr) {
    assert(expr && "emitCall() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Emit the callee ──────────────────────────────────────────────────
    Val calleeVal = emit(expr->callee);
    if (!calleeVal.isValid()) return {};

    // ─── Callee's function type ───────────────────────────────────────────
    // Sema guarantees the callee's resolved type is a `FuncTypeAST`. If
    // it isn't, the call is malformed.
    FuncTypeAST* calleeFnTy = expr->callee->resolvedType
        ? (expr->callee->resolvedType->isa<FuncTypeAST>()
              ? expr->callee->resolvedType->as<FuncTypeAST>()
              : nullptr)
        : nullptr;
    if (!calleeFnTy) {
        program.diagnostics.errorAt(
            DiagCode::Sem_NotCallable, expr->callee->loc,
            "call callee does not resolve to a function type");
        return {};
    }

    // ─── LLVM function type ───────────────────────────────────────────────
    llvm::FunctionType* fnTy = program.types().functionType(
        calleeFnTy, /*isClosure=*/false);
    if (!fnTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "call has an invalid function signature");
        return {};
    }

    // ─── Evaluate and coerce arguments ────────────────────────────────────
    std::vector<llvm::Value*> args;
    args.reserve(expr->args.size());

    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* argExpr = expr->args[i];
        Val argVal = emit(argExpr);
        if (!argVal.isValid()) return {};

        // ─── Parameter type for this argument ─────────────────────────────
        // `calleeFnTy->params[i]` is the parameter's AST type. Sema
        // checked the arity; if `i` is out of range here, it's a Sema
        // bug, and the emitter shouldn't produce bad IR for it.
        TypeAST* paramTy = (i < calleeFnTy->params.size())
            ? calleeFnTy->params[i]->type
            : nullptr;

        // ─── Coercion ─────────────────────────────────────────────────────
        // Handles `fn → cls` widening, integer width adjustment, pointer
        // casts.
        if (paramTy) {
            argVal = coerceArgument(argVal, paramTy);
            if (!argVal.isValid()) return {};
        }

        // ─── Materialize ──────────────────────────────────────────────────
        // Spill aggregates to stack slots; pass scalars as-is.
        llvm::Value* materialized = materializeArgument(argVal);
        if (!materialized) return {};

        // ─── Acquire a fresh claim ────────────────────────────────────────
        // The callee's parameter binding takes over the claim. For a
        // `Borrowed` argument (a load from a binding), this retains or
        // deep-copies. For an `Owned` argument (a fresh value), it's a
        // no-op. Either way, the value passed to the call carries a
        // claim the callee can release at function exit.
        //
        // The claim-acquisition is applied to the *materialized* value,
        // not the pre-spill value. The materialized value is what the
        // callee will see; the claim it holds is what the callee's
        // binding takes over.
        Val materializedVal{materialized, argVal.ty, argVal.own};
        Val owned = program.ownership().intoOwned(materializedVal, b);
        if (!owned.isValid()) return {};

        args.push_back(owned.v);
    }

    // ─── Emit the call ────────────────────────────────────────────────────
    llvm::Value* result = emitCallableCall(
        calleeVal.v, args, fnTy, calleeFnTy->shape, "call");
    if (!result) return {};

    // ─── Return type ──────────────────────────────────────────────────────
    TypeAST* returnTy = calleeFnTy->returnType;
    if (!returnTy) {
        // Void return. No value, no claim.
        return {};
    }

    return Val{result, returnTy, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIntrinsic — dispatch to the intrinsic subsystem
// ─────────────────────────────────────────────────────────────────────────────
//
// The intrinsic subsystem lives in `src/codegen/intrinsic/`. It handles
// argument coercion and returns a value. The emitter wraps the value in
// a `Val` with the AST's resolved type and the `Owned` tag.
//
// ─── Why `Owned` ──────────────────────────────────────────────────────────
// Intrinsics compute a value. Even an intrinsic that reads from memory
// (like `#memcpy`'s source) produces a result that the caller owns; the
// source is not aliased by the result. The `Owned` tag reflects that.
//
// If an intrinsic ever needs to return a `Borrowed` result (a
// hypothetical `#ptr` that returns a view of a value), the intrinsic
// subsystem must signal it. Today, no intrinsic does.

Val Emitter::emitIntrinsic(IntrinsicCallExprAST* expr) {
    assert(expr && "emitIntrinsic() with null expression");

    // The intrinsic subsystem returns a bare `llvm::Value*`; the emitter
    // tags it. `emitIntrinsicFromAST` is the entry point defined in
    // `intrinsic/IntrinsicEmitter.hpp`.
    llvm::Value* result = emitIntrinsicFromAST(expr, *this);
    if (!result) return {};

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitCallableCall — the fn/cls dispatch
// ─────────────────────────────────────────────────────────────────────────────
//
// Two paths:
//
//   - `fn`-shaped: the callee is a bare `ptr` to a function. Cast to the
//     expected `FunctionType` and call directly.
//
//   - `cls`-shaped: the callee is a `{ ptr fn, ptr env }` fat pointer.
//     Extract both fields, prepend `env` to the argument list, and call
//     `fn` indirectly.

llvm::Value* Emitter::emitCallableCall(llvm::Value* callee,
                                       llvm::ArrayRef<llvm::Value*> args,
                                       llvm::FunctionType* fnType,
                                       FuncShape shape,
                                       const llvm::Twine& name) {
    assert(callee && "emitCallableCall() with null callee");
    assert(fnType && "emitCallableCall() with null function type");

    llvm::IRBuilder<>& b = program.builder();

    if (shape == FuncShape::Fn) {
        // ─── Bare function pointer ────────────────────────────────────────
        // The callee value is an opaque `ptr`. With opaque pointers, no
        // cast is needed at the LLVM level — but building the typed
        // pointer value keeps the call site self-documenting and lets
        // `CreateCall` infer the type from the callee's `FunctionType`.
        //
        // `CreateCall` with an explicit `FunctionType` handles the cast
        // internally; passing the opaque callee is fine.
        return b.CreateCall(fnType, callee, args, name);
    }

    // ─── Closure fat pointer ──────────────────────────────────────────────
    // The callee is `{ ptr fn, ptr env }`. Extract both fields.
    llvm::Value* funcPtr = b.CreateExtractValue(
        callee, 0, name + ".fn");
    llvm::Value* envPtr = b.CreateExtractValue(
        callee, 1, name + ".env");

    // The closure's underlying function has signature
    // `R fn(ptr env, declaredParams...)`. The emitter builds a
    // `FunctionType` reflecting that and calls `funcPtr` indirectly.
    return emitClosureCall(funcPtr, envPtr, args, fnType->getReturnType());
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureCall — the low-level call through a fat pointer
// ─────────────────────────────────────────────────────────────────────────────
//
// The closure's underlying function has signature
// `R fn(ptr env, declaredParams...)` — the environment pointer is
// prepended to the declared parameters. The emitter builds the extended
// `FunctionType` from the arguments' LLVM types and the return type, then
// casts the function pointer and calls it.
//
// ─── Why the Signature Is Rebuilt ─────────────────────────────────────────
// The closure's fat pointer holds an opaque `ptr` for the function. The
// emitter doesn't have a symbol to look up; it only has the pointer. The
// signature must be reconstructed from the call's context: the declared
// parameters come from the args list, and the return type comes from the
// callee's AST type. Building the `FunctionType` from those is the only
// way to produce a type-correct call.

llvm::Value* Emitter::emitClosureCall(llvm::Value* funcPtr,
                                      llvm::Value* envPtr,
                                      llvm::ArrayRef<llvm::Value*> args,
                                      llvm::Type* returnType) {
    assert(funcPtr && "emitClosureCall() with null funcPtr");

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    // ─── Extended parameter list ──────────────────────────────────────────
    // `env` first, then the declared arguments.
    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(1 + args.size());
    paramTypes.push_back(llvm::PointerType::get(ctx, 0));  // env
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx);
    }

    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        returnType, paramTypes, /*isVarArg=*/false);

    // ─── Cast the function pointer ────────────────────────────────────────
    // `funcPtr` is an opaque `ptr`. With opaque pointers, no cast is
    // needed to call it; `CreateCall` uses the `FunctionType` directly.
    // But building a typed cast documents the intent and matches what a
    // C compiler emits for an indirect call through a function pointer.
    llvm::Value* typedFunc = b.CreatePointerCast(
        funcPtr,
        llvm::PointerType::get(ctx, 0),  // opaque; the cast is a no-op
        "closure.fn.typed");

    // ─── Build the argument list ──────────────────────────────────────────
    std::vector<llvm::Value*> callArgs;
    callArgs.reserve(1 + args.size());
    callArgs.push_back(envPtr);
    for (llvm::Value* arg : args) {
        callArgs.push_back(arg);
    }

    return b.CreateCall(fnTy, typedFunc, callArgs, "closure.call");
}

// ─────────────────────────────────────────────────────────────────────────────
// coerceArgument — coerce a value to a parameter's declared type
// ─────────────────────────────────────────────────────────────────────────────
//
// The argument-passing form of coercion. Delegates to `coerceTo` — the
// two are semantically identical; the name reflects the call site's role.

Val Emitter::coerceArgument(Val arg, TypeAST* paramTy) {
    return coerceTo(arg, paramTy);
}

// ─────────────────────────────────────────────────────────────────────────────
// coerceTo — coerce a value to a target AST type
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Two Kinds of Coercion ────────────────────────────────────────────
//   1. **Shape changes**: `fn → cls` widening. This is AST-aware: only
//      `fn`-shaped `FuncTypeAST`s convert to `cls`-shaped ones, and the
//      conversion produces a `{ ptr, ptr }` fat pointer from a bare `ptr`.
//
//   2. **LLVM type adjustments**: integer widening/narrowing, pointer
//      casts, float extension/truncation. These operate at the LLVM
//      type level and don't care about the AST.
//
// The order matters: shape changes run first, because they produce a
// value whose LLVM shape is different (the fat pointer). Once the shape
// is right, the LLVM-level adjustments fix any remaining mismatch.

Val Emitter::coerceTo(Val val, TypeAST* targetTy) {
    if (!val.isValid()) return val;
    if (!targetTy) return val;

    // ─── Shape change: fn → cls ───────────────────────────────────────────
    if (val.ty && val.ty->isa<FuncTypeAST>() && targetTy->isa<FuncTypeAST>()) {
        FuncTypeAST* srcFn = val.ty->as<FuncTypeAST>();
        FuncTypeAST* dstFn = targetTy->as<FuncTypeAST>();

        if (srcFn->shape == FuncShape::Fn
            && dstFn->shape == FuncShape::Cls) {
            llvm::StructType* closureTy = program.types().closureType();
            if (!closureTy) return val;

            llvm::IRBuilder<>& b = program.builder();
            llvm::Value* wrapped = llvm::UndefValue::get(closureTy);
            wrapped = b.CreateInsertValue(
                wrapped, val.v, 0, "fn_to_cls.fn");
            wrapped = b.CreateInsertValue(
                wrapped,
                llvm::ConstantPointerNull::get(
                    llvm::PointerType::get(program.llvmContext(), 0)),
                1, "fn_to_cls.env");

            Val result = val;
            result.v = wrapped;
            result.ty = targetTy;
            return result;
        }
        // `cls → fn` is a narrowing the language doesn't support
        // (a `cls` value has an env the `fn` shape can't carry). Sema
        // rejects it; the emitter passes the value through unchanged.
    }

    // ─── LLVM type adjustments ────────────────────────────────────────────
    llvm::Type* targetLlvmTy = program.types().get(targetTy);
    if (!targetLlvmTy) return val;

    if (val.v->getType() != targetLlvmTy) {
        llvm::IRBuilder<>& b = program.builder();
        llvm::Value* adjusted = coerceValueToType(
            val.v, targetLlvmTy, b);
        if (!adjusted) return val;

        Val result = val;
        result.v = adjusted;
        result.ty = targetTy;
        return result;
    }

    // Already the right shape and type. Return with the target type so
    // the caller's subsequent operations use the right AST type.
    Val result = val;
    result.ty = targetTy;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// coerceValueToType — the LLVM-level coercion
// ─────────────────────────────────────────────────────────────────────────────
//
// The lowest-level coercion helper. Given an `llvm::Value*` and a target
// `llvm::Type*`, produce a value of the target type — or return the
// original value unchanged if the types already match.
//
// ─── Supported Coercions ──────────────────────────────────────────────────
//   - Integer → integer: sign-extend, zero-extend, or truncate.
//   - Float → float: extend or truncate.
//   - Integer → float: signed integer to float.
//   - Float → integer: float to signed integer (truncating).
//   - Pointer → pointer: bitcast (no-op with opaque pointers).
//
// ─── Unsupported Coercions ────────────────────────────────────────────────
// Any conversion not in the list above returns the original value. The
// caller gets a type mismatch at the next use, which the verifier
// reports. Returning the original value is safer than emitting an
// invalid instruction; a hard error would abort compilation on what might
// be a Sema-adjacent bug.
//
// ─── Signedness ───────────────────────────────────────────────────────────
// LLVM integers are signless. The emitter picks sign-extension by default
// (the language's integer types are signed unless prefixed with `u`). If
// the AST had full signedness information, this would dispatch on it;
// today the emitter assumes signed.

llvm::Value* Emitter::coerceValueToType(llvm::Value* val,
                                        llvm::Type* targetTy,
                                        llvm::IRBuilder<>& b) {
    if (!val) return nullptr;
    if (!targetTy) return val;

    llvm::Type* srcTy = val->getType();
    if (srcTy == targetTy) return val;

    // ─── Integer ↔ integer ────────────────────────────────────────────────
    if (srcTy->isIntegerTy() && targetTy->isIntegerTy()) {
        unsigned srcBits = srcTy->getIntegerBitWidth();
        unsigned dstBits = targetTy->getIntegerBitWidth();
        if (srcBits < dstBits) {
            return b.CreateSExt(val, targetTy, "coerce.sext");
        }
        if (srcBits > dstBits) {
            return b.CreateTrunc(val, targetTy, "coerce.trunc");
        }
        return val;  // same width
    }

    // ─── Float ↔ float ────────────────────────────────────────────────────
    if (srcTy->isFloatingPointTy() && targetTy->isFloatingPointTy()) {
        unsigned srcBits = srcTy->getPrimitiveSizeInBits();
        unsigned dstBits = targetTy->getPrimitiveSizeInBits();
        if (srcBits < dstBits) {
            return b.CreateFPExt(val, targetTy, "coerce.fpext");
        }
        return b.CreateFPTrunc(val, targetTy, "coerce.fptrunc");
    }

    // ─── Integer ↔ float ──────────────────────────────────────────────────
    if (srcTy->isIntegerTy() && targetTy->isFloatingPointTy()) {
        return b.CreateSIToFP(val, targetTy, "coerce.sitofp");
    }
    if (srcTy->isFloatingPointTy() && targetTy->isIntegerTy()) {
        return b.CreateFPToSI(val, targetTy, "coerce.fptosi");
    }

    // ─── Pointer ↔ pointer ────────────────────────────────────────────────
    if (srcTy->isPointerTy() && targetTy->isPointerTy()) {
        // With opaque pointers, the bitcast is a no-op and LLVM may fold
        // it away. Emitting it keeps the IR self-documenting.
        return b.CreatePointerCast(val, targetTy, "coerce.ptrcast");
    }

    // ─── Aggregate → aggregate (same shape) ───────────────────────────────
    // A struct-to-struct conversion is not a coercion the emitter
    // supports. If the caller needs one (e.g. a nominal subtype), Sema
    // should have inserted an explicit conversion. Return the value
    // unchanged; the caller's next use catches the mismatch.
    if (srcTy->isStructTy() && targetTy->isStructTy()) {
        return val;
    }

    // ─── Fallthrough ──────────────────────────────────────────────────────
    // Unsupported coercion. Return the original value; the caller's
    // next use produces a type mismatch, which the verifier catches.
    return val;
}

} // namespace codegen