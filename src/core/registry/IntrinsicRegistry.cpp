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
//
// ════════════════════════════════════════════════════════════════════════════
// isVoid column: true if the intrinsic returns no value.
// ─── Void intrinsics ──────────────────────────────────────────────────────
// Memory Operations: memcpy, memmove, memset
// CPU Hints: prefetch, prefetch_r, prefetch_w, fence, pause
// Atomics: atomic_store (others return a value)
// Memory Management: free
// Scope Exit: scope_exit
// SIMD: simd_store

static const IntrinsicEntry INTRINSIC_TABLE[] = {
    // ─── Floating-Point Math ──────────────────────────────────────────────
    {"sqrt",      IntrinsicKind::Sqrt,  IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::sqrt,   1, 1, false, false, false},
    {"abs",       IntrinsicKind::Abs,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::fabs,   1, 1, false, false, false},
    {"fma",       IntrinsicKind::Fma,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::fma,    3, 3, false, false, false},
    {"ceil",      IntrinsicKind::Ceil,  IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::ceil,   1, 1, false, false, false},
    {"floor",     IntrinsicKind::Floor, IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::floor,  1, 1, false, false, false},
    {"round",     IntrinsicKind::Round, IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::round,  1, 1, false, false, false},
    {"pow",       IntrinsicKind::Pow,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::pow,    2, 2, false, false, false},
    {"min",       IntrinsicKind::Min,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"max",       IntrinsicKind::Max,   IntrinsicEmitterKind::LLVM,  llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},

    // ─── Memory Operations ─────────────────────────────────────────────────
    {"memcpy",    IntrinsicKind::Memcpy,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memcpy,  3, 3, false, false, true},
    {"memmove",   IntrinsicKind::Memmove, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memmove, 3, 3, false, false, true},
    {"memset",    IntrinsicKind::Memset,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::memset,  3, 3, false, false, true},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"clz",       IntrinsicKind::Clz,      IntrinsicEmitterKind::LLVM, llvm::Intrinsic::ctlz,    1, 1, false, false, false},
    {"ctz",       IntrinsicKind::Ctz,      IntrinsicEmitterKind::LLVM, llvm::Intrinsic::cttz,    1, 1, false, false, false},
    {"popcount",  IntrinsicKind::Popcount, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::ctpop,   1, 1, false, false, false},
    {"bswap",     IntrinsicKind::Bswap,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::bswap,   1, 1, false, false, false},

    // ─── CPU Hints ──────────────────────────────────────────────────────────
    {"prefetch",   IntrinsicKind::Prefetch,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false, true},
    {"prefetch_r", IntrinsicKind::PrefetchR, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false, true},
    {"prefetch_w", IntrinsicKind::PrefetchW, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::prefetch, 1, 1, false, false, true},
    {"fence",      IntrinsicKind::Fence,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, true},
    {"pause",      IntrinsicKind::Pause,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 0, 0, false, true, true},

    // ─── Atomics ────────────────────────────────────────────────────────────
    {"atomic_load",  IntrinsicKind::AtomicLoad,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_store", IntrinsicKind::AtomicStore, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, true},
    {"atomic_add",   IntrinsicKind::AtomicAdd,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_sub",   IntrinsicKind::AtomicSub,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_and",   IntrinsicKind::AtomicAnd,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_or",    IntrinsicKind::AtomicOr,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_xor",   IntrinsicKind::AtomicXor,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"atomic_cas",   IntrinsicKind::AtomicCas,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true, false},

    // ─── Type & Value Inspection ──────────────────────────────────────────
    {"sizeof",    IntrinsicKind::Sizeof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"alignof",   IntrinsicKind::Alignof, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"typeof",    IntrinsicKind::Typeof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"nameof",    IntrinsicKind::Nameof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"tostr",     IntrinsicKind::Tostr,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"ptrstr",    IntrinsicKind::Ptrstr,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"addrof",    IntrinsicKind::Addrof,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},

    // ─── Pointer Operations ────────────────────────────────────────────────
    {"ptrOffset",  IntrinsicKind::PtrOffset, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"ptrDiff",    IntrinsicKind::PtrDiff,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"toRef",      IntrinsicKind::ToRef,     IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"toPtr",      IntrinsicKind::ToPtr,     IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},

    // ─── Bit Manipulation ──────────────────────────────────────────────────
    {"bitcast",    IntrinsicKind::Bitcast, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},

    // ─── Branch Prediction ──────────────────────────────────────────────────
    {"likely",     IntrinsicKind::Likely,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"unlikely",   IntrinsicKind::Unlikely, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},

    // ─── String Operations ──────────────────────────────────────────────────
    {"str_len",       IntrinsicKind::StrLen,      IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"str_ptr",       IntrinsicKind::StrPtr,      IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"str_from_ptr",  IntrinsicKind::StrFromPtr,  IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"str_concat",    IntrinsicKind::StrConcat,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"str_slice",     IntrinsicKind::StrSlice,    IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 3, 3, false, true, false},
    {"str_eq",        IntrinsicKind::StrEq,       IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"str_byte_at",   IntrinsicKind::StrByteAt,   IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},

    // ─── Memory Management ──────────────────────────────────────────────────
    // Note: Arena management is now handled by the builtin Arena type with :: operations.
    // Only #alloc and #free remain for explicit heap management in FFI contexts.
    {"alloc",         IntrinsicKind::Alloc,       IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"free",          IntrinsicKind::Free,        IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, true},

    // ─── Scope Exit Callback ──────────────────────────────────────────────
    {"scope_exit",    IntrinsicKind::ScopeExit, IntrinsicEmitterKind::Lucid, llvm::Intrinsic::not_intrinsic, 1, 0, true, true, true},

    // ─── SIMD ────────────────────────────────────────────────────────────────
    {"simd_add",      IntrinsicKind::SimdAdd,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_sub",      IntrinsicKind::SimdSub,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_mul",      IntrinsicKind::SimdMul,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_div",      IntrinsicKind::SimdDiv,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_fma",      IntrinsicKind::SimdFma,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true, false},
    {"simd_min",      IntrinsicKind::SimdMin,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_max",      IntrinsicKind::SimdMax,     IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_load",     IntrinsicKind::SimdLoad,    IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 1, 1, false, true, false},
    {"simd_store",    IntrinsicKind::SimdStore,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, true},
    {"simd_splat",    IntrinsicKind::SimdSplat,   IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true, false},
    {"simd_extract",  IntrinsicKind::SimdExtract, IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 2, 2, false, true, false},
    {"simd_insert",   IntrinsicKind::SimdInsert,  IntrinsicEmitterKind::LLVM, llvm::Intrinsic::not_intrinsic, 3, 3, false, true, false},
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
                           entry.isCompilerHandled, entry.isVoid);
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

// ─── isVoid query method ────────────────────────────────────────────

bool IntrinsicRegistry::isVoid(InternedString name) const {
    auto* info = getInfo(name);
    if (!info) return false;
    return info->isVoid;
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