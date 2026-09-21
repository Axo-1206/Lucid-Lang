/// @file codegen/intrinsic/IntrinsicEmitter.cpp
/// @brief Implementation of the intrinsic dispatcher.

#include "IntrinsicEmitter.hpp"
#include "LucidIntrinsicEmitter.hpp"
#include "LLVMIntrinsicEmitter.hpp"

#include "codegen/emit/Emitter.hpp"
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
    // The registry has TWO independent classifications:
    //
    //   - `info.llvmID` — whether the intrinsic maps to a literal
    //     `llvm::Intrinsic::ID`. Set for `#sqrt`, `#fma`, `#memcpy`,
    //     `#clz`, etc. Unset (`llvm::Intrinsic::not_intrinsic`) for
    //     `#min`, `#max`, `#fence`, `#pause`, every `#atomic_*`, every
    //     `#simd_*`, and every Lucid-side intrinsic.
    //
    //   - `info.emitterKind` — which emitter file owns the codegen.
    //     `LLVM` for anything that lowers to a native LLVM construct
    //     (a real IR instruction or an `Intrinsic::ID`); `Lucid` for
    //     anything CodeGen implements directly.
    //
    // These are deliberately different axes — see the long comment in
    // `IntrinsicRegistry.hpp` for why. `#min` has no `llvmID` (LLVM
    // has no `min` intrinsic) but is still emitted by
    // `LLVMIntrinsicEmitter.cpp` as a cmp + select. The dispatch here
    // must be on `emitterKind`, NOT on `llvmID`.
    //
    // Dispatching on `llvmID` (as an earlier revision of this file did)
    // routes `#min`, `#fence`, every `#atomic_*`, and every `#simd_*`
    // to the Lucid emitter, which does not handle them.
    if (info->emitterKind == IntrinsicEmitterKind::LLVM) {
        return emitLLVMIntrinsic(expr, *info, emitter);
    }

    return emitLucidIntrinsic(expr, *info, emitter);
}

} // namespace codegen