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
    // intrinsic functions. NOTE: exp/log/log10/sin/cos/tan/atan2/fmod are
    // NOT part of the Lucid grammar and must not be registered here.

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

    registerLLVMIntrinsic("memcpy", llvm::Intrinsic::memcpy, 3);
    registerLLVMIntrinsic("memmove", llvm::Intrinsic::memmove, 3);
    registerLLVMIntrinsic("memset", llvm::Intrinsic::memset, 3);

    // ─── Bit Manipulation Intrinsics ────────────────────────────────────────

    registerLLVMIntrinsic("clz", llvm::Intrinsic::ctlz, 1);
    registerLLVMIntrinsic("ctz", llvm::Intrinsic::cttz, 1);
    registerLLVMIntrinsic("popcount", llvm::Intrinsic::ctpop, 1);
    registerLLVMIntrinsic("bswap", llvm::Intrinsic::bswap, 1);

    // ─── CPU Hints ──────────────────────────────────────────────────────────

    registerLLVMIntrinsic("prefetch", llvm::Intrinsic::prefetch, 1);
    registerLLVMIntrinsic("prefetch_w", llvm::Intrinsic::prefetch, 1);
    // pause is x86-specific, use a different approach
    // fence is an instruction, not an intrinsic

    // ─── Atomics ────────────────────────────────────────────────────────────
    // These are LLVM instructions, NOT intrinsics!
    // They are handled differently in the lowering phase

    // ─── Compiler-Handled Intrinsics ──────────────────────────────────────

    // These are handled directly by the compiler, not LLVM intrinsics
    registerCompilerIntrinsic("sizeof", 1);
    registerCompilerIntrinsic("alignof", 1);
    registerCompilerIntrinsic("typeof", 1);
    registerCompilerIntrinsic("nameof", 1);
    registerCompilerIntrinsic("tostr", 1);
    registerCompilerIntrinsic("ptrstr", 1);
    registerCompilerIntrinsic("addrof", 1);
    registerCompilerIntrinsic("ptrOffset", 2);
    registerCompilerIntrinsic("ptrDiff", 2);
    registerCompilerIntrinsic("toRef", 1);
    registerCompilerIntrinsic("toPtr", 1);
    registerCompilerIntrinsic("bitcast", 2, 2);  // (T, value) - T is a type
    registerCompilerIntrinsic("likely", 1);
    registerCompilerIntrinsic("unlikely", 1);
    registerCompilerIntrinsic("pause", 0);
    registerCompilerIntrinsic("fence", 1);  // (ordering)

    // ─── String Operations ─────────────────────────────────────────────────
    // Low-level string intrinsics the standard library builds on
    registerCompilerIntrinsic("str_len", 1);
    registerCompilerIntrinsic("str_ptr", 1);
    registerCompilerIntrinsic("str_from_ptr", 2);
    registerCompilerIntrinsic("str_concat", 2);
    registerCompilerIntrinsic("str_slice", 3);
    registerCompilerIntrinsic("str_eq", 2);
    registerCompilerIntrinsic("str_byte_at", 2);

    // ─── Memory Management ──────────────────────────────────────────────────
    // Foreign-interop allocation only — never used in ordinary Lucid code
    registerCompilerIntrinsic("alloc", 2);          // (T, count)
    registerCompilerIntrinsic("free", 1);           // (ptr)
    registerCompilerIntrinsic("arena_create", 1);   // (size)
    registerCompilerIntrinsic("arena_alloc", 3);    // (arena, T, n)
    registerCompilerIntrinsic("arena_reset", 1);    // (arena)
    registerCompilerIntrinsic("arena_free", 1);     // (arena)

    // ─── Atomics (LLVM instructions, not intrinsics) ──────────────────────
    registerCompilerIntrinsic("atomic_load", 2);
    registerCompilerIntrinsic("atomic_store", 2);
    registerCompilerIntrinsic("atomic_add", 2);
    registerCompilerIntrinsic("atomic_sub", 2);
    registerCompilerIntrinsic("atomic_and", 2);
    registerCompilerIntrinsic("atomic_or", 2);
    registerCompilerIntrinsic("atomic_xor", 2);
    registerCompilerIntrinsic("atomic_cas", 3);

    // ─── SIMD Intrinsics ────────────────────────────────────────────────────
    // These are compiler-handled and lower to LLVM instructions
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