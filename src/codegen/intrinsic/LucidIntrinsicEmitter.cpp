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

#include <unordered_set>

namespace codegen {

// ─── Helper: Check if intrinsic is LLVM or Lucid ─────────────────────────

static bool isLLVMIntrinsic(const std::string& name) {
    static const std::unordered_set<std::string> LLVM_INTRINSICS = {
        "sqrt", "abs", "fma", "ceil", "floor", "round", "pow", "min", "max",
        "memcpy", "memmove", "memset",
        "clz", "ctz", "popcount", "bswap",
        "simd_add", "simd_sub", "simd_mul", "simd_div",
        "simd_fma", "simd_min", "simd_max",
        "simd_load", "simd_store",
        "simd_splat", "simd_extract", "simd_insert",
        "atomic_load", "atomic_store", "atomic_add", "atomic_sub",
        "atomic_and", "atomic_or", "atomic_xor", "atomic_cas",
        "prefetch", "prefetch_r", "prefetch_w", "fence", "pause"
    };
    return LLVM_INTRINSICS.find(name) != LLVM_INTRINSICS.end();
}

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Value* emitIntrinsicFromAST(
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return nullptr;

    std::string name = ctx.pool.lookup(expr->intrinsicName);
    SourceLocation loc = expr->loc;

    // ─── Special-case intrinsics that need raw addresses ────────────────
    if (name == "addrof") {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#addrof' requires an argument");
            return nullptr;
        }

        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        return argVal;
    }

    if (name == "toRef") {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toRef' requires an argument");
            return nullptr;
        }

        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;

        return emitNullCheck(argVal, ctx);
    }

    // ─── Normal path: load lvalues ────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) return nullptr;

        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (!elemType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, arg->loc,
                                       "cannot determine type of argument for '#", name, "'");
                return nullptr;
            }
            argVal = loadIfNeeded(argVal, elemType, ctx);
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    return emitIntrinsic(name, args, expr, ctx);
}

llvm::Value* emitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── Dispatch to appropriate emitter ──────────────────────────────────

    if (isLLVMIntrinsic(name)) {
        // Math intrinsics
        static const std::unordered_set<std::string> MATH_INTRINSICS = {
            "sqrt", "abs", "fma", "ceil", "floor", "round", "pow", "min", "max"
        };
        if (MATH_INTRINSICS.find(name) != MATH_INTRINSICS.end()) {
            std::vector<llvm::Value*> mutableArgs = args;
            return emitLLVMMathIntrinsic(name, mutableArgs, expr, ctx);
        }

        // Memory intrinsics
        static const std::unordered_set<std::string> MEMORY_INTRINSICS = {
            "memcpy", "memmove", "memset"
        };
        if (MEMORY_INTRINSICS.find(name) != MEMORY_INTRINSICS.end()) {
            return emitLLVMMemoryIntrinsic(name, args, expr, ctx);
        }

        // Bit intrinsics
        static const std::unordered_set<std::string> BIT_INTRINSICS = {
            "clz", "ctz", "popcount", "bswap"
        };
        if (BIT_INTRINSICS.find(name) != BIT_INTRINSICS.end()) {
            return emitLLVMBitIntrinsic(name, args, expr, ctx);
        }

        // Atomic intrinsics
        if (name.find("atomic_") == 0) {
            return emitLLVMAtomicIntrinsic(name, args, expr, ctx);
        }

        // SIMD intrinsics
        if (name.find("simd_") == 0) {
            return emitLLVMSIMDIntrinsic(name, args, expr, ctx);
        }

        // CPU hint intrinsics
        static const std::unordered_set<std::string> CPU_HINT_INTRINSICS = {
            "prefetch", "prefetch_r", "prefetch_w", "fence", "pause"
        };
        if (CPU_HINT_INTRINSICS.find(name) != CPU_HINT_INTRINSICS.end()) {
            return emitLLVMCPUHintIntrinsic(name, args, expr, ctx);
        }

        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                                "unknown LLVM intrinsic '#'", name, "'");
        return nullptr;
    }

    // ─── Lucid Compiler-Handled Intrinsics ──────────────────────────────

    // Type inspection intrinsics
    static const std::unordered_set<std::string> TYPE_INTRINSICS = {
        "sizeof", "alignof", "typeof", "nameof", "tostr", "ptrstr", "bitcast"
    };
    if (TYPE_INTRINSICS.find(name) != TYPE_INTRINSICS.end()) {
        return emitLucidTypeIntrinsic(name, args, expr, ctx);
    }

    // Pointer intrinsics (addrof is special-cased above)
    static const std::unordered_set<std::string> POINTER_INTRINSICS = {
        "toPtr", "ptrOffset", "ptrDiff"
    };
    if (POINTER_INTRINSICS.find(name) != POINTER_INTRINSICS.end()) {
        return emitLucidPointerIntrinsic(name, args, expr, ctx);
    }

    // Memory management intrinsics
    static const std::unordered_set<std::string> MEMORY_MGMT_INTRINSICS = {
        "alloc", "free", "arena_create", "arena_alloc",
        "arena_reset", "arena_free"
    };
    if (MEMORY_MGMT_INTRINSICS.find(name) != MEMORY_MGMT_INTRINSICS.end()) {
        return emitLucidMemoryMgmtIntrinsic(name, args, expr, ctx);
    }

    // String intrinsics
    static const std::unordered_set<std::string> STRING_INTRINSICS = {
        "str_len", "str_ptr", "str_from_ptr", "str_concat",
        "str_slice", "str_eq", "str_byte_at"
    };
    if (STRING_INTRINSICS.find(name) != STRING_INTRINSICS.end()) {
        return emitLucidStringIntrinsic(name, args, expr, ctx);
    }

    // Control flow intrinsics
    static const std::unordered_set<std::string> CONTROL_INTRINSICS = {
        "likely", "unlikely", "scope_exit"
    };
    if (CONTROL_INTRINSICS.find(name) != CONTROL_INTRINSICS.end()) {
        return emitLucidControlIntrinsic(name, args, expr, ctx);
    }

    // Unknown intrinsic
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown intrinsic '#'", name, "'");
    return nullptr;
}

} // namespace codegen