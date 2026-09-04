/// @file IntrinsicEmitter.cpp
/// @brief Implementation of intrinsic dispatcher.

#include "IntrinsicEmitter.hpp"
#include "LLVMIntrinsicEmitter.hpp"
#include "LucidIntrinsicEmitter.hpp"
#include "../types/CodeGenType.hpp"
#include "../CodeGen.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../types/LLVMTypeHelpers.hpp"
#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Intrinsics.h>

namespace codegen {

// ─── Helper: Determine if an intrinsic argument should NOT be loaded ────
// 
// Some intrinsics need raw addresses/pointers, not loaded values:
//   - addrof(x)      → needs the address of x
//   - toRef(ptr)     → needs the raw pointer (for null check)
//   - ptrstr(x)      → needs the raw address, not the value
//   - arena_alloc(arena, T, count) → needs arena descriptor pointer
//   - alloc(T, count) → count should be loaded, but type T is compile-time
//
// This helper returns true for arguments that should skip the load.
static bool shouldSkipLoadForArg(IntrinsicKind kind, size_t argIndex) {
    switch (kind) {
        // ─── These intrinsics need raw addresses ──────────────────────────
        case IntrinsicKind::Addrof:
        case IntrinsicKind::ToRef:
        case IntrinsicKind::Ptrstr:
            return true;  // All args should NOT be loaded
            
        // ─── All other intrinsics load their arguments normally ──────────
        default:
            return false;
    }
}

// ─── Helper: Get the expected argument type for an intrinsic ────────────
// 
// Some intrinsics have special type requirements that don't come from
// the argument's resolved type (e.g., arena_alloc's arena arg is i8*,
// but the AST says ArenaDescriptor*). This helper maps intrinsic kinds
// to the correct LLVM type for each argument.
static llvm::Type* getExpectedArgType(
    IntrinsicKind kind,
    size_t argIndex,
    CodeGenContext& ctx
) {
    switch (kind) {
        // ─── Alloc: count is i64 ──────────────────────────────────────────
        case IntrinsicKind::Alloc:
            if (argIndex == 0) {
                return llvm::Type::getInt64Ty(ctx.llvmCtx);
            }
            break;
            
        default:
            break;
    }
    return nullptr;  // Use the type from AST
}

// ─── Helper: Lower a single intrinsic argument ──────────────────────────
static llvm::Value* lowerIntrinsicArg(
    ExprAST* arg,
    IntrinsicKind kind,
    size_t argIndex,
    CodeGenContext& ctx
) {
    if (!arg) return nullptr;
    
    // ─── Lower the expression ─────────────────────────────────────────────
    llvm::Value* argVal = lowerExpression(arg, ctx);
    if (!argVal) return nullptr;
    
    // ─── Check if we should skip loading ──────────────────────────────────
    bool skipLoad = shouldSkipLoadForArg(kind, argIndex);
    
    if (skipLoad) {
        // ─── For raw address arguments, just return the pointer ──────────
        // But we may need to cast to the expected type
        llvm::Type* expectedType = getExpectedArgType(kind, argIndex, ctx);
        if (expectedType && argVal->getType() != expectedType) {
            // Cast to the expected type (e.g., ArenaDescriptor* → i8*)
            if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) {
                argVal = ctx.builder.CreatePointerCast(argVal, expectedType, "intrinsic_arg_cast");
            }
        }
        return argVal;
    }
    
    // ─── Normal path: load lvalues ──────────────────────────────────────
    if (arg->isLValue) {
        // Get the element type from the AST
        llvm::Type* elemType = getType(ctx, arg->resolvedType);
        if (!elemType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, arg->loc,
                                    "cannot determine type of argument for intrinsic");
            return nullptr;
        }
        argVal = loadIfNeeded(argVal, elemType, ctx);
        if (!argVal) return nullptr;
    }
    
    // ─── Cast to expected type if needed ──────────────────────────────────
    llvm::Type* expectedType = getExpectedArgType(kind, argIndex, ctx);
    if (expectedType && argVal->getType() != expectedType) {
        if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
            argVal = ctx.builder.CreateIntCast(argVal, expectedType, true, "intrinsic_int_cast");
        } else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) {
            argVal = ctx.builder.CreatePointerCast(argVal, expectedType, "intrinsic_ptr_cast");
        }
    }
    
    return argVal;
}

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Value* emitIntrinsicFromAST(
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return nullptr;

    SourceLocation loc = expr->loc;
    const IntrinsicInfo* info = IntrinsicRegistry::getInstance(ctx.pool).getInfo(expr->intrinsicName);
    if (!info) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                "unknown intrinsic '#", ctx.pool.lookup(expr->intrinsicName), "'");
        return nullptr;
    }

    // ─── Special-case: #scope_exit ────────────────────────────────────────
    // #scope_exit is handled entirely in Sema and stored on BlockStmtAST.
    // No runtime code is generated at the call site itself.
    if (info->kind == IntrinsicKind::ScopeExit) {
        return nullptr;
    }

    // ─── Special-case: #sizeof(T) / #alignof(T) ──────────────────────────
    // These intrinsics operate on types, not values. They have no arguments
    // at runtime - the type comes from expr->resolvedType.
    if (info->kind == IntrinsicKind::Sizeof || info->kind == IntrinsicKind::Alignof) {
        // Just delegate to the emitter with empty args
        return emitIntrinsic(expr->intrinsicName, {}, expr, ctx);
    }

    // ─── Special-case: #addrof(x) ─────────────────────────────────────────
    // addrof(x) returns the address of x - do NOT load
    if (info->kind == IntrinsicKind::Addrof) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#addrof' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        // argVal should already be a pointer (l-value)
        return argVal;
    }

    // ─── #toRef(ptr) ────────────────────────────────────────────────────────
    // Null check with fallback/panic support. No flag needed - the builder's
    // position tells us the success path.
    if (info->kind == IntrinsicKind::ToRef) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toRef' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;

        // ─── Null check ──────────────────────────────────────────────────
        llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::Value* checkedPtr = argVal;
        if (checkedPtr->getType() != i8Ptr) {
            checkedPtr = ctx.builder.CreatePointerCast(checkedPtr, i8Ptr, "toRef_ptr_cast");
        }

        llvm::Value* isNull = ctx.builder.CreateIsNull(checkedPtr, "toRef_null_check");
        llvm::Function* func = ctx.getCurrentFunction();

        // ─── Create continue block (success path) ──────────────────────
        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "toRef_continue", func);

        if (ctx.isInsideNullCoalesce()) {
            // ─── In ?? context: branch to fallback on null ──────────────
            llvm::BasicBlock* fallbackBlock = ctx.getNullCoalesceFallbackBlock();
            ctx.builder.CreateCondBr(isNull, fallbackBlock, continueBlock);
            // No flag needed - the builder is now in continueBlock
        } else {
            // ─── Normal context: panic on null ──────────────────────────
            llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "toRef_panic", func);
            ctx.builder.CreateCondBr(isNull, panicBlock, continueBlock);

            ctx.builder.SetInsertPoint(panicBlock);
            emitPanic(RuntimeErrorKind::NullPointerDereference, ctx, expr->args[0]);
            // emitPanic already emits CreateUnreachable()
        }

        // ─── Success path ────────────────────────────────────────────────
        ctx.builder.SetInsertPoint(continueBlock);
        return argVal;
    }

    // ─── General case: Lower all arguments with the appropriate rules ────
    // This handles all other intrinsics (math, memory, string, etc.)
    std::vector<llvm::Value*> args;
    args.reserve(expr->args.size());

    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* arg = expr->args[i];
        llvm::Value* argVal = lowerIntrinsicArg(arg, info->kind, i, ctx);
        if (!argVal) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, arg->loc,
                                    "failed to lower argument ", i, " for intrinsic '#",
                                    ctx.pool.lookup(expr->intrinsicName), "'");
            return nullptr;
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
                case IntrinsicKind::Trunc:
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