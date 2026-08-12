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

/// @brief Information about a registered intrinsic.
struct IntrinsicInfo {
    llvm::Intrinsic::ID llvmID;
    InternedString name;
    size_t minArgs;
    size_t maxArgs;
    bool isVarArg;
    bool isCompilerHandled;
    
    IntrinsicInfo()
        : llvmID(llvm::Intrinsic::not_intrinsic)
        , minArgs(0)
        , maxArgs(0)
        , isVarArg(false)
        , isCompilerHandled(false) {}
    
    IntrinsicInfo(llvm::Intrinsic::ID id, InternedString n,
                  size_t min, size_t max = 0, bool varArg = false, bool compiler = false)
        : llvmID(id), name(n), minArgs(min), maxArgs(max ? max : min),
          isVarArg(varArg), isCompilerHandled(compiler) {}
    
    bool isValid() const { return llvmID != llvm::Intrinsic::not_intrinsic; }
    bool hasFixedArgs() const { return !isVarArg; }
};

/// @brief Intrinsic entry for the data table.
struct IntrinsicEntry {
    std::string_view name;
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