/**
 * @file IntrinsicRegistry.hpp
 * @brief Maps Lucid intrinsic names to LLVM intrinsic IDs and provides validation.
 *
 * @responsibility Provides:
 *   - Mapping from Lucid intrinsic names to LLVM intrinsic IDs
 *   - Argument count validation
 *   - Argument type validation per intrinsic
 *   - Return type determination per intrinsic
 *   - Value state determination per intrinsic
 *   - Detection of compiler-handled intrinsics (no LLVM enum)
 *
 * @architectural_note Bound to one StringPool, for the registry's lifetime
 *   The registry interns its ~60 canonical names into a `StringPool`
 *   exactly once, at construction, producing real `InternedString` keys.
 *   Every query thereafter is a `uint32_t`-keyed hash lookup.
 *
 * @architectural_note Integration with SemaContext
 *   The registry uses SemaContext for error reporting and type predicates.
 *   It assumes expressions have been resolved (resolvedType is set).
 */

#pragma once

#include "llvm/IR/Intrinsics.h"

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "../context/SemaContext.hpp"
#include "../support/LiteralHelpers.hpp"
#include "../types/SemaCompare.hpp"   // Type predicates: isIntegerType, isNumericType, etc.

#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>
#include <string>
#include <cstddef>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicInfo
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Information about a registered intrinsic.
struct IntrinsicInfo {
    llvm::Intrinsic::ID id;               // LLVM intrinsic ID
    InternedString name;                  // Full intrinsic name, interned
    size_t minArgs;                       // Minimum number of arguments
    size_t maxArgs;                       // Maximum number of arguments
    bool isVarArg;                        // Whether the intrinsic is variadic
    std::vector<llvm::Type*> typeParams;  // Expected type parameters (if any)

    IntrinsicInfo()
        : id(llvm::Intrinsic::not_intrinsic)
        , minArgs(0)
        , maxArgs(0)
        , isVarArg(false) {}

    IntrinsicInfo(llvm::Intrinsic::ID id, InternedString name,
                  size_t minArgs, size_t maxArgs = 0, bool isVarArg = false)
        : id(id), name(name), minArgs(minArgs), maxArgs(maxArgs), isVarArg(isVarArg) {}

    bool isValid() const { return id != llvm::Intrinsic::not_intrinsic; }
    bool hasFixedArgs() const { return !isVarArg && maxArgs > 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry
// ─────────────────────────────────────────────────────────────────────────────

class IntrinsicRegistry {
public:
    static IntrinsicRegistry& getInstance(StringPool& pool);

    // ─── Lookup Methods ──────────────────────────────────────────────────────

    std::optional<llvm::Intrinsic::ID> getLLVMIntrinsicID(InternedString name) const;
    const IntrinsicInfo* getIntrinsicInfo(InternedString name) const;
    bool isCompilerIntrinsic(InternedString name) const;
    bool isLLVMIntrinsic(InternedString name) const;

    // ─── Argument Validation ────────────────────────────────────────────────

    bool validateArgCount(InternedString name, size_t argCount) const;
    std::optional<size_t> getExpectedArgCount(InternedString name) const;

    /// @brief Validate fence ordering string.
    bool validateFenceOrdering(InternedString ordering, StringPool& pool) const;

    // ─── Intrinsic Call Validation ──────────────────────────────────────────

    /// @brief Validate the entire intrinsic call.
    bool validateIntrinsicCall(const IntrinsicCallExprAST* expr,
                                SemaContext& ctx) const;

    /// @brief Get the return type of an intrinsic call.
    const TypeAST* getIntrinsicReturnType(const IntrinsicCallExprAST* expr,
                                           const TypeAST* targetType,
                                           SemaContext& ctx) const;

    /// @brief Get the value state of an intrinsic call.
    ValueState getIntrinsicValueState(const IntrinsicCallExprAST* expr,
                                       SemaContext& ctx) const;

    // ─── Listing Methods ────────────────────────────────────────────────────

    std::vector<std::string> getAllIntrinsicNames() const;
    std::vector<std::string> getLLVMIntrinsicNames() const;
    std::vector<std::string> getCompilerIntrinsicNames() const;

private:
    explicit IntrinsicRegistry(StringPool& pool);
    ~IntrinsicRegistry() = default;

    IntrinsicRegistry(const IntrinsicRegistry&) = delete;
    IntrinsicRegistry& operator=(const IntrinsicRegistry&) = delete;

    void registerIntrinsics();

    void registerLLVMIntrinsic(std::string_view lucidName,
                                llvm::Intrinsic::ID llvmID,
                                size_t minArgs,
                                size_t maxArgs = 0,
                                bool isVarArg = false);

    void registerCompilerIntrinsic(std::string_view name,
                                    size_t minArgs,
                                    size_t maxArgs = 0,
                                    bool isVarArg = false);

    // ─── Validation Helper Methods ──────────────────────────────────────────

    bool validatePtrArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;
    bool validateNumericArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;
    bool validateIntArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;
    bool validateStringArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;
    bool validateBoolArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;
    bool validateRefArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) const;

    // ─── Members ─────────────────────────────────────────────────────────────

    StringPool& m_pool;
    std::unordered_map<InternedString, IntrinsicInfo> m_intrinsicMap;
    std::unordered_set<InternedString> m_compilerIntrinsics;
    std::unordered_set<InternedString> m_llvmIntrinsics;
    bool m_initialized = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience Functions
// ─────────────────────────────────────────────────────────────────────────────

inline std::optional<llvm::Intrinsic::ID> getIntrinsicID(InternedString name, StringPool& pool) {
    return IntrinsicRegistry::getInstance(pool).getLLVMIntrinsicID(name);
}

inline bool isCompilerIntrinsic(InternedString name, StringPool& pool) {
    return IntrinsicRegistry::getInstance(pool).isCompilerIntrinsic(name);
}

inline bool isLLVMIntrinsic(InternedString name, StringPool& pool) {
    return IntrinsicRegistry::getInstance(pool).isLLVMIntrinsic(name);
}

inline bool validateFenceOrdering(InternedString ordering, StringPool& pool) {
    return IntrinsicRegistry::getInstance(pool).validateFenceOrdering(ordering, pool);
}

} // namespace sema