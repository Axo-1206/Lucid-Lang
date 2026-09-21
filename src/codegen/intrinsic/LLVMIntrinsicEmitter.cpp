/// @file codegen/intrinsic/LLVMIntrinsicEmitter.cpp
/// @brief Implementation of the LLVM-side intrinsic emitters.
///
/// ─── Two Paths ────────────────────────────────────────────────────────────
/// `emitLLVMIntrinsic` handles every intrinsic the registry marks with
/// `emitterKind == IntrinsicEmitterKind::LLVM`. That set splits in two:
///
///   - **Literal ID** (`info.llvmID.has_value()`): the intrinsic maps to
///     an `llvm::Intrinsic::ID`. `#sqrt`, `#fma`, `#memcpy`, `#clz`,
///     `#popcount`, `#bswap`. Lowered by `llvm::Intrinsic::getDeclaration`
///     + `CreateCall`.
///
///   - **No literal ID** (`info.llvmID == llvm::Intrinsic::not_intrinsic`):
///     the intrinsic still lowers to a native LLVM construct, but there's
///     no `Intrinsic::ID` for it. `#min` and `#max` (cmp + select),
///     `#fence` (`CreateFence`), `#pause`, every `#atomic_*`
///     (`LoadInst`/`StoreInst`/`AtomicRMWInst`/`AtomicCmpXchgInst`), every
///     `#simd_*` (vector ops). These are dispatched on `info.kind` after
///     the literal-ID path declines.
///
/// The registry's `IntrinsicEmitterKind` doc comment explains why these
/// are two separate axes and why both belong in this file.
///
/// ─── Special-Case Argument Lowering ───────────────────────────────────────
/// Two SIMD intrinsics take a **type argument** in position 0:
/// `#simd_splat(T, n, x)` and `#simd_load(ptr, n)`. `#simd_splat`'s arg 0
/// is a type; `#simd_load`'s type comes from `expr->resolvedType`. Both
/// are handled by their own emitters before the general path.
///
/// ─── Overload Type Lists ──────────────────────────────────────────────────
/// `llvm::Intrinsic::getDeclaration` takes a `SmallVector<Type*>` of
/// overload types, one per overload slot in the intrinsic's signature.
/// For `#sqrt(x)` that's `{x->getType()}`. For `#fma(a,b,c)` it's
/// `{a->getType()}` — ONE slot, not three, because `llvm.fma` matches all
/// three operands to a single overload slot via `LLVMMatchType<0>`.
///
/// Passing one type per argument (as an earlier revision did) fails
/// `getDeclaration`'s arity check for any multi-argument intrinsic. The
/// dispatch below groups intrinsics by their overload arity.
///
/// ─── `Own::Owned` vs `Own::Borrowed` ──────────────────────────────────────
/// Every LLVM-side intrinsic produces a fresh value — they have no side
/// effects on Lucid-owned storage, except for the memory intrinsics
/// (`#memcpy`, `#memset`, `#memmove`) and `#atomic_store`/`#simd_store`,
/// which write through a pointer and return void.
///
/// For pass 1, every non-void result is tagged `Own::Owned`. `#simd_extract`
/// on a borrowed vector is technically `Borrowed`, but the redundancy is
/// a no-op retain (integers, floats, and vectors have no resource kind).
/// Fix in pass 2 if the ownership checker complains.

#include "LLVMIntrinsicEmitter.hpp"

#include "codegen/emit/Emitter.hpp"
#include "codegen/Program.hpp"
#include "codegen/Types.hpp"

#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Anonymous-namespace helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// ─── Diagnostics helpers ──────────────────────────────────────────────────

Val argCountError(IntrinsicCallExprAST* expr,
                  const char* expected,
                  Emitter& emitter) {
    emitter.program.diagnostics.errorAt(
        DiagCode::Sem_ArgCountMismatch, expr->loc,
        "intrinsic '#", emitter.program.pool.lookup(expr->intrinsicName),
        "' requires ", expected, " argument(s)");
    return {};
}

Val typeMismatchError(IntrinsicCallExprAST* expr,
                      const char* detail,
                      Emitter& emitter) {
    emitter.program.diagnostics.errorAt(
        DiagCode::Sem_TypeMismatch, expr->loc,
        "intrinsic '#", emitter.program.pool.lookup(expr->intrinsicName),
        "': ", detail);
    return {};
}

/// Wrap an LLVM result value as an `Owned` `Val`.
Val wrapResult(llvm::Value* value, IntrinsicCallExprAST* expr) {
    if (!value) return {};
    Val out;
    out.v = value;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

// ─── AST string-literal recovery ──────────────────────────────────────────
//
// Several intrinsics take a string argument that is semantically a
// compile-time constant (e.g. the ordering name in `#atomic_add` and
// `#fence`). By the time the args are lowered, the string is a runtime
// `{ptr, len, cap}` value, and the text is no longer recoverable from
// the LLVM value. Read it from the AST instead.

bool tryGetStringLiteralArg(IntrinsicCallExprAST* expr,
                            size_t index,
                            Emitter& emitter,
                            std::string& outStr) {
    if (!expr || index >= expr->args.size()) return false;
    ExprAST* argExpr = expr->args[index];
    if (argExpr->isa<LiteralExprAST>()) {
        LiteralExprAST* lit = argExpr->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::String
            || lit->kind == LiteralKind::RawString) {
            outStr = emitter.program.pool.lookup(lit->value);
            return true;
        }
    }
    return false;
}

// ─── Argument lowering ────────────────────────────────────────────────────
//
// Lower one argument through the emitter. The result is an SSA value
// already loaded from any lvalue; ownership tag is discarded because
// LLVM-side intrinsics consume value arguments.
//
// `expectedTy`, if non-null, is the LLVM parameter type the intrinsic's
// declaration expects for this position. Coercion is done when the
// emitter's natural type differs (integer width mismatch, pointer cast).
llvm::Value* lowerArg(ExprAST* argExpr,
                      llvm::Type* expectedTy,
                      Emitter& emitter) {
    if (!argExpr) return nullptr;

    Val v = emitter.emit(argExpr);
    if (!v.isValid()) return nullptr;

    llvm::Value* value = v.v;
    if (expectedTy && value->getType() != expectedTy) {
        value = emitter.coerceValueToType(
            value, expectedTy, emitter.program.builder());
    }
    return value;
}

/// Lower every argument of an LLVM-mapped intrinsic, coercing each to the
/// corresponding parameter type of the resolved declaration.
std::vector<llvm::Value*> lowerArgsAgainstDecl(IntrinsicCallExprAST* expr,
                                                llvm::Function* decl,
                                                Emitter& emitter) {
    std::vector<llvm::Value*> args;
    if (!expr || !decl) return args;

    llvm::FunctionType* fnTy = decl->getFunctionType();
    args.reserve(expr->args.size());

    for (size_t i = 0; i < expr->args.size(); ++i) {
        llvm::Type* paramTy = (i < fnTy->getNumParams())
            ? fnTy->getParamType(i)
            : nullptr;
        llvm::Value* v = lowerArg(expr->args[i], paramTy, emitter);
        if (!v) return {};
        args.push_back(v);
    }

    return args;
}

/// Derive the overload-type list for `llvm::Intrinsic::getDeclaration`
/// from an intrinsic's kind and its arguments.
///
/// Most intrinsics have one overload slot driven by argument 0. Binary
/// math has one slot shared by both operands (LLVMMatchType). SIMD
/// arithmetic has one slot driven by the vector argument. The memory
/// intrinsics have overloads on their pointer arguments.
std::vector<llvm::Type*> overloadTypesFor(IntrinsicKind kind,
                                          IntrinsicCallExprAST* expr,
                                          Emitter& emitter) {
    std::vector<llvm::Type*> types;

    switch (kind) {
        // ─── Unary math / bit ops: one slot on arg 0 ─────────────────────
        case IntrinsicKind::Sqrt:
        case IntrinsicKind::Abs:
        case IntrinsicKind::Ceil:
        case IntrinsicKind::Floor:
        case IntrinsicKind::Round:
        case IntrinsicKind::Trunc:
        case IntrinsicKind::Clz:
        case IntrinsicKind::Ctz:
        case IntrinsicKind::Popcount:
        case IntrinsicKind::Bswap: {
            if (!expr->args.empty()) {
                if (llvm::Type* t = emitter.program.types().get(
                        expr->args[0]->resolvedType)) {
                    types.push_back(t);
                }
            }
            break;
        }

        // ─── Binary math: single overload slot (LLVMMatchType) ───────────
        case IntrinsicKind::Fma:
        case IntrinsicKind::Pow: {
            if (!expr->args.empty()) {
                if (llvm::Type* t = emitter.program.types().get(
                        expr->args[0]->resolvedType)) {
                    types.push_back(t);
                }
            }
            break;
        }

        // ─── Memory: overload on pointer types ───────────────────────────
        //
        // llvm.memcpy/memmove/memset are overloaded on their pointer
        // parameter (one slot, all pointers match) plus an integer length
        // slot. For `getDeclaration`, pass the pointer type of arg 0.
        case IntrinsicKind::Memcpy:
        case IntrinsicKind::Memmove:
        case IntrinsicKind::Memset: {
            if (!expr->args.empty()) {
                if (llvm::Type* t = emitter.program.types().get(
                        expr->args[0]->resolvedType)) {
                    types.push_back(t);
                }
            }
            break;
        }

        // ─── Prefetch: overload on pointer type ──────────────────────────
        case IntrinsicKind::Prefetch:
        case IntrinsicKind::PrefetchR:
        case IntrinsicKind::PrefetchW: {
            if (!expr->args.empty()) {
                if (llvm::Type* t = emitter.program.types().get(
                        expr->args[0]->resolvedType)) {
                    types.push_back(t);
                }
            }
            break;
        }

        default:
            break;
    }

    return types;
}

// ─── CPU hint helpers ─────────────────────────────────────────────────────

/// Parse a memory-ordering name into an `llvm::AtomicOrdering`.
///
/// Mirrors `parseAtomicOrdering` from `LLVMTypeHelpers.hpp` but recovers
/// the string from the AST rather than an LLVM value. Returns
/// `SequentiallyConsistent` when no ordering is given (the conservative
/// default) or when the name is unrecognized (Sema validates names).
llvm::AtomicOrdering parseOrderingFromAST(IntrinsicCallExprAST* expr,
                                          size_t argIndex,
                                          Emitter& emitter) {
    std::string orderStr;
    if (!tryGetStringLiteralArg(expr, argIndex, emitter, orderStr)) {
        return llvm::AtomicOrdering::SequentiallyConsistent;
    }
    using llvm::AtomicOrdering;
    if (orderStr == "relaxed") return AtomicOrdering::Monotonic;
    if (orderStr == "acquire") return AtomicOrdering::Acquire;
    if (orderStr == "release") return AtomicOrdering::Release;
    if (orderStr == "acq_rel") return AtomicOrdering::AcquireRelease;
    if (orderStr == "seq_cst") return AtomicOrdering::SequentiallyConsistent;
    return AtomicOrdering::SequentiallyConsistent;
}

// ─── Atomic helpers ───────────────────────────────────────────────────────

/// Recover the pointee LLVM type for an atomic's pointer argument.
///
/// With opaque pointers, LLVM's `LoadInst`/`StoreInst`/`AtomicRMWInst`
/// need the element type explicitly. The AST carries it in
/// `PtrTypeAST::inner`. Fall back to the intrinsic's `resolvedType`
/// (which for `#atomic_load` is `T`, the loaded value's type).
llvm::Type* pointeeTypeForAtomic(IntrinsicCallExprAST* expr,
                                 size_t argIndex,
                                 Emitter& emitter) {
    if (!expr || argIndex >= expr->args.size()) return nullptr;

    TypeAST* ptrTy = expr->args[argIndex]->resolvedType;
    if (ptrTy && ptrTy->isa<PtrTypeAST>()) {
        TypeAST* pointee = ptrTy->as<PtrTypeAST>()->inner;
        if (llvm::Type* resolved = emitter.program.types().get(pointee)) {
            return resolved;
        }
    }

    // Fall back to the intrinsic's own resolved type.
    if (expr->resolvedType) {
        if (llvm::Type* resolved = emitter.program.types().get(
                expr->resolvedType)) {
            return resolved;
        }
    }

    return nullptr;
}

// ─── SIMD helpers ─────────────────────────────────────────────────────────

/// Recover the vector LLVM type for a SIMD intrinsic from its resolved
/// type. Sema guarantees the intrinsic's `resolvedType` is a `SimdTypeAST`.
llvm::VectorType* vectorTypeForSimd(IntrinsicCallExprAST* expr,
                                    Emitter& emitter) {
    if (!expr || !expr->resolvedType) return nullptr;
    llvm::Type* llvmTy = emitter.program.types().get(expr->resolvedType);
    if (!llvmTy || !llvmTy->isVectorTy()) return nullptr;
    return llvm::cast<llvm::VectorType>(llvmTy);
}

/// Emit a runtime bounds check for a SIMD lane index.
///
/// On failure, emits a panic and `unreachable`; on success, leaves the
/// builder at the continue block. Returns the (possibly cast) index value.
///
/// `TODO(null-coalesce)`: the old code branched to the `??` fallback on
/// failure. The new `Emitter` has no null-coalesce state yet; this always
/// panics. See the same TODO in `LucidIntrinsicEmitter.cpp::emitToRef`.
llvm::Value* emitSimdBoundsCheck(llvm::Value* index,
                                 uint64_t laneCount,
                                 const char* operation,
                                 SourceLocation loc,
                                 Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LLVMContext& ctx = emitter.program.llvmContext();

    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    if (index->getType() != i32Ty) {
        index = irb.CreateIntCast(index, i32Ty, /*isSigned=*/true,
                                  "simd_idx_cast");
    }

    llvm::Value* laneCountVal = llvm::ConstantInt::get(i32Ty, laneCount);
    llvm::Value* inBounds = irb.CreateICmpULT(
        index, laneCountVal, "simd_in_bounds");

    llvm::Function* func = irb.GetInsertBlock()->getParent();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx, "simd_idx_continue", func);
    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
        ctx, "simd_idx_panic", func);

    irb.CreateCondBr(inBounds, continueBlock, panicBlock);

    irb.SetInsertPoint(panicBlock);
    emitter.emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, loc);
    // emitPanic terminates with `unreachable`.

    irb.SetInsertPoint(continueBlock);
    return index;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Literal-ID path
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Emit an intrinsic that maps to an `llvm::Intrinsic::ID`.
///
/// The declaration is resolved via `getDeclaration` with the overload
/// types derived from the intrinsic's kind. Then every AST argument is
/// lowered against the resolved declaration's parameter types, and the
/// call is emitted.
///
/// Extra parameters that the AST does not supply (the `isvolatile` flag
/// on the memory intrinsics, the `is_int_min_poison` flag on `llvm.abs`)
/// are appended by the per-kind helpers below before this function is
/// called, so this function never sees an arity mismatch.
Val emitMappedIntrinsic(IntrinsicCallExprAST* expr,
                        const IntrinsicInfo& info,
                        Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LLVMContext& ctx = emitter.program.llvmContext();

    // ─── Resolve the declaration ──────────────────────────────────────────
    std::vector<llvm::Type*> overloadTypes =
        overloadTypesFor(info.kind, expr, emitter);

    llvm::Function* decl = llvm::Intrinsic::getDeclaration(
        &emitter.program.module(), info.llvmID, overloadTypes);
    if (!decl) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "could not resolve LLVM intrinsic for '#",
            emitter.program.pool.lookup(expr->intrinsicName), "'");
        return {};
    }

    // ─── Lower the AST arguments ──────────────────────────────────────────
    std::vector<llvm::Value*> args =
        lowerArgsAgainstDecl(expr, decl, emitter);
    if (args.size() != expr->args.size()) return {};

    // ─── Append intrinsic-specific extra parameters ───────────────────────
    //
    // Several LLVM intrinsics take parameters the AST call doesn't supply.
    // The canonical values for each are added here.
    switch (info.llvmID) {
        // llvm.abs(x, is_int_min_poison): saturate rather than poison
        // at INT_MIN. Sema's `#abs` should have already done this for
        // integer arguments, but the mapped path handles the case where
        // `#abs` was routed here for a float.
        case llvm::Intrinsic::abs: {
            args.push_back(llvm::ConstantInt::getFalse(ctx));
            break;
        }

        // llvm.ctlz(x, is_zero_undef) / llvm.cttz(x, is_zero_undef):
        // return the bit width (not poison) for zero input.
        case llvm::Intrinsic::ctlz:
        case llvm::Intrinsic::cttz: {
            args.push_back(llvm::ConstantInt::getFalse(ctx));
            break;
        }

        // llvm.memcpy/memmove(dst, src, len, isvolatile)
        // llvm.memset(dst, val, len, isvolatile)
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memmove:
        case llvm::Intrinsic::memset: {
            args.push_back(llvm::ConstantInt::getFalse(ctx));
            break;
        }

        // llvm.prefetch(ptr, rw, locality, cachetype): the AST supplies
        // only the pointer. Sema populates `#prefetch_r`/`#prefetch_w`
        // with the right intrinsic kind; the rw/locality/cachetype
        // values are canonical here.
        case llvm::Intrinsic::prefetch: {
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
            int rw = (info.kind == IntrinsicKind::PrefetchW) ? 1 : 0;
            args.push_back(llvm::ConstantInt::get(i32Ty, rw));      // rw
            args.push_back(llvm::ConstantInt::get(i32Ty, 3));       // locality
            args.push_back(llvm::ConstantInt::get(i32Ty, 0));       // cachetype
            break;
        }

        default:
            break;
    }

    // ─── Emit the call ────────────────────────────────────────────────────
    llvm::CallInst* call = irb.CreateCall(
        decl, args, emitter.program.pool.lookup(expr->intrinsicName));

    // ─── Apply call-site attributes the intrinsic's semantics require ─────
    //
    // The memory intrinsics need an alignment attribute on their pointer
    // arguments. The AST does not carry an alignment, so use the natural
    // alignment of the pointee type when known, 1 otherwise.
    //
    // TODO: when Sema records an alignment for `#memcpy<T>(...)` etc.,
    // thread it through here instead of guessing.
    switch (info.llvmID) {
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memmove: {
            call->addParamAttr(0, llvm::Attribute::getWithAlignment(
                ctx, llvm::Align(1)));
            call->addParamAttr(1, llvm::Attribute::getWithAlignment(
                ctx, llvm::Align(1)));
            break;
        }
        case llvm::Intrinsic::memset: {
            call->addParamAttr(0, llvm::Attribute::getWithAlignment(
                ctx, llvm::Align(1)));
            break;
        }
        default:
            break;
    }

    // ─── Return ───────────────────────────────────────────────────────────
    if (call->getType()->isVoidTy()) {
        return {};
    }
    return wrapResult(call, expr);
}

// ─── #min / #max ──────────────────────────────────────────────────────────

/// `#min(a, b)` / `#max(a, b)`. Integer: `icmp` + `select`. Float:
/// `llvm.minimum`/`llvm.maximum`.
Val emitMinMax(IntrinsicCallExprAST* expr,
               const IntrinsicInfo& info,
               Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    if (!a.isValid() || !b.isValid()) return {};

    if (a.v->getType() != b.v->getType()) {
        return typeMismatchError(expr, "arguments must have the same type",
                                 emitter);
    }

    llvm::IRBuilder<>& irb = emitter.program.builder();
    bool isMin = (info.kind == IntrinsicKind::Min);

    if (a.v->getType()->isIntegerTy()) {
        llvm::CmpInst::Predicate pred = isMin
            ? llvm::CmpInst::ICMP_SLT
            : llvm::CmpInst::ICMP_SGT;
        llvm::Value* cmp = irb.CreateICmp(pred, a.v, b.v, "minmax_cmp");
        llvm::Value* result = irb.CreateSelect(cmp, a.v, b.v, "minmax_sel");
        return wrapResult(result, expr);
    }

    if (a.v->getType()->isFloatingPointTy()) {
        llvm::Intrinsic::ID id = isMin
            ? llvm::Intrinsic::minimum
            : llvm::Intrinsic::maximum;
        llvm::Function* decl = llvm::Intrinsic::getDeclaration(
            &emitter.program.module(), id, {a.v->getType()});
        if (!decl) {
            return typeMismatchError(
                expr, "could not resolve float min/max intrinsic", emitter);
        }
        llvm::Value* result = irb.CreateCall(decl, {a.v, b.v});
        return wrapResult(result, expr);
    }

    return typeMismatchError(expr, "arguments must be numeric", emitter);
}

// ─── #fence / #pause ──────────────────────────────────────────────────────

/// `#fence(ordering)` — `CreateFence` with the ordering parsed from the
/// AST. `#pause()` — a `seq_cst` fence (the old code's shape; LLVM has no
/// first-class pause intrinsic on all targets, and a fence is the
/// portable fallback).
Val emitFence(IntrinsicCallExprAST* expr,
              const IntrinsicInfo& info,
              Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();

    if (info.kind == IntrinsicKind::Pause) {
        irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return {};
    }

    // Fence.
    llvm::AtomicOrdering ordering = parseOrderingFromAST(
        expr, /*argIndex=*/0, emitter);
    irb.CreateFence(ordering);
    return {};
}

// ─── Atomics ──────────────────────────────────────────────────────────────

/// `#atomic_load(ptr, ordering)`.
Val emitAtomicLoad(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() < 1) {
        return argCountError(expr, "1 or 2", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    if (!ptr.isValid()) return {};

    llvm::Type* elemTy = pointeeTypeForAtomic(expr, 0, emitter);
    if (!elemTy) {
        return typeMismatchError(
            expr, "could not determine pointee type", emitter);
    }

    llvm::AtomicOrdering ordering = parseOrderingFromAST(
        expr, /*argIndex=*/1, emitter);

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LoadInst* load = irb.CreateLoad(elemTy, ptr.v, "atomic_load");
    load->setAtomic(ordering);

    return wrapResult(load, expr);
}

/// `#atomic_store(ptr, value, ordering)`.
Val emitAtomicStore(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() < 2) {
        return argCountError(expr, "2 or 3", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    Val val = emitter.emit(expr->args[1]);
    if (!ptr.isValid() || !val.isValid()) return {};

    llvm::AtomicOrdering ordering = parseOrderingFromAST(
        expr, /*argIndex=*/2, emitter);

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::StoreInst* store = irb.CreateStore(val.v, ptr.v);
    store->setAtomic(ordering);
    return {};
}

/// `#atomic_add/sub/and/or/xor(ptr, value, ordering) -> old_value`.
Val emitAtomicRMW(IntrinsicCallExprAST* expr,
                  const IntrinsicInfo& info,
                  Emitter& emitter) {
    if (expr->args.size() < 2) {
        return argCountError(expr, "2 or 3", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    Val val = emitter.emit(expr->args[1]);
    if (!ptr.isValid() || !val.isValid()) return {};

    llvm::AtomicRMWInst::BinOp op;
    switch (info.kind) {
        case IntrinsicKind::AtomicAdd: op = llvm::AtomicRMWInst::Add; break;
        case IntrinsicKind::AtomicSub: op = llvm::AtomicRMWInst::Sub; break;
        case IntrinsicKind::AtomicAnd: op = llvm::AtomicRMWInst::And; break;
        case IntrinsicKind::AtomicOr:  op = llvm::AtomicRMWInst::Or;  break;
        case IntrinsicKind::AtomicXor: op = llvm::AtomicRMWInst::Xor; break;
        default:
            return typeMismatchError(expr, "unknown atomic RMW op", emitter);
    }

    llvm::AtomicOrdering ordering = parseOrderingFromAST(
        expr, /*argIndex=*/2, emitter);

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Value* result = irb.CreateAtomicRMW(
        op, ptr.v, val.v, llvm::MaybeAlign(), ordering);

    return wrapResult(result, expr);
}

/// `#atomic_cas(ptr, expected, desired, ordering) -> bool` (success flag).
Val emitAtomicCas(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() < 3) {
        return argCountError(expr, "3 or 4", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    Val expected = emitter.emit(expr->args[1]);
    Val desired = emitter.emit(expr->args[2]);
    if (!ptr.isValid() || !expected.isValid() || !desired.isValid()) {
        return {};
    }

    llvm::AtomicOrdering ordering = parseOrderingFromAST(
        expr, /*argIndex=*/3, emitter);

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::AtomicCmpXchgInst* cas = irb.CreateAtomicCmpXchg(
        ptr.v, expected.v, desired.v,
        llvm::MaybeAlign(),
        ordering,
        llvm::AtomicOrdering::SequentiallyConsistent);

    // Field 1 is the "success" i1 flag.
    llvm::Value* success = irb.CreateExtractValue(cas, 1, "cas_success");
    return wrapResult(success, expr);
}

// ─── SIMD ─────────────────────────────────────────────────────────────────

/// `#simd_add/sub/mul/div(a, b)` — lane-wise arithmetic.
Val emitSimdArith(IntrinsicCallExprAST* expr,
                  const IntrinsicInfo& info,
                  Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    if (!a.isValid() || !b.isValid()) return {};

    if (a.v->getType() != b.v->getType()) {
        return typeMismatchError(expr, "arguments must have the same type",
                                 emitter);
    }
    if (!a.v->getType()->isVectorTy()) {
        return typeMismatchError(expr, "arguments must be vector types",
                                 emitter);
    }

    llvm::IRBuilder<>& irb = emitter.program.builder();
    bool isFloat = a.v->getType()->getScalarType()->isFloatingPointTy();

    llvm::Value* result = nullptr;
    switch (info.kind) {
        case IntrinsicKind::SimdAdd:
            result = isFloat ? irb.CreateFAdd(a.v, b.v)
                             : irb.CreateAdd(a.v, b.v);
            break;
        case IntrinsicKind::SimdSub:
            result = isFloat ? irb.CreateFSub(a.v, b.v)
                             : irb.CreateSub(a.v, b.v);
            break;
        case IntrinsicKind::SimdMul:
            result = isFloat ? irb.CreateFMul(a.v, b.v)
                             : irb.CreateMul(a.v, b.v);
            break;
        case IntrinsicKind::SimdDiv:
            result = isFloat ? irb.CreateFDiv(a.v, b.v)
                             : irb.CreateSDiv(a.v, b.v);
            break;
        default:
            return {};
    }
    return wrapResult(result, expr);
}

/// `#simd_fma(a, b, c)` — fused multiply-add on vectors.
Val emitSimdFma(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 3) {
        return argCountError(expr, "3", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    Val c = emitter.emit(expr->args[2]);
    if (!a.isValid() || !b.isValid() || !c.isValid()) return {};

    if (a.v->getType() != b.v->getType() || a.v->getType() != c.v->getType()) {
        return typeMismatchError(
            expr, "all arguments must have the same type", emitter);
    }
    if (!a.v->getType()->isVectorTy()) {
        return typeMismatchError(expr, "arguments must be vector types",
                                 emitter);
    }

    llvm::Function* decl = llvm::Intrinsic::getDeclaration(
        &emitter.program.module(),
        llvm::Intrinsic::fma,
        {a.v->getType()});
    if (!decl) {
        return typeMismatchError(
            expr, "could not resolve vector fma intrinsic", emitter);
    }

    llvm::Value* result = emitter.program.builder().CreateCall(
        decl, {a.v, b.v, c.v});
    return wrapResult(result, expr);
}

/// `#simd_min/max(a, b)` — lane-wise min/max.
Val emitSimdMinMax(IntrinsicCallExprAST* expr,
                   const IntrinsicInfo& info,
                   Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    if (!a.isValid() || !b.isValid()) return {};

    if (a.v->getType() != b.v->getType()) {
        return typeMismatchError(expr, "arguments must have the same type",
                                 emitter);
    }
    if (!a.v->getType()->isVectorTy()) {
        return typeMismatchError(expr, "arguments must be vector types",
                                 emitter);
    }

    llvm::IRBuilder<>& irb = emitter.program.builder();
    bool isInt = a.v->getType()->getScalarType()->isIntegerTy();
    bool isMin = (info.kind == IntrinsicKind::SimdMin);

    llvm::CmpInst::Predicate pred;
    if (isInt) {
        pred = isMin ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_SGT;
    } else {
        pred = isMin ? llvm::CmpInst::FCMP_OLT : llvm::CmpInst::FCMP_OGT;
    }

    llvm::Value* cmp = isInt
        ? irb.CreateICmp(pred, a.v, b.v, "simd_minmax_cmp")
        : irb.CreateFCmp(pred, a.v, b.v, "simd_minmax_cmp");
    llvm::Value* result = irb.CreateSelect(cmp, a.v, b.v, "simd_minmax_sel");

    return wrapResult(result, expr);
}

/// `#simd_splat(T, lanes, scalar)` — broadcast a scalar across a vector.
///
/// Arg 0 is a type; arg 1 is `lanes` (a value, but Sema constrains it to
/// match `T`'s lane count); arg 2 is the scalar.
Val emitSimdSplat(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 3) {
        return argCountError(expr, "3 (type, lanes, scalar)", emitter);
    }

    llvm::VectorType* vecTy = vectorTypeForSimd(expr, emitter);
    if (!vecTy) {
        return typeMismatchError(
            expr, "resolved type is not a vector", emitter);
    }

    // Lower the scalar (arg 2). The `lanes` argument (arg 1) is a runtime
    // value Sema has already validated against the vector's lane count.
    Val scalar = emitter.emit(expr->args[2]);
    if (!scalar.isValid()) return {};

    llvm::Type* elemTy = vecTy->getElementType();
    llvm::Value* scalarVal = scalar.v;
    if (scalarVal->getType() != elemTy) {
        scalarVal = emitter.coerceValueToType(
            scalarVal, elemTy, emitter.program.builder());
    }

    llvm::Value* splat = emitter.program.builder().CreateVectorSplat(
        vecTy->getElementCount(), scalarVal, "simd_splat");
    return wrapResult(splat, expr);
}

/// `#simd_load(ptr, lanes)` — load a vector from a pointer.
///
/// The vector type comes from `expr->resolvedType`. `lanes` is a runtime
/// value Sema has validated.
Val emitSimdLoad(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (ptr, lanes)", emitter);
    }

    llvm::VectorType* vecTy = vectorTypeForSimd(expr, emitter);
    if (!vecTy) {
        return typeMismatchError(
            expr, "resolved type is not a vector", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    if (!ptr.isValid()) return {};

    if (!ptr.v->getType()->isPointerTy()) {
        return typeMismatchError(expr, "first argument must be a pointer",
                                 emitter);
    }

    llvm::Value* loaded = emitter.program.builder().CreateLoad(
        vecTy, ptr.v, "simd_load");
    return wrapResult(loaded, expr);
}

/// `#simd_store(ptr, value)` — store a vector through a pointer.
Val emitSimdStore(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (ptr, value)", emitter);
    }

    Val ptr = emitter.emit(expr->args[0]);
    Val val = emitter.emit(expr->args[1]);
    if (!ptr.isValid() || !val.isValid()) return {};

    if (!ptr.v->getType()->isPointerTy()) {
        return typeMismatchError(expr, "first argument must be a pointer",
                                 emitter);
    }
    if (!val.v->getType()->isVectorTy()) {
        return typeMismatchError(expr, "second argument must be a vector",
                                 emitter);
    }

    emitter.program.builder().CreateStore(val.v, ptr.v);
    return {};
}

/// `#simd_extract(vec, index) -> scalar`.
Val emitSimdExtract(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (vector, index)", emitter);
    }

    Val vec = emitter.emit(expr->args[0]);
    Val idx = emitter.emit(expr->args[1]);
    if (!vec.isValid() || !idx.isValid()) return {};

    if (!vec.v->getType()->isVectorTy()) {
        return typeMismatchError(
            expr, "first argument must be a vector", emitter);
    }

    llvm::VectorType* vecTy = llvm::cast<llvm::VectorType>(vec.v->getType());
    uint64_t laneCount = vecTy->getElementCount().getKnownMinValue();

    llvm::Value* checkedIdx = emitSimdBoundsCheck(
        idx.v, laneCount, "extract", expr->loc, emitter);

    llvm::Value* result = emitter.program.builder().CreateExtractElement(
        vec.v, checkedIdx, "simd_extract");
    return wrapResult(result, expr);
}

/// `#simd_insert(vec, index, value) -> vector`.
Val emitSimdInsert(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 3) {
        return argCountError(expr, "3 (vector, index, value)", emitter);
    }

    Val vec = emitter.emit(expr->args[0]);
    Val idx = emitter.emit(expr->args[1]);
    Val val = emitter.emit(expr->args[2]);
    if (!vec.isValid() || !idx.isValid() || !val.isValid()) return {};

    if (!vec.v->getType()->isVectorTy()) {
        return typeMismatchError(
            expr, "first argument must be a vector", emitter);
    }

    llvm::VectorType* vecTy = llvm::cast<llvm::VectorType>(vec.v->getType());
    llvm::Type* elemTy = vecTy->getElementType();
    if (val.v->getType() != elemTy) {
        return typeMismatchError(
            expr, "value type must match the vector element type", emitter);
    }

    uint64_t laneCount = vecTy->getElementCount().getKnownMinValue();
    llvm::Value* checkedIdx = emitSimdBoundsCheck(
        idx.v, laneCount, "insert", expr->loc, emitter);

    llvm::Value* result = emitter.program.builder().CreateInsertElement(
        vec.v, val.v, checkedIdx, "simd_insert");
    return wrapResult(result, expr);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

Val emitLLVMIntrinsic(IntrinsicCallExprAST* expr,
                      const IntrinsicInfo& info,
                      Emitter& emitter) {
    if (!expr) return {};

    // ─── Literal-ID path ──────────────────────────────────────────────────
    //
    // Intrinsics with an `llvm::Intrinsic::ID` go through the generic
    // `getDeclaration` + `CreateCall` path. Exceptions are `#abs`, `#min`,
    // `#max`, which have special argument shapes even when they have IDs.
    if (info.isValid()) {
        switch (info.kind) {
            case IntrinsicKind::Abs:
                // Falls through to the generic path below; `#abs` over an
                // integer argument needs the extra `is_int_min_poison`
                // parameter, which `emitMappedIntrinsic` appends.
            case IntrinsicKind::Sqrt:
            case IntrinsicKind::Fma:
            case IntrinsicKind::Ceil:
            case IntrinsicKind::Floor:
            case IntrinsicKind::Round:
            case IntrinsicKind::Trunc:
            case IntrinsicKind::Pow:
            case IntrinsicKind::Memcpy:
            case IntrinsicKind::Memmove:
            case IntrinsicKind::Memset:
            case IntrinsicKind::Clz:
            case IntrinsicKind::Ctz:
            case IntrinsicKind::Popcount:
            case IntrinsicKind::Bswap:
            case IntrinsicKind::Prefetch:
            case IntrinsicKind::PrefetchR:
            case IntrinsicKind::PrefetchW:
                return emitMappedIntrinsic(expr, info, emitter);

            default:
                // An intrinsic with an `Intrinsic::ID` and no case above
                // is a registry/dispatcher mismatch, not a user-facing
                // condition. Reaching here means the registry gained an ID
                // and the emitter didn't gain the corresponding case.
                emitter.program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, expr->loc,
                    "intrinsic '#",
                    emitter.program.pool.lookup(expr->intrinsicName),
                    "' has an llvm::Intrinsic::ID but no dispatch case "
                    "in emitLLVMIntrinsic()");
                return {};
        }
    }

    // ─── No literal-ID path ───────────────────────────────────────────────
    //
    // These are LLVM-emitter intrinsics without a first-class LLVM ID.
    // Each lowers to a specific IR construct via its own helper.
    switch (info.kind) {
        // ─── Math with no direct ID ───────────────────────────────────────
        case IntrinsicKind::Min:
        case IntrinsicKind::Max:
            return emitMinMax(expr, info, emitter);

        // ─── CPU hints ────────────────────────────────────────────────────
        case IntrinsicKind::Fence:
        case IntrinsicKind::Pause:
            return emitFence(expr, info, emitter);

        // ─── Atomics ──────────────────────────────────────────────────────
        case IntrinsicKind::AtomicLoad:
            return emitAtomicLoad(expr, emitter);
        case IntrinsicKind::AtomicStore:
            return emitAtomicStore(expr, emitter);
        case IntrinsicKind::AtomicAdd:
        case IntrinsicKind::AtomicSub:
        case IntrinsicKind::AtomicAnd:
        case IntrinsicKind::AtomicOr:
        case IntrinsicKind::AtomicXor:
            return emitAtomicRMW(expr, info, emitter);
        case IntrinsicKind::AtomicCas:
            return emitAtomicCas(expr, emitter);

        // ─── SIMD ─────────────────────────────────────────────────────────
        case IntrinsicKind::SimdAdd:
        case IntrinsicKind::SimdSub:
        case IntrinsicKind::SimdMul:
        case IntrinsicKind::SimdDiv:
            return emitSimdArith(expr, info, emitter);
        case IntrinsicKind::SimdFma:
            return emitSimdFma(expr, emitter);
        case IntrinsicKind::SimdMin:
        case IntrinsicKind::SimdMax:
            return emitSimdMinMax(expr, info, emitter);
        case IntrinsicKind::SimdSplat:
            return emitSimdSplat(expr, emitter);
        case IntrinsicKind::SimdLoad:
            return emitSimdLoad(expr, emitter);
        case IntrinsicKind::SimdStore:
            return emitSimdStore(expr, emitter);
        case IntrinsicKind::SimdExtract:
            return emitSimdExtract(expr, emitter);
        case IntrinsicKind::SimdInsert:
            return emitSimdInsert(expr, emitter);

        default:
            break;
    }

    // ─── Unreachable: every LLVM-emitter intrinsic has a case above ──────
    emitter.program.diagnostics.errorAt(
        DiagCode::Backend_CodegenError, expr->loc,
        "intrinsic '#", emitter.program.pool.lookup(expr->intrinsicName),
        "' is marked LLVM-emitter but has no dispatch case in "
        "emitLLVMIntrinsic() — registry/dispatcher mismatch");
    return {};
}

} // namespace codegen