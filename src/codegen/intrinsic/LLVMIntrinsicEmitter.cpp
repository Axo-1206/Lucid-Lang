/// @file codegen/intrinsic/LLVMIntrinsicEmitter.cpp
/// @brief Implementation of the LLVM-mapped intrinsic emitters.

#include "LLVMIntrinsicEmitter.hpp"

#include "codegen/Emitter.hpp"
#include "codegen/Program.hpp"
#include "codegen/Types.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>

namespace codegen {

Val emitLLVMIntrinsic(IntrinsicCallExprAST* expr,
                      const IntrinsicInfo& info,
                      Emitter& emitter) {
    if (!expr) return {};
    if (!info.llvmID.has_value()) return {};

    ProgramState& program = emitter.program;
    llvm::IRBuilder<>& b = program.builder();

    // ─── Emit arguments ───────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    std::vector<llvm::Type*> argTypes;
    args.reserve(expr->args.size());
    argTypes.reserve(expr->args.size());

    for (ExprAST* argExpr : expr->args) {
        Val argVal = emitter.emit(argExpr);
        if (!argVal.isValid()) return {};

        args.push_back(argVal.v);
        argTypes.push_back(argVal.v->getType());
    }

    // ─── Get the intrinsic declaration ────────────────────────────────────
    // `llvm::Intrinsic::getDeclaration` returns the overloaded form
    // matching the argument types. For non-overloaded intrinsics the
    // types are ignored.
    llvm::Function* fn = llvm::Intrinsic::getDeclaration(
        &program.module(), *info.llvmID, argTypes);
    if (!fn) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "could not declare LLVM intrinsic for '#",
            program.pool.lookup(expr->intrinsicName), "'");
        return {};
    }

    // ─── Emit the call ────────────────────────────────────────────────────
    llvm::Value* result = b.CreateCall(fn, args,
                                       program.pool.lookup(expr->intrinsicName));

    // ─── Return type ──────────────────────────────────────────────────────
    // A void intrinsic (e.g. an atomic store) returns an invalid `Val`.
    if (fn->getReturnType()->isVoidTy()) {
        return {};
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

} // namespace codegen