/// @file codegen/intrinsic/IntrinsicEmitter.cpp
/// @brief Implementation of the intrinsic dispatcher.

#include "IntrinsicEmitter.hpp"
#include "LucidIntrinsicEmitter.hpp"
#include "LLVMIntrinsicEmitter.hpp"

#include "codegen/Emitter.hpp"
#include "codegen/Program.hpp"

#include "core/registry/IntrinsicRegistry.hpp"

namespace codegen {

Val emitIntrinsicFromAST(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (!expr) return {};

    // ─── Registry lookup ──────────────────────────────────────────────────
    // The registry holds the intrinsic's metadata: whether it's an
    // LLVM-mapped intrinsic (with an `llvmID`), whether it's a
    // compiler-handled intrinsic (folded by Sema's const evaluator),
    // and any other per-intrinsic facts.
    //
    // The registry is a singleton keyed on the string pool. This mirrors
    // the old code's access pattern; the registry isn't part of codegen's
    // state and doesn't need to be a member of `ProgramState`.
    IntrinsicRegistry& registry =
        IntrinsicRegistry::getInstance(emitter.program.pool);

    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info || !info->isValid()) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_UnknownIntrinsic, expr->loc,
            "unknown intrinsic '#",
            emitter.program.pool.lookup(expr->intrinsicName), "'");
        return {};
    }

    // ─── Dispatch ─────────────────────────────────────────────────────────
    // Two routes:
    //
    //   - If the intrinsic has an `llvmID`, it maps to an LLVM intrinsic.
    //     Delegate to the LLVM-side emitter, which calls
    //     `llvm::Intrinsic::getDeclaration` and emits the call.
    //
    //   - Otherwise, it's a Lucid-side intrinsic implemented directly by
    //     CodeGen. Delegate to the Lucid-side emitter.
    //
    // The registry's `llvmID` field is `std::optional<llvm::Intrinsic::ID>`
    // — null for Lucid-side intrinsics, set for LLVM-mapped ones. Some
    // Lucid-side intrinsics (`#sizeof`, `#typeof`) are also compiler-
    // folded by Sema; by the time the intrinsic emitter runs, `expr->isConst`
    // would have short-circuited via `Emitter::emit`. If the fold didn't
    // happen, the Lucid-side emitter handles them at runtime.
    if (info->llvmID.has_value()) {
        return emitLLVMIntrinsic(expr, *info, emitter);
    }

    return emitLucidIntrinsic(expr, *info, emitter);
}

} // namespace codegen