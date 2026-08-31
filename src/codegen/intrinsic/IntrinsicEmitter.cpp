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

    // ─── Special-case: #toRef(ptr) ────────────────────────────────────────
    // toRef(ptr) - do NOT load, but add null check
    if (info->kind == IntrinsicKind::ToRef) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toRef' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        return emitNullCheck(argVal, ctx);
    }

    // ─── Special-case: #ptrstr(x) ─────────────────────────────────────────
    // ptrstr(x) formats x's memory address - needs the raw address
    if (info->kind == IntrinsicKind::Ptrstr) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrstr' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
    }

    // ─── Special-case: #bitcast(T, x) ─────────────────────────────────────
    // #bitcast takes a type argument T (compile-time) and a value x (runtime)
    // The type T comes from expr->resolvedType, not from the arguments.
    if (info->kind == IntrinsicKind::Bitcast) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#bitcast' requires an argument");
            return nullptr;
        }
        // Lower the value argument (the type T is from resolvedType)
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        if (expr->args[0]->isLValue) {
            llvm::Type* elemType = getType(ctx, expr->args[0]->resolvedType);
            if (elemType) {
                argVal = loadIfNeeded(argVal, elemType, ctx);
            }
            if (!argVal) return nullptr;
        }
        return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
    }

    // ─── Special-case: #alloc(T, count) ──────────────────────────────────
    // #alloc takes a type argument T (compile-time) and a count (runtime)
    // The type T comes from expr->resolvedType (*T), count is the argument.
    if (info->kind == IntrinsicKind::Alloc) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#alloc' requires an argument (count)");
            return nullptr;
        }
        // Lower the count argument - it should be loaded
        llvm::Value* count = lowerIntrinsicArg(expr->args[0], info->kind, 0, ctx);
        if (!count) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {count}, expr, ctx);
    }

    // ─── Special-case: #tostr(x) ──────────────────────────────────────────
    // #tostr can take ANY type - load the value normally
    if (info->kind == IntrinsicKind::Tostr) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#tostr' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerIntrinsicArg(expr->args[0], info->kind, 0, ctx);
        if (!argVal) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
    }

    // ─── Special-case: #type_of(x) / #name_of(x) ─────────────────────────
    // These intrinsics operate on expressions - they need the AST info
    if (info->kind == IntrinsicKind::Typeof || info->kind == IntrinsicKind::Nameof) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#'", ctx.pool.lookup(expr->intrinsicName), 
                                   "' requires an argument");
            return nullptr;
        }
        // For #typeof and #nameof, we actually DON'T need to lower the argument
        // - we just need its type/name from the AST. But we still need to
        // pass something to the emitter (it will ignore the value and use
        // the AST).
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
    }

    // ─── Special-case: #simd_splat(x) ─────────────────────────────────────
    // #simd_splat takes a scalar value and returns a vector - load normally
    if (info->kind == IntrinsicKind::SimdSplat) {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_splat' requires an argument");
            return nullptr;
        }
        llvm::Value* argVal = lowerIntrinsicArg(expr->args[0], info->kind, 0, ctx);
        if (!argVal) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {argVal}, expr, ctx);
    }

    // ─── Special-case: #simd_extract(vec, index) ─────────────────────────
    // #simd_extract takes a vector and an index - index must be constant
    if (info->kind == IntrinsicKind::SimdExtract) {
        if (expr->args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_extract' requires 2 arguments");
            return nullptr;
        }
        llvm::Value* vec = lowerIntrinsicArg(expr->args[0], info->kind, 0, ctx);
        if (!vec) return nullptr;
        llvm::Value* idx = lowerIntrinsicArg(expr->args[1], info->kind, 1, ctx);
        if (!idx) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {vec, idx}, expr, ctx);
    }

    // ─── Special-case: #simd_insert(vec, index, value) ───────────────────
    if (info->kind == IntrinsicKind::SimdInsert) {
        if (expr->args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_insert' requires 3 arguments");
            return nullptr;
        }
        llvm::Value* vec = lowerIntrinsicArg(expr->args[0], info->kind, 0, ctx);
        if (!vec) return nullptr;
        llvm::Value* idx = lowerIntrinsicArg(expr->args[1], info->kind, 1, ctx);
        if (!idx) return nullptr;
        llvm::Value* val = lowerIntrinsicArg(expr->args[2], info->kind, 2, ctx);
        if (!val) return nullptr;
        return emitIntrinsic(expr->intrinsicName, {vec, idx, val}, expr, ctx);
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