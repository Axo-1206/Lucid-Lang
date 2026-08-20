/// @file registry/IntrinsicRegistry.hpp
/// @brief Pure data registry for intrinsics - no semantic or codegen dependencies.

#pragma once

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "llvm/IR/Intrinsics.h"
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <string_view>
#include <vector>

/// @brief Enumerates every intrinsic by name, mirroring RuntimeFn's role
/// for runtime library functions. Resolve InternedString -> IntrinsicKind
/// ONCE (via IntrinsicRegistry::getInfo), then dispatch on the enum
/// everywhere downstream - never re-compare against a string (interned or
/// not) at every dispatch layer.
enum class IntrinsicKind {
    // ─── Floating-Point Math ────────────────────────────────────────────
    Sqrt, Abs, Fma, Ceil, Floor, Round, Pow, Min, Max,
    // ─── Memory Operations ───────────────────────────────────────────────
    Memcpy, Memmove, Memset,
    // ─── Bit Manipulation ────────────────────────────────────────────────
    Clz, Ctz, Popcount, Bswap,
    // ─── CPU Hints ────────────────────────────────────────────────────────
    Prefetch, PrefetchR, PrefetchW, Fence, Pause,
    // ─── Atomics ─────────────────────────────────────────────────────────
    AtomicLoad, AtomicStore, AtomicAdd, AtomicSub,
    AtomicAnd, AtomicOr, AtomicXor, AtomicCas,
    // ─── Type & Value Inspection ────────────────────────────────────────
    Sizeof, Alignof, Typeof, Nameof, Tostr, Ptrstr, Addrof,
    // ─── Pointer Operations ──────────────────────────────────────────────
    PtrOffset, PtrDiff, ToRef, ToPtr,
    // ─── Bit Manipulation (type-punning) ────────────────────────────────
    Bitcast,
    // ─── Branch Prediction ───────────────────────────────────────────────
    Likely, Unlikely,
    // ─── String Operations ───────────────────────────────────────────────
    StrLen, StrPtr, StrFromPtr, StrConcat, StrSlice, StrEq, StrByteAt,
    // ─── Memory Management ───────────────────────────────────────────────
    Alloc, Free, ArenaCreate, ArenaAlloc, ArenaReset, ArenaFree,
    // ─── Scope Exit Callback ─────────────────────────────────────────────
    ScopeExit,
    // ─── SIMD ────────────────────────────────────────────────────────────
    SimdAdd, SimdSub, SimdMul, SimdDiv, SimdFma, SimdMin, SimdMax,
    SimdLoad, SimdStore, SimdSplat, SimdExtract, SimdInsert,
};

/// @brief Which emitter module handles this intrinsic's codegen.
///
/// This is NOT the same axis as IntrinsicInfo::isCompilerHandled /
/// isValid() (whether the intrinsic maps to a literal llvm::Intrinsic::ID).
/// Several intrinsics have no literal llvm::Intrinsic::ID (fence, pause,
/// every atomic_*, every simd_*, min, max - all isCompilerHandled=true)
/// but are still emitted from LLVMIntrinsicEmitter.cpp, because that file
/// is organized by "maps to a native LLVM construct" (a real IR
/// instruction or Intrinsic::ID), not strictly "has an Intrinsic::ID".
/// Conflating these two axes was exactly the bug in the old
/// isLLVMIntrinsic()/isLucidIntrinsic() hardcoded sets in
/// IntrinsicEmitter.cpp: they silently reimplemented a second, differently
/// -shaped classification instead of reading isCompilerHandled, and there
/// was nothing keeping the two in sync. This field is the actual, single
/// source of truth for "which emitter file", kept deliberately distinct
/// from isCompilerHandled so neither one has to pretend to mean the other.
enum class IntrinsicEmitterKind {
    LLVM,   // LLVMIntrinsicEmitter.cpp
    Lucid,  // LucidIntrinsicEmitter.cpp
};

/// @brief Information about a registered intrinsic.
struct IntrinsicInfo {
    llvm::Intrinsic::ID llvmID;
    InternedString name;
    IntrinsicKind kind;
    IntrinsicEmitterKind emitterKind;
    size_t minArgs;
    size_t maxArgs;
    bool isVarArg;
    bool isCompilerHandled;

    IntrinsicInfo()
        : llvmID(llvm::Intrinsic::not_intrinsic)
        , kind(IntrinsicKind::Sqrt) // arbitrary default, never read: isValid()-style
                                    // callers should always check getInfo()'s
                                    // return for nullptr before touching kind
        , emitterKind(IntrinsicEmitterKind::Lucid)
        , minArgs(0)
        , maxArgs(0)
        , isVarArg(false)
        , isCompilerHandled(false) {}

    IntrinsicInfo(llvm::Intrinsic::ID id, InternedString n, IntrinsicKind k,
                  IntrinsicEmitterKind ek, size_t min, size_t max = 0,
                  bool varArg = false, bool compiler = false)
        : llvmID(id), name(n), kind(k), emitterKind(ek), minArgs(min),
          maxArgs(max ? max : min), isVarArg(varArg), isCompilerHandled(compiler) {}

    bool isValid() const { return llvmID != llvm::Intrinsic::not_intrinsic; }
    bool hasFixedArgs() const { return !isVarArg; }
};

/// @brief Intrinsic entry for the data table.
struct IntrinsicEntry {
    std::string_view name;
    IntrinsicKind kind;
    IntrinsicEmitterKind emitterKind;
    llvm::Intrinsic::ID llvmID;
    size_t minArgs;
    size_t maxArgs;
    bool isVarArg;
    bool isCompilerHandled;
};

/// @brief Core intrinsic registry - pure data, no semantic dependencies.
class IntrinsicRegistry {
public:
    static IntrinsicRegistry& getInstance(StringPool& pool);

    const IntrinsicInfo* getInfo(InternedString name) const;
    std::optional<llvm::Intrinsic::ID> getLLVMID(InternedString name) const;
    bool isCompilerHandled(InternedString name) const;

    /// @brief Which emitter file handles this intrinsic's codegen.
    /// Replaces the old isLLVMIntrinsic()/isLucidIntrinsic() hardcoded
    /// unordered_set<std::string> pair that used to live in
    /// IntrinsicEmitter.cpp - this is now registry data, kept in the same
    /// single table as everything else about an intrinsic, instead of a
    /// second, independently-maintained classification.
    /// @return The emitter kind, or std::nullopt if `name` isn't registered.
    std::optional<IntrinsicEmitterKind> getEmitterKind(InternedString name) const;

    size_t getMinArgs(InternedString name) const;
    size_t getMaxArgs(InternedString name) const;
    bool isVariadic(InternedString name) const;
    bool hasFixedArgs(InternedString name) const;

    static bool isValidFenceOrdering(std::string_view ordering);

    std::vector<InternedString> getAllNames() const;
    std::vector<std::string> getAllNamesAsStrings() const;
    std::vector<std::string> getLLVMIntrinsicNames() const;
    std::vector<std::string> getCompilerIntrinsicNames() const;

private:
    IntrinsicRegistry(StringPool& pool);
    ~IntrinsicRegistry() = default;

    IntrinsicRegistry(const IntrinsicRegistry&) = delete;
    IntrinsicRegistry& operator=(const IntrinsicRegistry&) = delete;

    StringPool& m_pool;
    std::unordered_map<InternedString, IntrinsicInfo> m_intrinsics;
    std::unordered_set<InternedString> m_compilerHandled;
    bool m_initialized = false;
};

// ─── Void Intrinsics Registry ─────────────────────────────────────────────

static const std::unordered_set<std::string> VOID_INTRINSICS = {
    // Memory Operations - no return value
    "memcpy", "memmove", "memset",
    
    // Memory Management - no return value (free operations)
    "free", "arena_free", "arena_reset",
    
    // Synchronization - no return value
    "fence",
    
    // CPU Hints - no return value
    "pause", "prefetch", "prefetch_r", "prefetch_w",
    
    // Scope Exit - no return value
    "scope_exit",
    
    // SIMD Store - no return value
    "simd_store",
    
    // Atomic Store - no return value
    "atomic_store",
};