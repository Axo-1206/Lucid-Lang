/**
 * @file IntrinsicRegistry.hpp
 * @brief Maps Lucid intrinsic names to LLVM intrinsic IDs and provides validation.
 *
 * @responsibility Provides compile-time and runtime mapping between Lucid
 *                 intrinsic names and their corresponding LLVM intrinsic IDs.
 *                 Also handles validation of arguments for each intrinsic.
 *
 * @related_files
 *   - src/codegen/IRLoweringIntrinsic.cpp - uses IntrinsicRegistry for lowering
 *   - src/sema/rules/SemaExpr.cpp - uses IntrinsicRegistry to validate
 *     #intrinsic() calls and set IntrinsicCallExprAST::intrinsicID during Sema
 *   - src/ast/ExprAST.hpp - IntrinsicCallExprAST node (its `intrinsicName`
 *     field is exactly the InternedString this registry is keyed on)
 *
 * @architectural_note Bound to one StringPool, for the registry's lifetime
 *   `IntrinsicInfo` used to be keyed by `std::string`, which meant every
 *   lookup either allocated a temporary `std::string` from the caller's
 *   `InternedString` (via `pool.lookup()` + a copy) or duplicated the same
 *   hashing work the caller's `StringPool::intern()` already did once. That
 *   defeats the entire point of `InternedString` — see InternedString.hpp's
 *   "Comparisons" note on why every subsystem in this codebase is expected
 *   to compare IDs, not text.
 *
 *   The registry now interns its ~60 canonical names into a `StringPool`
 *   exactly once, at construction, producing real `InternedString` keys.
 *   Every query thereafter (`getIntrinsicInfo()`, `validateArgCount()`, the
 *   hot path called on every single `#name(...)` site during Sema) is a
 *   `uint32_t`-keyed hash lookup, not a string compare.
 *
 *   This only works because `InternedString` IDs are meaningless outside
 *   the specific `StringPool` that minted them (see StringPool.hpp — IDs
 *   are sequential per-pool, not content-addressed). So this registry must
 *   be bound to the *same* `StringPool` as the caller — in practice, the
 *   one `StringPool` a `CompilerSession` owns for its whole run (see
 *   SemaContext.hpp's architectural note: "one StringPool ... for the
 *   whole (possibly multi-module) semantic analysis"). `getInstance(pool)`
 *   binds to whichever pool is passed on its *first* call for the process's
 *   lifetime and asserts every later call passes that same pool — the same
 *   "single-threaded, on purpose" / one-session-per-process invariant
 *   `diagnostic::`'s global state already relies on (see Diagnostic.cpp).
 *
 * @architectural_note Prefetch Family (#prefetch, #prefetch_r, #prefetch_w)
 *   All three map to the same LLVM intrinsic `llvm.prefetch`, differentiated
 *   by the `rw` (read/write) argument:
 *     - #prefetch(ptr)   → rw=0 (read), default, general use
 *     - #prefetch_r(ptr) → rw=0 (read), explicit read prefetch
 *     - #prefetch_w(ptr) → rw=1 (write), explicit write prefetch
 *   The `_r` and `_w` suffixes make the intent explicit for performance-
 *   critical code, while `#prefetch` remains the simple default.
 *
 * @architectural_note Fence Ordering Validation
 *   #fence(ordering) is compiler-handled (not an LLVM intrinsic).
 *   The ordering string is validated at compile time. Valid values:
 *     - "relaxed" - No synchronization, only atomicity
 *     - "acquire" - Subsequent reads/writes see prior releases
 *     - "release" - Prior reads/writes visible before this op
 *     - "acq_rel" - Both acquire and release (read-modify-write)
 *     - "seq_cst" - Total sequential consistency
 *   Emits LLVM 'fence' instruction directly during code generation.
 *
 * @architectural_note SIMD Splat (#simd_splat) - Type Argument Handling
 *   #simd_splat(T, N, scalar) is compiler-handled because:
 *     1. T is a type (e.g., float32), not a value — requires type resolution
 *     2. N must be a compile-time integer constant
 *     3. There is no direct LLVM intrinsic for splat — it's lowered to:
 *        `insertelement` + `shufflevector` or `vector_splat` (LLVM 18+)
 *   The compiler validates that T is a valid SIMD element type and that N
 *   is a compile-time constant within the target's vector register limits.
 *
 * @architectural_note AttributesRegistry vs IntrinsicRegistry
 *   IntrinsicRegistry is a class with state (maps of names to LLVM IDs)
 *   because it stores data that must persist across the compilation session.
 *   AttributeRegistry is header-only because it has no state — it only
 *   validates attributes against declarations using pure functions.
 */

#pragma once

#include "llvm/IR/Intrinsics.h"

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>
#include <string>
#include <cstddef>

/**
 * @brief Information about a registered intrinsic.
 */
struct IntrinsicInfo {
    llvm::Intrinsic::ID id;              // LLVM intrinsic ID
    InternedString name;                  // Full intrinsic name, interned
                                           // into the registry's bound pool
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

/**
 * @brief Registry for intrinsic validation and lookup.
 *
 * Provides:
 *   - Mapping from Lucid intrinsic names to LLVM intrinsic IDs
 *   - Argument count validation
 *   - Type parameter inference for overloaded intrinsics
 *   - Detection of compiler-handled intrinsics (no LLVM enum)
 *
 * @note This class has state (maps) and is appropriate as a singleton.
 *       Compare with `attr::validateAttributes()` which is stateless and
 *       header-only. See architectural note above for the distinction.
 */
class IntrinsicRegistry {
public:
    /**
     * @brief Get the singleton instance, bound to `pool`.
     *
     * The *first* call in the process's lifetime interns every canonical
     * intrinsic name into `pool` and that binding is permanent — see this
     * file's "Bound to one StringPool" note above. Every subsequent call
     * must pass that same `pool`; passing a different one is a caller bug
     * (two StringPools in one process means the process is analyzing two
     * unrelated sessions concurrently, which contradicts the single-
     * threaded, strictly sequential pipeline this registry assumes) and is
     * caught by an assert in debug builds.
     *
     * @param pool The session's StringPool — pass `ctx.pool` from Sema, or
     *             the equivalent pool codegen's lowering context holds.
     */
    static IntrinsicRegistry& getInstance(StringPool& pool);

    /**
     * @brief Get the LLVM intrinsic ID for a Lucid intrinsic name.
     *
     * @param name The Lucid intrinsic name, already interned into the same
     *             pool this registry is bound to (e.g.
     *             `IntrinsicCallExprAST::intrinsicName`).
     * @return std::optional<llvm::Intrinsic::ID> The LLVM ID, or nullopt if not found
     */
    std::optional<llvm::Intrinsic::ID> getLLVMIntrinsicID(InternedString name) const;

    /**
     * @brief Get information about an intrinsic.
     *
     * @param name The Lucid intrinsic name
     * @return const IntrinsicInfo* Pointer to info, or nullptr if not found
     */
    const IntrinsicInfo* getIntrinsicInfo(InternedString name) const;

    /**
     * @brief Check if an intrinsic is compiler-handled (no LLVM enum).
     *
     * Compiler-handled intrinsics are validated and lowered by the compiler
     * itself, not mapped to an LLVM intrinsic. Examples:
     *   - #fence - Emits LLVM 'fence' instruction
     *   - #simd_splat - Lowers to vector operations
     *   - #sizeof - Resolved at compile time
     */
    bool isCompilerIntrinsic(InternedString name) const;

    /**
     * @brief Check if an intrinsic maps to an LLVM intrinsic.
     */
    bool isLLVMIntrinsic(InternedString name) const;

    /**
     * @brief Validate the number of arguments for an intrinsic.
     *
     * @param name The Lucid intrinsic name
     * @param argCount The number of arguments provided
     * @return true if the argument count is valid
     */
    bool validateArgCount(InternedString name, size_t argCount) const;

    /**
     * @brief Get the expected argument count for an intrinsic.
     *
     * @return std::optional<size_t> The expected count, or nullopt if variable/range
     */
    std::optional<size_t> getExpectedArgCount(InternedString name) const;

    /**
     * @brief Validate fence ordering string.
     *
     * #fence(ordering) takes an ordering string that must be one of:
     *   - "relaxed", "acquire", "release", "acq_rel", "seq_cst"
     *
     * @param ordering The ordering string as an InternedString
     * @param pool The StringPool for lookup
     * @return true if the ordering is valid
     */
    bool validateFenceOrdering(InternedString ordering, StringPool& pool) const;

    /**
     * @brief Get all registered intrinsic names, sorted, as display text.
     *
     * Returns `std::string` (not `InternedString`) since the only callers
     * of this one — diagnostics listing valid intrinsics, LSP completion —
     * need text, not an ID to compare with. Sorting also has to happen on
     * text: InternedString's whole point is that its integer order carries
     * no meaning (see InternedString.hpp), so sorting IDs would sort by
     * "which name happened to get interned first," not alphabetically.
     */
    std::vector<std::string> getAllIntrinsicNames() const;
    std::vector<std::string> getLLVMIntrinsicNames() const;
    std::vector<std::string> getCompilerIntrinsicNames() const;

private:
    explicit IntrinsicRegistry(StringPool& pool);
    ~IntrinsicRegistry() = default;

    IntrinsicRegistry(const IntrinsicRegistry&) = delete;
    IntrinsicRegistry& operator=(const IntrinsicRegistry&) = delete;

    void registerIntrinsics();

    /// `lucidName` is a `string_view` over a string-literal (all call sites
    /// in registerIntrinsics() pass literals) — interned into `m_pool` once
    /// here, at registry construction, not re-interned per query.
    void registerLLVMIntrinsic(std::string_view lucidName,
                                llvm::Intrinsic::ID llvmID,
                                size_t minArgs,
                                size_t maxArgs = 0,
                                bool isVarArg = false);

    void registerCompilerIntrinsic(std::string_view name,
                                    size_t minArgs,
                                    size_t maxArgs = 0,
                                    bool isVarArg = false);

    // ─── Members ─────────────────────────────────────────────────────────

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