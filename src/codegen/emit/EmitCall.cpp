/// @file codegen/emit/EmitCall.cpp
/// @brief Call lowering: function invocation, argument passing, and
///        intrinsic dispatch.
///
/// ─── Why This File Is Separate ────────────────────────────────────────────
/// Call lowering is where ownership interacts with function types. Every
/// argument must be coerced to its parameter type and, if the parameter is
/// a resource, acquired as `Owned` before the call. Every return value is
/// `Owned` from the callee's perspective (Rule 3 in the ownership model).
///
/// The `fn`/`cls` dispatch and the variadic packing are specific to this
/// file — no other emitter needs them. Keeping them here keeps the general
/// expression emitter focused on producing values.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/intrinsic/IntrinsicEmitter.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitCall — the public entry point
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::emitCall(CallExprAST* expr) {
    // ... (as sketched in Task 5; the full body goes here) ...
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIntrinsic — dispatch to the intrinsic subsystem
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::emitIntrinsic(IntrinsicCallExprAST* expr) {
    // The intrinsic subsystem lives in `src/codegen/intrinsic/`. It
    // handles argument coercion and returns a `Val`. Intrinsics that
    // produce a value return `Owned`; the emitter's caller doesn't need
    // to know which specific intrinsic was called.
    return emitIntrinsicFromAST(expr, *this);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitCallableCall — dispatch on shape
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* Emitter::emitCallableCall(
    llvm::Value* callee,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::FunctionType* fnType,
    FuncShape shape,
    const llvm::Twine& name)
{
    llvm::IRBuilder<>& b = program.builder();

    if (shape == FuncShape::Fn) {
        // Bare function pointer. Cast to the expected signature and
        // call directly.
        llvm::Value* typed = callee;
        if (callee->getType() != llvm::PointerType::get(fnType, 0)) {
            typed = b.CreatePointerCast(
                callee, llvm::PointerType::get(fnType, 0),
                name + "_fn_cast");
        }
        return b.CreateCall(fnType, typed, args, name);
    }

    // cls: fat pointer { fn, env }. Extract both, prepend env, call.
    llvm::Value* funcPtr = b.CreateExtractValue(callee, 0, name + "_func");
    llvm::Value* envPtr = b.CreateExtractValue(callee, 1, name + "_env");
    return emitClosureCall(funcPtr, envPtr, args, fnType->getReturnType());
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureCall — the low-level call through a fat pointer
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* Emitter::emitClosureCall(
    llvm::Value* funcPtr,
    llvm::Value* envPtr,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::Type* returnType)
{
    llvm::IRBuilder<>& b = program.builder();

    // Build the effective function type: env pointer first, then the
    // declared arguments.
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(program.llvmContext(), 0));
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    if (!returnType) {
        returnType = llvm::Type::getVoidTy(program.llvmContext());
    }

    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        returnType, paramTypes, /*isVarArg=*/false);

    // Cast the function pointer to the specific type and call.
    llvm::Value* typedFunc = b.CreatePointerCast(
        funcPtr, llvm::PointerType::get(fnTy, 0), "closure_func_cast");

    std::vector<llvm::Value*> callArgs;
    callArgs.reserve(1 + args.size());
    callArgs.push_back(envPtr);
    for (llvm::Value* arg : args) {
        callArgs.push_back(arg);
    }

    return b.CreateCall(fnTy, typedFunc, callArgs, "closure_call");
}

// ─────────────────────────────────────────────────────────────────────────────
// coerceArgument — argument coercion
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::coerceArgument(Val arg, TypeAST* paramTy) {
    if (!arg.isValid() || !paramTy) return arg;

    llvm::IRBuilder<>& b = program.builder();

    // ─── fn → cls widening ────────────────────────────────────────────────
    if (arg.ty && arg.ty->isa<FuncTypeAST>() && paramTy->isa<FuncTypeAST>()) {
        FuncTypeAST* srcFn = arg.ty->as<FuncTypeAST>();
        FuncTypeAST* dstFn = paramTy->as<FuncTypeAST>();
        if (srcFn->shape == FuncShape::Fn
            && dstFn->shape == FuncShape::Cls) {
            llvm::StructType* closureTy = program.types().closureType();
            llvm::Value* wrapped = llvm::UndefValue::get(closureTy);
            wrapped = b.CreateInsertValue(wrapped, arg.v, 0, "fn_to_cls_func");
            wrapped = b.CreateInsertValue(
                wrapped,
                llvm::ConstantPointerNull::get(
                    llvm::PointerType::get(program.llvmContext(), 0)),
                1, "fn_to_cls_env");
            return Val{wrapped, paramTy, arg.own};
        }
    }

    // ─── Numeric widening/narrowing ───────────────────────────────────────
    llvm::Type* paramLlvmTy = program.types().get(paramTy);
    if (paramLlvmTy && arg.v->getType() != paramLlvmTy) {
        if (arg.v->getType()->isIntegerTy() && paramLlvmTy->isIntegerTy()) {
            unsigned srcBits = arg.v->getType()->getIntegerBitWidth();
            unsigned dstBits = paramLlvmTy->getIntegerBitWidth();
            if (srcBits < dstBits) {
                arg.v = b.CreateSExt(arg.v, paramLlvmTy, "arg_sext");
            } else if (srcBits > dstBits) {
                arg.v = b.CreateTrunc(arg.v, paramLlvmTy, "arg_trunc");
            }
        } else if (arg.v->getType()->isPointerTy()
                   && paramLlvmTy->isPointerTy()) {
            arg.v = b.CreatePointerCast(arg.v, paramLlvmTy, "arg_ptr_cast");
        }
    }

    return arg;
}

// ─────────────────────────────────────────────────────────────────────────────
// coerceTo / coerceValueToType — general coercion helpers
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::coerceTo(Val val, TypeAST* targetTy) {
    return coerceArgument(val, targetTy);
}

llvm::Value* Emitter::coerceValueToType(llvm::Value* val,
                                         llvm::Type* targetTy,
                                         llvm::IRBuilder<>& b) {
    if (val->getType() == targetTy) return val;

    if (val->getType()->isIntegerTy() && targetTy->isIntegerTy()) {
        unsigned srcBits = val->getType()->getIntegerBitWidth();
        unsigned dstBits = targetTy->getIntegerBitWidth();
        if (srcBits < dstBits) return b.CreateSExt(val, targetTy);
        if (srcBits > dstBits) return b.CreateTrunc(val, targetTy);
    }

    if (val->getType()->isFloatingPointTy()
        && targetTy->isFloatingPointTy()) {
        if (val->getType()->getPrimitiveSizeInBits()
            < targetTy->getPrimitiveSizeInBits()) {
            return b.CreateFPExt(val, targetTy);
        }
        return b.CreateFPTrunc(val, targetTy);
    }

    if (val->getType()->isPointerTy() && targetTy->isPointerTy()) {
        return b.CreatePointerCast(val, targetTy);
    }

    // Unsupported coercion. Return the value unchanged so the caller
    // gets a type mismatch at the call instruction, which LLVM reports
    // clearly.
    return val;
}

} // namespace codegen