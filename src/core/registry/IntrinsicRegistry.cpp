/// @file registry/IntrinsicRegistry.cpp
/// @brief Implementation of intrinsic registry.

#include "IntrinsicRegistry.hpp"
#include "core/memory/StringPool.hpp"
#include <algorithm>

// ─── Data Table ─────────────────────────────────────────────────────────────
//
// emitterKind here is transcribed directly from the old isLLVMIntrinsic()/
// isLucidIntrinsic() hardcoded sets in IntrinsicEmitter.cpp - e.g. fence,
// pause, every atomic_*, every simd_*, min, and max were all in the "LLVM"
// set there despite isCompilerHandled=true (no literal llvm::Intrinsic::ID)
// below, because LLVMIntrinsicEmitter.cpp is where their codegen actually
// lives. That's intentional, not a mismatch - see IntrinsicEmitterKind's
// doc comment in the header for why these two columns legitimately differ.

// IntrinsicEntry is defined at namespace scope in IntrinsicRegistry.hpp
static const IntrinsicEntry INTRINSIC_TABLE[] = {
    // ─── Floating-Point Math ──────────────────────────────────────────────
    {"sqrt",      IntrinsicKind::Sqrt,  IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::sqrt,   1, 1, false, false},
    {"abs",       IntrinsicKind::Abs,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::fabs,   1, 1, false, false},
    {"fma",       IntrinsicKind::Fma,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::fma,    3, 3, false, false},
    {"ceil",      IntrinsicKind::Ceil,  IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::ceil,   1, 1, false, false},
    {"floor",     IntrinsicKind::Floor, IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::floor,  1, 1, false, false},
    {"round",     IntrinsicKind::Round, IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::round,  1, 1, false, false},
    {"pow",       IntrinsicKind::Pow,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::pow,    2, 2, false, false},
    {"min",       IntrinsicKind::Min,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"max",       IntrinsicKind::Max,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Memory Operations ─────────────────────────────────────────────────
    {"memcpy",    IntrinsicKind::Memcpy,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memcpy,  3, 3, false, false},
    {"memmove",   IntrinsicKind::Memmove, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memmove, 3, 3, false, false},
    {"memset",    IntrinsicKind::Memset,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memset,  3, 3, false, false},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"clz",       IntrinsicKind::Clz,      IntrinsicEmitterKind::LLVM, llvm::Intrinsic::ctlz,    1, 1, false, false},
    {"ctz",       IntrinsicKind::Ctz,      IntrinsicEmitterKind::LLVM, llvm::Intrinsic::cttz,    1, 1, false, false},
    {"popcount",  IntrinsicKind::Popcount, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::ctpop,   1, 1, false, false},
    {"bswap",     IntrinsicKind::Bswap,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::bswap,   1, 1, false, false},

    // ─── CPU Hints ──────────────────────────────────────────────────────────
    {"prefetch",   IntrinsicKind::Prefetch,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"prefetch_r", IntrinsicKind::PrefetchR, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"prefetch_w", IntrinsicKind::PrefetchW, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false},
    {"fence",      IntrinsicKind::Fence,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"pause",      IntrinsicKind::Pause,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 0, 0, false, true},

    // ─── Atomics ────────────────────────────────────────────────────────────
    {"atomic_load",  IntrinsicKind::AtomicLoad,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_store", IntrinsicKind::AtomicStore, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_add",   IntrinsicKind::AtomicAdd,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_sub",   IntrinsicKind::AtomicSub,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_and",   IntrinsicKind::AtomicAnd,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_or",    IntrinsicKind::AtomicOr,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_xor",   IntrinsicKind::AtomicXor,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"atomic_cas",   IntrinsicKind::AtomicCas,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},

    // ─── Type & Value Inspection ──────────────────────────────────────────
    {"sizeof",    IntrinsicKind::Sizeof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"alignof",   IntrinsicKind::Alignof, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"typeof",    IntrinsicKind::Typeof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"nameof",    IntrinsicKind::Nameof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"tostr",     IntrinsicKind::Tostr,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"ptrstr",    IntrinsicKind::Ptrstr,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"addrof",    IntrinsicKind::Addrof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Pointer Operations ────────────────────────────────────────────────
    {"ptrOffset",  IntrinsicKind::PtrOffset, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"ptrDiff",    IntrinsicKind::PtrDiff,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"toRef",      IntrinsicKind::ToRef,     IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"toPtr",      IntrinsicKind::ToPtr,     IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"bitcast",    IntrinsicKind::Bitcast, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Branch Prediction ──────────────────────────────────────────────────
    {"likely",     IntrinsicKind::Likely,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"unlikely",   IntrinsicKind::Unlikely, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── String Operations ──────────────────────────────────────────────────
    {"str_len",       IntrinsicKind::StrLen,      IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"str_ptr",       IntrinsicKind::StrPtr,      IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"str_from_ptr",  IntrinsicKind::StrFromPtr,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_concat",    IntrinsicKind::StrConcat,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_slice",     IntrinsicKind::StrSlice,    IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"str_eq",        IntrinsicKind::StrEq,       IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"str_byte_at",   IntrinsicKind::StrByteAt,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},

    // ─── Memory Management ──────────────────────────────────────────────────
    {"alloc",         IntrinsicKind::Alloc,       IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"free",          IntrinsicKind::Free,        IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_create",  IntrinsicKind::ArenaCreate, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_alloc",   IntrinsicKind::ArenaAlloc,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"arena_reset",   IntrinsicKind::ArenaReset,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"arena_free",    IntrinsicKind::ArenaFree,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},

    // ─── Scope Exit Callback ──────────────────────────────────────────────
    {"scope_exit",    IntrinsicKind::ScopeExit, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 0, true, true},  // 1+ args, variadic

    // ─── SIMD ────────────────────────────────────────────────────────────────
    {"simd_add",      IntrinsicKind::SimdAdd,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_sub",      IntrinsicKind::SimdSub,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_mul",      IntrinsicKind::SimdMul,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_div",      IntrinsicKind::SimdDiv,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_fma",      IntrinsicKind::SimdFma,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"simd_min",      IntrinsicKind::SimdMin,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_max",      IntrinsicKind::SimdMax,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_load",     IntrinsicKind::SimdLoad,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 1, 1, false, true},
    {"simd_store",    IntrinsicKind::SimdStore,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_splat",    IntrinsicKind::SimdSplat,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
    {"simd_extract",  IntrinsicKind::SimdExtract, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true},
    {"simd_insert",   IntrinsicKind::SimdInsert,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true},
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
        IntrinsicInfo info(entry.llvmID, name, entry.kind, entry.emitterKind,
                           entry.minArgs, entry.maxArgs, entry.isVarArg,
                           entry.isCompilerHandled);
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

std::optional<IntrinsicEmitterKind> IntrinsicRegistry::getEmitterKind(InternedString name) const {
    auto* info = getInfo(name);
    if (!info) return std::nullopt;
    return info->emitterKind;
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