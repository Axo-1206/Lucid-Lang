/// @file registry/IntrinsicRegistry.cpp
/// @brief Implementation of intrinsic registry.

#include "IntrinsicRegistry.hpp"
#include "core/memory/StringPool.hpp"
#include <algorithm>

// ─── Data Table ─────────────────────────────────────────────────────────────

// IntrinsicEntry is defined at namespace scope in IntrinsicRegistry.hpp
static const IntrinsicEntry INTRINSIC_TABLE[] = {
    // ─── Floating-Point Math ──────────────────────────────────────────────
    {"sqrt",      llvm::Intrinsic::sqrt,   1, 1, false, false},
    {"abs",       llvm::Intrinsic::fabs,   1, 1, false, false},
    {"fma",       llvm::Intrinsic::fma,    3, 3, false, false},
    {"ceil",      llvm::Intrinsic::ceil,   1, 1, false, false},
    {"floor",     llvm::Intrinsic::floor,  1, 1, false, false},
    {"round",     llvm::Intrinsic::round,  1, 1, false, false},
    {"pow",       llvm::Intrinsic::pow,    2, 2, false, false},
    {"min",       llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"max",       llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Memory Operations ─────────────────────────────────────────────────
    {"memcpy",    llvm::Intrinsic::memcpy,  3, 3, false, false},
    {"memmove",   llvm::Intrinsic::memmove, 3, 3, false, false},
    {"memset",    llvm::Intrinsic::memset,  3, 3, false, false},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"clz",       llvm::Intrinsic::ctlz,    1, 1, false, false},
    {"ctz",       llvm::Intrinsic::cttz,    1, 1, false, false},
    {"popcount",  llvm::Intrinsic::ctpop,   1, 1, false, false},
    {"bswap",     llvm::Intrinsic::bswap,   1, 1, false, false},

    // ─── CPU Hints ──────────────────────────────────────────────────────────
    {"prefetch",   llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"prefetch_r", llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"prefetch_w", llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"fence",      llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"pause",      llvm::Intrinsic::not_intrinsic, 0, 0, false, true},

    // ─── Atomics ────────────────────────────────────────────────────────────
    {"atomic_load",  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_store", llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_add",   llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_sub",   llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_and",   llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_or",    llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_xor",   llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_cas",   llvm::Intrinsic::not_intrinsic, 3, 3, false, true},

    // ─── Type & Value Inspection ──────────────────────────────────────────
    {"sizeof",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"alignof",   llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"typeof",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"nameof",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"tostr",     llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"ptrstr",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"addrof",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Pointer Operations ────────────────────────────────────────────────
    {"ptrOffset",  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"ptrDiff",    llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"toRef",      llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"toPtr",      llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"bitcast",    llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Branch Prediction ──────────────────────────────────────────────────
    {"likely",     llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"unlikely",   llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── String Operations ──────────────────────────────────────────────────
    {"str_len",       llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"str_ptr",       llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"str_from_ptr",  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_concat",    llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_slice",     llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"str_eq",        llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_byte_at",   llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Memory Management ──────────────────────────────────────────────────
    {"alloc",         llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"free",          llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_create",  llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_alloc",   llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"arena_reset",   llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_free",    llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Scope Exit Callback ──────────────────────────────────────────────
    {"scope_exit",    llvm::Intrinsic::not_intrinsic, 1, 0, true, true},  // 1+ args, variadic

    // ─── SIMD ────────────────────────────────────────────────────────────────
    {"simd_add",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_sub",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_mul",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_div",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_fma",      llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"simd_min",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_max",      llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_load",     llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"simd_store",    llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_splat",    llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"simd_extract",  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_insert",   llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
};

static constexpr size_t INTRINSIC_COUNT = sizeof(INTRINSIC_TABLE) / sizeof(INTRINSIC_TABLE[0]);

// ─── Fence Orderings ──────────────────────────────────────────────────────

static const std::string_view FENCE_ORDERINGS[] = {
    "relaxed", "acquire", "release", "acq_rel", "seq_cst"
};

// ─── Singleton ──────────────────────────────────────────────────────────────

IntrinsicRegistry& IntrinsicRegistry::getInstance(StringPool& pool) {
    static IntrinsicRegistry instance(pool);
    return instance;
}

// ─── Constructor ────────────────────────────────────────────────────────────

IntrinsicRegistry::IntrinsicRegistry(StringPool& pool) : m_pool(pool) {
    for (const auto& entry : INTRINSIC_TABLE) {
        InternedString name = m_pool.intern(entry.name);
        IntrinsicInfo info(entry.llvmID, name, entry.minArgs, 
                           entry.maxArgs, entry.isVarArg, entry.isCompilerHandled);
        m_intrinsics[name] = info;
        if (entry.isCompilerHandled) {
            m_compilerHandled.insert(name);
        }
    }
}

// ─── Query Methods ─────────────────────────────────────────────────────────

const IntrinsicInfo* IntrinsicRegistry::getInfo(InternedString name) const {
    auto it = m_intrinsics.find(name);
    if (it != m_intrinsics.end()) {
        return &it->second;
    }
    return nullptr;
}

std::optional<llvm::Intrinsic::ID> IntrinsicRegistry::getLLVMID(InternedString name) const {
    auto* info = getInfo(name);
    if (info && info->isValid()) {
        return info->llvmID;
    }
    return std::nullopt;
}

bool IntrinsicRegistry::isCompilerHandled(InternedString name) const {
    return m_compilerHandled.find(name) != m_compilerHandled.end();
}

size_t IntrinsicRegistry::getMinArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->minArgs : 0;
}

size_t IntrinsicRegistry::getMaxArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->maxArgs : 0;
}

bool IntrinsicRegistry::isVariadic(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->isVarArg : false;
}

bool IntrinsicRegistry::hasFixedArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->hasFixedArgs() : false;
}

// ─── Fence Ordering ─────────────────────────────────────────────────────────

bool IntrinsicRegistry::isValidFenceOrdering(std::string_view ordering) {
    for (const auto& valid : FENCE_ORDERINGS) {
        if (ordering == valid) {
            return true;
        }
    }
    return false;
}

// ─── Listing ────────────────────────────────────────────────────────────────

std::vector<InternedString> IntrinsicRegistry::getAllNames() const {
    std::vector<InternedString> names;
    names.reserve(m_intrinsics.size());
    for (const auto& pair : m_intrinsics) {
        names.push_back(pair.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> IntrinsicRegistry::getAllNamesAsStrings() const {
    std::vector<std::string> names;
    names.reserve(m_intrinsics.size());
    for (const auto& pair : m_intrinsics) {
        names.push_back(m_pool.lookup(pair.first));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> IntrinsicRegistry::getLLVMIntrinsicNames() const {
    std::vector<std::string> names;
    for (const auto& pair : m_intrinsics) {
        if (pair.second.isValid()) {
            names.push_back(m_pool.lookup(pair.first));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> IntrinsicRegistry::getCompilerIntrinsicNames() const {
    std::vector<std::string> names;
    for (const auto& name : m_compilerHandled) {
        names.push_back(m_pool.lookup(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}
