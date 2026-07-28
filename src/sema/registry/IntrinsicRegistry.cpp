/**
 * @file IntrinsicRegistry.cpp
 * @brief Implementation of the intrinsic registry.
 */

#include "IntrinsicRegistry.hpp"

#include <cassert>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry - Singleton, bound to one StringPool
// ─────────────────────────────────────────────────────────────────────────────

IntrinsicRegistry& IntrinsicRegistry::getInstance(StringPool& pool) {
    static IntrinsicRegistry instance(pool);

    // See IntrinsicRegistry.hpp's "Bound to one StringPool" note: every
    // call after the first one that constructed `instance` must pass the
    // exact same pool, or its InternedString keys silently mean nothing.
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

    // min/max apply to "same type" per the grammar (int or float), so there
    // is no single LLVM intrinsic ID that covers every case — handled by
    // the compiler, which picks minnum/maxnum vs smin/smax/umin/umax by
    // operand type during lowering.
    registerCompilerIntrinsic("min", 2);
    registerCompilerIntrinsic("max", 2);

    // ─── Memory Intrinsics (Raw Memory Operations) ──────────────────────────
    // These map directly to LLVM memory intrinsics.

    registerLLVMIntrinsic("memcpy", llvm::Intrinsic::memcpy, 3);
    registerLLVMIntrinsic("memmove", llvm::Intrinsic::memmove, 3);
    registerLLVMIntrinsic("memset", llvm::Intrinsic::memset, 3);

    // ─── Bit Manipulation Intrinsics ────────────────────────────────────────
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

    // #fence(ordering) - Compiler-handled; validates ordering strings:
    //   relaxed, acquire, release, acq_rel, seq_cst
    // Emits LLVM 'fence' instruction directly during code generation.
    // See validateFenceOrdering() for validation logic.
    registerCompilerIntrinsic("fence", 1);

    // #pause() - Compiler-handled; x86-specific PAUSE instruction.
    // On other architectures, may be a no-op or a yield instruction.
    registerCompilerIntrinsic("pause", 0);

    // ─── Atomics ────────────────────────────────────────────────────────────
    // These are LLVM instructions, NOT intrinsics!
    // They are handled by the compiler during lowering, not mapped to an
    // intrinsic function call. The registry marks them as compiler-handled
    // so the frontend validates argument counts but code generation emits
    // the LLVM instruction directly.

    registerCompilerIntrinsic("atomic_load", 2);   // (ptr, ordering)
    registerCompilerIntrinsic("atomic_store", 2);  // (ptr, val, ordering)
    registerCompilerIntrinsic("atomic_add", 2);    // (ptr, val, ordering)
    registerCompilerIntrinsic("atomic_sub", 2);
    registerCompilerIntrinsic("atomic_and", 2);
    registerCompilerIntrinsic("atomic_or", 2);
    registerCompilerIntrinsic("atomic_xor", 2);
    registerCompilerIntrinsic("atomic_cas", 3);    // (ptr, expected, desired, ordering)

    // ─── Compiler-Handled Intrinsics ──────────────────────────────────────
    //
    // These are handled directly by the compiler, not LLVM intrinsics.
    // They are resolved at compile time or lowered to specific LLVM IR
    // patterns during code generation.

    // Type & Value Inspection - Resolved at compile time
    registerCompilerIntrinsic("sizeof", 1);    // (T) → compile-time constant
    registerCompilerIntrinsic("alignof", 1);   // (T) → compile-time constant
    registerCompilerIntrinsic("typeof", 1);    // (x) → type name string
    registerCompilerIntrinsic("nameof", 1);    // (x) → variable/field name string
    registerCompilerIntrinsic("tostr", 1);     // (x) → string representation
    registerCompilerIntrinsic("ptrstr", 1);    // (x) → address as hex string
    registerCompilerIntrinsic("addrof", 1);    // (x) → *T raw pointer

    // Pointer Operations - Cross the safe/pointer boundary
    registerCompilerIntrinsic("ptrOffset", 2); // (ptr, n) → ptr arithmetic
    registerCompilerIntrinsic("ptrDiff", 2);   // (p1, p2) → distance in elements
    registerCompilerIntrinsic("toRef", 1);     // (*T) → &T (assert non-null)
    registerCompilerIntrinsic("toPtr", 1);     // (&T) → *T (convert back)

    // Bit Manipulation
    registerCompilerIntrinsic("bitcast", 2, 2); // (T, x) - T is a type

    // Branch Prediction Hints
    registerCompilerIntrinsic("likely", 1);
    registerCompilerIntrinsic("unlikely", 1);

    // ─── String Operations ──────────────────────────────────────────────────
    // Low-level string intrinsics the standard library builds on.
    // Strings are immutable UTF-8 sequences managed by the compiler.

    registerCompilerIntrinsic("str_len", 1);        // (s) → byte length
    registerCompilerIntrinsic("str_ptr", 1);        // (s) → *uint8 raw bytes (read-only)
    registerCompilerIntrinsic("str_from_ptr", 2);   // (ptr, len) → string (copies, validates UTF-8)
    registerCompilerIntrinsic("str_concat", 2);     // (a, b) → concatenated string
    registerCompilerIntrinsic("str_slice", 3);      // (s, from, to) → byte-range slice
    registerCompilerIntrinsic("str_eq", 2);         // (a, b) → byte-exact equality
    registerCompilerIntrinsic("str_byte_at", 2);    // (s, i) → byte at position

    // ─── Memory Management ──────────────────────────────────────────────────
    // Foreign-interop allocation only — never used in ordinary Lucid code.
    // The compiler tracks #alloc/#free to catch double-free and null-free.

    registerCompilerIntrinsic("alloc", 2);          // (T, count) → *T
    registerCompilerIntrinsic("free", 1);           // (ptr) → void
    registerCompilerIntrinsic("arena_create", 1);   // (size) → ArenaDescriptor
    registerCompilerIntrinsic("arena_alloc", 3);    // (arena, T, n) → *T
    registerCompilerIntrinsic("arena_reset", 1);    // (arena) → void (releases contents)
    registerCompilerIntrinsic("arena_free", 1);     // (arena) → void (destroys arena)

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
    registerCompilerIntrinsic("simd_load", 1);      // (ptr) → vec<T,N>
    registerCompilerIntrinsic("simd_store", 2);     // (ptr, vec)
    registerCompilerIntrinsic("simd_splat", 3);     // (T, N, scalar) - T is a type!
    registerCompilerIntrinsic("simd_extract", 2);   // (vec, i) → T
    registerCompilerIntrinsic("simd_insert", 3);    // (vec, i, x) → vec

    m_initialized = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry - Registration Helpers
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
// IntrinsicRegistry - Queries
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
        return std::nullopt;  // Variable arguments
    }

    if (info->minArgs == info->maxArgs) {
        return info->minArgs;
    }

    // Range of possible counts
    return std::nullopt;
}

bool IntrinsicRegistry::validateFenceOrdering(InternedString ordering, StringPool& pool) const {
    std::string ord = pool.lookup(ordering);
    return ord == "relaxed" || ord == "acquire" ||
           ord == "release" || ord == "acq_rel" || ord == "seq_cst";
}

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