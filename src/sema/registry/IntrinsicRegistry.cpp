/**
 * @file IntrinsicRegistry.cpp
 * @brief Implementation of the intrinsic registry.
 */

#include "IntrinsicRegistry.hpp"
#include "../support/LiteralHelpers.hpp"
#include "debug/DebugUtils.hpp"

#include <cassert>
#include <algorithm>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry - Singleton, bound to one StringPool
// ─────────────────────────────────────────────────────────────────────────────

IntrinsicRegistry& IntrinsicRegistry::getInstance(StringPool& pool) {
    static IntrinsicRegistry instance(pool);

    assert(&instance.m_pool == &pool &&
           "IntrinsicRegistry::getInstance() called with a different "
           "StringPool than the one it was first bound to");

    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry - Construction
// ─────────────────────────────────────────────────────────────────────────────

IntrinsicRegistry::IntrinsicRegistry(StringPool& pool) : m_pool(pool) {
    registerIntrinsics();
}

void IntrinsicRegistry::registerIntrinsics() {
    if (m_initialized) {
        return;
    }

    // ─── Floating-Point Math Intrinsics ─────────────────────────────────────
    // Grammar.md "Floating-Point Math" table — these map directly to LLVM
    // intrinsic functions.
    //
    // NOTE: exp/log/log10/sin/cos/tan/atan2/fmod are NOT part of the Lucid
    // grammar and must not be registered here. They are in the standard
    // library as plain Lucid functions, not intrinsics.

    registerLLVMIntrinsic("sqrt", llvm::Intrinsic::sqrt, 1);
    registerLLVMIntrinsic("abs", llvm::Intrinsic::fabs, 1);
    registerLLVMIntrinsic("fma", llvm::Intrinsic::fma, 3);
    registerLLVMIntrinsic("ceil", llvm::Intrinsic::ceil, 1);
    registerLLVMIntrinsic("floor", llvm::Intrinsic::floor, 1);
    registerLLVMIntrinsic("round", llvm::Intrinsic::round, 1);
    registerLLVMIntrinsic("pow", llvm::Intrinsic::pow, 2);

    // min/max are compiler-handled (pick minnum/maxnum vs smin/smax by type)
    registerCompilerIntrinsic("min", 2);
    registerCompilerIntrinsic("max", 2);

    // ─── Memory Intrinsics ──────────────────────────────────────────────────
    // These map directly to LLVM memory intrinsics.

    registerLLVMIntrinsic("memcpy", llvm::Intrinsic::memcpy, 3);
    registerLLVMIntrinsic("memmove", llvm::Intrinsic::memmove, 3);
    registerLLVMIntrinsic("memset", llvm::Intrinsic::memset, 3);

    // ─── Bit Manipulation Intrinsics ──────────────────────────────────────
    // Maps to single CPU instructions (BSF, BSR, POPCNT, BSWAP on x86-64).

    registerLLVMIntrinsic("clz", llvm::Intrinsic::ctlz, 1);
    registerLLVMIntrinsic("ctz", llvm::Intrinsic::cttz, 1);
    registerLLVMIntrinsic("popcount", llvm::Intrinsic::ctpop, 1);
    registerLLVMIntrinsic("bswap", llvm::Intrinsic::bswap, 1);

    // ─── CPU Hints ──────────────────────────────────────────────────────────
    //
    // PREFETCH FAMILY:
    //   All three map to the same LLVM intrinsic `llvm.prefetch`, differentiated
    //   by the `rw` (read/write) argument:
    //     - #prefetch(ptr)   → rw=0 (read), default, general use
    //     - #prefetch_r(ptr) → rw=0 (read), explicit read prefetch
    //     - #prefetch_w(ptr) → rw=1 (write), explicit write prefetch
    //
    //   The `_r` and `_w` suffixes make the intent explicit for performance-
    //   critical code, while `#prefetch` remains the simple default.
    //
    //   LLVM's llvm.prefetch signature:
    //     declare void @llvm.prefetch(ptr, i32, i32, i32)
    //     - ptr: memory address to prefetch
    //     - i32: read/write (0=read, 1=write)
    //     - i32: locality (0=none, 1=low, 2=moderate, 3=high)
    //     - i32: cache type (0=data, 1=instruction)
    //
    //   Code generation sets: rw=0 for #prefetch/#prefetch_r, rw=1 for #prefetch_w
    //   locality=3 (high), cache=0 (data) by default.

    registerLLVMIntrinsic("prefetch", llvm::Intrinsic::prefetch, 1);     // read (default)
    registerLLVMIntrinsic("prefetch_r", llvm::Intrinsic::prefetch, 1);   // explicit read
    registerLLVMIntrinsic("prefetch_w", llvm::Intrinsic::prefetch, 1);   // explicit write

    // #fence(ordering) - compiler-handled; validates ordering strings:
    //   relaxed, acquire, release, acq_rel, seq_cst
    // Emits LLVM 'fence' instruction directly during code generation.
    registerCompilerIntrinsic("fence", 1);

    // #pause() - compiler-handled; x86-specific PAUSE instruction.
    // On other architectures, may be a no-op or a yield instruction.
    registerCompilerIntrinsic("pause", 0);

    // ─── Atomics ────────────────────────────────────────────────────────────
    // These are LLVM instructions, NOT intrinsics!
    // They are handled by the compiler during lowering, not mapped to an
    // intrinsic function call. The registry marks them as compiler-handled
    // so the frontend validates argument counts but code generation emits
    // the LLVM instruction directly.

    registerCompilerIntrinsic("atomic_load", 2);
    registerCompilerIntrinsic("atomic_store", 2);
    registerCompilerIntrinsic("atomic_add", 2);
    registerCompilerIntrinsic("atomic_sub", 2);
    registerCompilerIntrinsic("atomic_and", 2);
    registerCompilerIntrinsic("atomic_or", 2);
    registerCompilerIntrinsic("atomic_xor", 2);
    registerCompilerIntrinsic("atomic_cas", 3);

    // ─── Type & Value Inspection ──────────────────────────────────────────
    // These are handled directly by the compiler, not LLVM intrinsics.
    // They are resolved at compile time or lowered to specific LLVM IR
    // patterns during code generation.

    registerCompilerIntrinsic("sizeof", 1);
    registerCompilerIntrinsic("alignof", 1);
    registerCompilerIntrinsic("typeof", 1);
    registerCompilerIntrinsic("nameof", 1);
    registerCompilerIntrinsic("tostr", 1);
    registerCompilerIntrinsic("ptrstr", 1);
    registerCompilerIntrinsic("addrof", 1);

    // ─── Pointer Operations ──────────────────────────────────────────────
    // Cross the safe/pointer boundary.

    registerCompilerIntrinsic("ptrOffset", 2);
    registerCompilerIntrinsic("ptrDiff", 2);
    registerCompilerIntrinsic("toRef", 1);
    registerCompilerIntrinsic("toPtr", 1);

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    registerCompilerIntrinsic("bitcast", 2, 2);

    // ─── Branch Prediction ──────────────────────────────────────────────────
    registerCompilerIntrinsic("likely", 1);
    registerCompilerIntrinsic("unlikely", 1);

    // ─── String Operations ──────────────────────────────────────────────────
    // Low-level string intrinsics the standard library builds on.
    // Strings are immutable UTF-8 sequences managed by the compiler.

    registerCompilerIntrinsic("str_len", 1);
    registerCompilerIntrinsic("str_ptr", 1);
    registerCompilerIntrinsic("str_from_ptr", 2);
    registerCompilerIntrinsic("str_concat", 2);
    registerCompilerIntrinsic("str_slice", 3);
    registerCompilerIntrinsic("str_eq", 2);
    registerCompilerIntrinsic("str_byte_at", 2);

    // ─── Memory Management ──────────────────────────────────────────────────
    // Foreign-interop allocation only — never used in ordinary Lucid code.
    // The compiler tracks #alloc/#free to catch double-free and null-free.

    registerCompilerIntrinsic("alloc", 2);
    registerCompilerIntrinsic("free", 1);
    registerCompilerIntrinsic("arena_create", 1);
    registerCompilerIntrinsic("arena_alloc", 3);
    registerCompilerIntrinsic("arena_reset", 1);
    registerCompilerIntrinsic("arena_free", 1);

    // ─── SIMD Intrinsics ────────────────────────────────────────────────────
    // These are compiler-handled and lower to LLVM instructions.
    //
    // #simd_splat(T, N, scalar) - Special handling because T is a type:
    //   1. T is a type (e.g., float32), not a value — requires type resolution
    //   2. N must be a compile-time integer constant
    //   3. There is no direct LLVM intrinsic for splat — it's lowered to:
    //      `insertelement` + `shufflevector` or `vector_splat` (LLVM 18+)
    //   The compiler validates that T is a valid SIMD element type and that N
    //   is a compile-time constant within the target's vector register limits.

    registerCompilerIntrinsic("simd_add", 2);
    registerCompilerIntrinsic("simd_sub", 2);
    registerCompilerIntrinsic("simd_mul", 2);
    registerCompilerIntrinsic("simd_div", 2);
    registerCompilerIntrinsic("simd_fma", 3);
    registerCompilerIntrinsic("simd_min", 2);
    registerCompilerIntrinsic("simd_max", 2);
    registerCompilerIntrinsic("simd_load", 1);
    registerCompilerIntrinsic("simd_store", 2);
    registerCompilerIntrinsic("simd_splat", 3);
    registerCompilerIntrinsic("simd_extract", 2);
    registerCompilerIntrinsic("simd_insert", 3);

    m_initialized = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration Helpers
// ─────────────────────────────────────────────────────────────────────────────

void IntrinsicRegistry::registerLLVMIntrinsic(std::string_view lucidName,
                                               llvm::Intrinsic::ID llvmID,
                                               size_t minArgs,
                                               size_t maxArgs,
                                               bool isVarArg) {
    if (maxArgs == 0) {
        maxArgs = minArgs;
    }

    InternedString name = m_pool.intern(lucidName);
    IntrinsicInfo info(llvmID, name, minArgs, maxArgs, isVarArg);
    m_intrinsicMap[name] = info;
    m_llvmIntrinsics.insert(name);
}

void IntrinsicRegistry::registerCompilerIntrinsic(std::string_view name,
                                                   size_t minArgs,
                                                   size_t maxArgs,
                                                   bool isVarArg) {
    if (maxArgs == 0) {
        maxArgs = minArgs;
    }

    InternedString id = m_pool.intern(name);
    IntrinsicInfo info(llvm::Intrinsic::not_intrinsic, id, minArgs, maxArgs, isVarArg);
    m_intrinsicMap[id] = info;
    m_compilerIntrinsics.insert(id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lookup Queries
// ─────────────────────────────────────────────────────────────────────────────

std::optional<llvm::Intrinsic::ID> IntrinsicRegistry::getLLVMIntrinsicID(InternedString name) const {
    auto it = m_intrinsicMap.find(name);
    if (it != m_intrinsicMap.end() && it->second.isValid()) {
        return it->second.id;
    }
    return std::nullopt;
}

const IntrinsicInfo* IntrinsicRegistry::getIntrinsicInfo(InternedString name) const {
    auto it = m_intrinsicMap.find(name);
    if (it != m_intrinsicMap.end()) {
        return &it->second;
    }
    return nullptr;
}

bool IntrinsicRegistry::isCompilerIntrinsic(InternedString name) const {
    return m_compilerIntrinsics.find(name) != m_compilerIntrinsics.end();
}

bool IntrinsicRegistry::isLLVMIntrinsic(InternedString name) const {
    return m_llvmIntrinsics.find(name) != m_llvmIntrinsics.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument Validation
// ─────────────────────────────────────────────────────────────────────────────

bool IntrinsicRegistry::validateArgCount(InternedString name, size_t argCount) const {
    auto* info = getIntrinsicInfo(name);
    if (!info) {
        return false;
    }

    if (info->isVarArg) {
        return argCount >= info->minArgs;
    }

    return argCount >= info->minArgs && argCount <= info->maxArgs;
}

std::optional<size_t> IntrinsicRegistry::getExpectedArgCount(InternedString name) const {
    auto* info = getIntrinsicInfo(name);
    if (!info) {
        return std::nullopt;
    }

    if (info->isVarArg) {
        return std::nullopt;
    }

    if (info->minArgs == info->maxArgs) {
        return info->minArgs;
    }

    return std::nullopt;
}

bool IntrinsicRegistry::validateFenceOrdering(InternedString ordering, StringPool& pool) const {
    std::string ord = pool.lookup(ordering);
    return ord == "relaxed" || ord == "acquire" ||
           ord == "release" || ord == "acq_rel" || ord == "seq_cst";
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument Type Validators (Private Helpers)
// ─────────────────────────────────────────────────────────────────────────────

bool IntrinsicRegistry::validatePtrArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !arg->resolvedType->isa<PtrTypeAST>()) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects pointer type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

bool IntrinsicRegistry::validateNumericArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !isNumericType(arg->resolvedType)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects numeric type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

bool IntrinsicRegistry::validateIntArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !isIntegerType(arg->resolvedType)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects integer type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

bool IntrinsicRegistry::validateStringArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !isStringType(arg->resolvedType)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects string type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

bool IntrinsicRegistry::validateBoolArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !isBoolType(arg->resolvedType)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects boolean type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

bool IntrinsicRegistry::validateRefArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const {
    if (!arg->resolvedType || !arg->resolvedType->isa<RefTypeAST>()) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects reference type, got ",
                  debug::typeToString(arg->resolvedType, ctx.pool()));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Intrinsic Call Validation (Used by SemaExpr.cpp)
// ─────────────────────────────────────────────────────────────────────────────

bool IntrinsicRegistry::validateIntrinsicCall(const IntrinsicCallExprAST* expr,
                                               SemaContext& ctx) const {
    if (!expr) return false;

    const std::string name = ctx.pool().lookup(expr->intrinsicName);

    // ─── Floating-Point Math ──────────────────────────────────────────────
    if (name == "sqrt" || name == "abs" || name == "fma" ||
        name == "ceil" || name == "floor" || name == "round" ||
        name == "pow" || name == "min" || name == "max") {
        for (size_t i = 0; i < expr->args.size(); ++i) {
            if (!validateNumericArg(expr->args[i], "arg" + std::to_string(i + 1), ctx)) {
                return false;
            }
        }
        return true;
    }

    // ─── Memory Operations ──────────────────────────────────────────────────
    if (name == "memcpy" || name == "memmove") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
        if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "src", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
        return true;
    }

    if (name == "memset") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "val", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
        return true;
    }

    // ─── CPU Hints ──────────────────────────────────────────────────────────
    if (name == "prefetch" || name == "prefetch_r" || name == "prefetch_w") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "fence") {
        if (!expr->args.empty()) {
            if (!validateFenceOrdering(expr->args[0]->as<LiteralExprAST>()->value, ctx.pool())) {
                ctx.error(expr->args[0], DiagCode::E3003,
                          "invalid fence ordering — must be: relaxed, acquire, "
                          "release, acq_rel, or seq_cst");
                return false;
            }
        }
        return true;
    }

    if (name == "likely" || name == "unlikely") {
        if (!expr->args.empty() && !validateBoolArg(expr->args[0], "condition", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "pause") {
        return true;
    }

    // ─── String Operations ──────────────────────────────────────────────────
    if (name == "str_len" || name == "str_ptr") {
        if (!expr->args.empty() && !validateStringArg(expr->args[0], "string", ctx)) return false;
        return true;
    }

    if (name == "str_from_ptr") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "len", ctx)) return false;
        return true;
    }

    if (name == "str_concat" || name == "str_eq") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "a", ctx)) return false;
        if (expr->args.size() >= 2 && !validateStringArg(expr->args[1], "b", ctx)) return false;
        return true;
    }

    if (name == "str_slice") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "from", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "to", ctx)) return false;
        return true;
    }

    if (name == "str_byte_at") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "index", ctx)) return false;
        return true;
    }

    // ─── Pointer Operations ──────────────────────────────────────────────────
    if (name == "addrof") {
        return true;
    }

    if (name == "toRef") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "toPtr") {
        if (!expr->args.empty() && !validateRefArg(expr->args[0], "ref", ctx)) return false;
        return true;
    }

    if (name == "ptrOffset") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "offset", ctx)) return false;
        return true;
    }

    if (name == "ptrDiff") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "p1", ctx)) return false;
        if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "p2", ctx)) return false;
        return true;
    }

    // ─── Bit Manipulation ────────────────────────────────────────────────────
    if (name == "clz" || name == "ctz" || name == "popcount" || name == "bswap") {
        if (!expr->args.empty() && !validateIntArg(expr->args[0], "value", ctx)) return false;
        return true;
    }

    // ─── Atomics ─────────────────────────────────────────────────────────────
    if (name.find("atomic_") == 0) {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        
        if (expr->args.size() >= 2) {
            const ExprAST* lastArg = expr->args[expr->args.size() - 1];
            if (!validateFenceOrdering(lastArg->as<LiteralExprAST>()->value, ctx.pool())) {
                ctx.error(lastArg, DiagCode::E3003,
                          "invalid ordering — must be: relaxed, acquire, "
                          "release, acq_rel, or seq_cst");
                return false;
            }
        }
        return true;
    }

    // ─── SIMD ────────────────────────────────────────────────────────────────
    if (name == "simd_splat") {
        if (expr->args.size() >= 2) {
            // Use LiteralHelpers for integer literal check
            if (!literal::isIntLiteral(expr->args[1])) {
                ctx.error(expr->args[1], DiagCode::E3003,
                          "argument 'N' must be a compile-time integer constant");
                return false;
            }
        }
        if (expr->args.size() >= 3 && !validateNumericArg(expr->args[2], "scalar", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "simd_load") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "simd_store") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    // ─── Memory Management ──────────────────────────────────────────────────
    if (name == "alloc") {
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "count", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "free") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "arena_create") {
        if (!expr->args.empty() && !validateIntArg(expr->args[0], "size", ctx)) return false;
        return true;
    }

    if (name == "arena_alloc") {
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "count", ctx)) {
            return false;
        }
        return true;
    }

    // ─── Compiler-Handled (no validation needed) ──────────────────────────
    // sizeof, alignof, typeof, nameof, tostr, ptrstr, bitcast
    // These are handled directly by the compiler.

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Return Type Determination
// ─────────────────────────────────────────────────────────────────────────────

const TypeAST* IntrinsicRegistry::getIntrinsicReturnType(const IntrinsicCallExprAST* expr,
                                                          const TypeAST* targetType,
                                                          SemaContext& ctx) const {
    const std::string name = ctx.pool().lookup(expr->intrinsicName);

    // ─── Type/Value Inspection ─────────────────────────────────────────────
    if (name == "sizeof" || name == "alignof") {
        return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Uint64);
    }

    if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr") {
        return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::String);
    }

    if (name == "addrof") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            return ctx.arena().makeType<PtrTypeAST>(expr->args[0]->resolvedType);
        }
        return targetType;
    }

    if (name == "toRef") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            const TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<PtrTypeAST>()) {
                return ctx.arena().makeType<RefTypeAST>(
                    argType->as<PtrTypeAST>()->inner
                );
            }
        }
        return targetType;
    }

    if (name == "toPtr") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            const TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<RefTypeAST>()) {
                return ctx.arena().makeType<PtrTypeAST>(
                    argType->as<RefTypeAST>()->inner
                );
            }
        }
        return targetType;
    }

    if (name == "bitcast") {
        return targetType;
    }

    // ─── String Operations ──────────────────────────────────────────────────
    if (name == "str_len" || name == "str_from_ptr" || name == "str_concat" ||
        name == "str_slice") {
        return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::String);
    }

    if (name == "str_ptr") {
        return ctx.arena().makeType<PtrTypeAST>(
            ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Uint8)
        );
    }

    if (name == "str_eq") {
        return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
    }

    if (name == "str_byte_at") {
        return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Uint8);
    }

    // ─── Memory Management ──────────────────────────────────────────────────
    if (name == "alloc" || name == "arena_alloc") {
        return ctx.arena().makeType<PtrTypeAST>(
            ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Uint8)
        );
    }

    if (name == "arena_create") {
        // TODO: Return ArenaDescriptor struct type
        return targetType;
    }

    // ─── SIMD ────────────────────────────────────────────────────────────────
    if (name.find("simd_") == 0) {
        return targetType;
    }

    return targetType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Value State Determination
// ─────────────────────────────────────────────────────────────────────────────

ValueState IntrinsicRegistry::getIntrinsicValueState(const IntrinsicCallExprAST* expr,
                                                      SemaContext& ctx) const {
    (void)expr;
    const std::string name = ctx.pool().lookup(expr->intrinsicName);

    if (name == "alloc" || name == "arena_alloc") {
        return ValueState::Unknown;
    }

    if (name == "toRef") {
        return ValueState::Definite;
    }

    if (name == "fence" || name == "pause") {
        return ValueState::Definite;
    }

    return ValueState::Definite;
}

// ─────────────────────────────────────────────────────────────────────────────
// Listing Methods
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> IntrinsicRegistry::getAllIntrinsicNames() const {
    std::vector<std::string> names;
    names.reserve(m_intrinsicMap.size());
    for (const auto& pair : m_intrinsicMap) {
        names.emplace_back(m_pool.lookup(pair.first));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> IntrinsicRegistry::getLLVMIntrinsicNames() const {
    std::vector<std::string> names;
    names.reserve(m_llvmIntrinsics.size());
    for (const auto& name : m_llvmIntrinsics) {
        names.emplace_back(m_pool.lookup(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> IntrinsicRegistry::getCompilerIntrinsicNames() const {
    std::vector<std::string> names;
    names.reserve(m_compilerIntrinsics.size());
    for (const auto& name : m_compilerIntrinsics) {
        names.emplace_back(m_pool.lookup(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace sema