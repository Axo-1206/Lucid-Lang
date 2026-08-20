/// @file IntrinsicEmitter.cpp
/// @brief Implementation of intrinsic dispatcher.

#include "IntrinsicEmitter.hpp"
#include "LLVMIntrinsicEmitter.hpp"
#include "LucidIntrinsicEmitter.hpp"
#include "../CodeGenType.hpp"
#include "../CodeGen.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../support/LLVMHelpers.hpp"
#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Intrinsics.h>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Value* emitIntrinsicFromAST(
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return nullptr;

    SourceLocation loc = expr->loc;
    const IntrinsicInfo* info = IntrinsicRegistry::getInstance(ctx.pool).getInfo(expr->intrinsicName);
    if (!info) {
        // Sema should already have rejected an unknown intrinsic name
        // before CodeGen ever sees it - this is defensive, not a
        // user-facing path, hence pool.lookup() here is fine (error path
        // only, not a hot dispatch comparison).
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                "unknown intrinsic '#", ctx.pool.lookup(expr->intrinsicName), "'");
        return nullptr;
    }

    // ─── Special-case intrinsics that need raw addresses ────────────────
    // These intrinsics should NOT load their arguments - dispatched by
    // IntrinsicKind (an enum, resolved once above), not by re-comparing
    // strings or InternedStrings against literals.
    switch (info->kind) {
        case IntrinsicKind::Addrof: {
            if (expr->args.empty()) {
                ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                       "intrinsic '#addrof' requires an argument");
                return nullptr;
            }
            // addrof(x) returns the address of x - do NOT load
            llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
            if (!argVal) return nullptr;
            // argVal should already be a pointer (l-value)
            return argVal;
        }

        case IntrinsicKind::ToRef: {
            if (expr->args.empty()) {
                ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                       "intrinsic '#toRef' requires an argument");
                return nullptr;
            }
            // toRef(ptr) - do NOT load, but add null check
            llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
            if (!argVal) return nullptr;
            return emitNullCheck(argVal, ctx);
        }

        case IntrinsicKind::Ptrstr: {
            if (expr->args.empty()) {
                ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                       "intrinsic '#ptrstr' requires an argument");
                return nullptr;
            }
            // ptrstr(x) formats x's memory address as a hex string - like
            // addrof, it needs the raw address itself, not x's value, so
            // it must NOT be loaded here.
            llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
            if (!argVal) return nullptr;
            return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
        }

        default:
            break;
    }

    // ─── Normal path: load lvalues ────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) return nullptr;

        if (arg->isLValue) {
            // Load with explicit element type
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (!elemType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, arg->loc,
                                       "cannot determine type of argument for '#",
                                       ctx.pool.lookup(expr->intrinsicName), "'");
                return nullptr;
            }
            argVal = loadIfNeeded(argVal, elemType, ctx);
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    return emitIntrinsic(expr->intrinsicName, args, expr, ctx);
}

llvm::Value* emitIntrinsic(
    InternedString name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    const IntrinsicInfo* info = IntrinsicRegistry::getInstance(ctx.pool).getInfo(name);
    if (!info) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                "unknown intrinsic '#", ctx.pool.lookup(name), "'");
        return nullptr;
    }

    // The individual emitter functions in LLVMIntrinsicEmitter.cpp/
    // LucidIntrinsicEmitter.cpp now take IntrinsicKind alongside name -
    // dispatch inside them switches on `kind`, `name` is passed through
    // only so they can build diagnostic message text without their own
    // pool.lookup(). nameStr is computed once, here, and threaded to
    // every case below - the same single-lookup shape as before.
    //
    // Note: IntrinsicKind::Addrof never reaches this switch - it's fully
    // handled and returned from within emitIntrinsicFromAST's own switch
    // above (it needs the raw, un-loaded address, same as ToRef/Ptrstr,
    // but unlike Ptrstr it never calls back into emitIntrinsic()). If
    // Addrof somehow does reach here, it deliberately has no case below
    // and falls to "unrecognized" - that's a caller bug, not something to
    // paper over with a case that would silently do the wrong thing.
    std::string nameStr = ctx.pool.lookup(name);

    // ─── Dispatch by emitter module ────────────────────────────────────────
    switch (info->emitterKind) {
        case IntrinsicEmitterKind::LLVM: {
            switch (info->kind) {
                // Floating-Point Math
                case IntrinsicKind::Sqrt:
                case IntrinsicKind::Abs:
                case IntrinsicKind::Fma:
                case IntrinsicKind::Ceil:
                case IntrinsicKind::Floor:
                case IntrinsicKind::Round:
                case IntrinsicKind::Pow:
                case IntrinsicKind::Min:
                case IntrinsicKind::Max: {
                    std::vector<llvm::Value*> mutableArgs = args;
                    return emitLLVMMathIntrinsic(info->kind, nameStr, mutableArgs, expr, ctx);
                }

                // Memory Operations
                case IntrinsicKind::Memcpy:
                case IntrinsicKind::Memmove:
                case IntrinsicKind::Memset:
                    return emitLLVMMemoryIntrinsic(info->kind, nameStr, args, expr, ctx);

                // Bit Manipulation
                case IntrinsicKind::Clz:
                case IntrinsicKind::Ctz:
                case IntrinsicKind::Popcount:
                case IntrinsicKind::Bswap:
                    return emitLLVMBitIntrinsic(info->kind, nameStr, args, expr, ctx);

                // Atomics
                case IntrinsicKind::AtomicLoad:
                case IntrinsicKind::AtomicStore:
                case IntrinsicKind::AtomicAdd:
                case IntrinsicKind::AtomicSub:
                case IntrinsicKind::AtomicAnd:
                case IntrinsicKind::AtomicOr:
                case IntrinsicKind::AtomicXor:
                case IntrinsicKind::AtomicCas:
                    return emitLLVMAtomicIntrinsic(info->kind, nameStr, args, expr, ctx);

                // SIMD
                case IntrinsicKind::SimdAdd:
                case IntrinsicKind::SimdSub:
                case IntrinsicKind::SimdMul:
                case IntrinsicKind::SimdDiv:
                case IntrinsicKind::SimdFma:
                case IntrinsicKind::SimdMin:
                case IntrinsicKind::SimdMax:
                case IntrinsicKind::SimdLoad:
                case IntrinsicKind::SimdStore:
                case IntrinsicKind::SimdSplat:
                case IntrinsicKind::SimdExtract:
                case IntrinsicKind::SimdInsert:
                    return emitLLVMSIMDIntrinsic(info->kind, nameStr, args, expr, ctx);

                // CPU Hints
                case IntrinsicKind::Prefetch:
                case IntrinsicKind::PrefetchR:
                case IntrinsicKind::PrefetchW:
                case IntrinsicKind::Fence:
                case IntrinsicKind::Pause:
                    return emitLLVMCPUHintIntrinsic(info->kind, nameStr, args, expr, ctx);

                default:
                    // An IntrinsicKind marked emitterKind=LLVM in the
                    // registry with no case here is a registry/dispatcher
                    // mismatch - a programmer error in this file, not a
                    // user-facing condition.
                    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                            "intrinsic '#", nameStr,
                                            "' is marked as an LLVM intrinsic but has "
                                            "no dispatch case in emitIntrinsic()");
                    return nullptr;
            }
        }

        case IntrinsicEmitterKind::Lucid: {
            switch (info->kind) {
                // Type Inspection
                case IntrinsicKind::Sizeof:
                case IntrinsicKind::Alignof:
                case IntrinsicKind::Typeof:
                case IntrinsicKind::Nameof:
                case IntrinsicKind::Tostr:
                case IntrinsicKind::Ptrstr:
                case IntrinsicKind::Bitcast:
                    return emitLucidTypeIntrinsic(info->kind, nameStr, args, expr, ctx);

                // Pointer Operations
                case IntrinsicKind::ToPtr:
                case IntrinsicKind::PtrOffset:
                case IntrinsicKind::PtrDiff:
                    return emitLucidPointerIntrinsic(info->kind, nameStr, args, expr, ctx);

                // Memory Management
                case IntrinsicKind::Alloc:
                case IntrinsicKind::Free:
                case IntrinsicKind::ArenaCreate:
                case IntrinsicKind::ArenaAlloc:
                case IntrinsicKind::ArenaReset:
                case IntrinsicKind::ArenaFree:
                    return emitLucidMemoryMgmtIntrinsic(info->kind, nameStr, args, expr, ctx);

                // String Operations
                case IntrinsicKind::StrLen:
                case IntrinsicKind::StrPtr:
                case IntrinsicKind::StrFromPtr:
                case IntrinsicKind::StrConcat:
                case IntrinsicKind::StrSlice:
                case IntrinsicKind::StrEq:
                case IntrinsicKind::StrByteAt:
                    return emitLucidStringIntrinsic(info->kind, nameStr, args, expr, ctx);

                // Control Flow
                case IntrinsicKind::Likely:
                case IntrinsicKind::Unlikely:
                case IntrinsicKind::ScopeExit:
                    return emitLucidControlIntrinsic(info->kind, nameStr, args, expr, ctx);

                default:
                    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                            "intrinsic '#", nameStr,
                                            "' is marked as a Lucid intrinsic but has "
                                            "no dispatch case in emitIntrinsic()");
                    return nullptr;
            }
        }
    }

    // Unreachable - IntrinsicEmitterKind has exactly two enumerators and
    // both are handled above. Kept only so a non-enum-aware compiler
    // doesn't warn on a missing return.
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "intrinsic '#", nameStr, "' has an unrecognized emitter kind");
    return nullptr;
}

} // namespace codegen