/**
 * @file QualifierRegistry.hpp
 * @brief Registry for Luc qualifiers (`~async`, `~nullable`, `~parallel`).
 *
 * Provides metadata about each qualifier: name, bitmask, whether it affects
 * type equality, and valid declaration contexts.
 *
 * @see QualifierRegistry.cpp
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>

class DiagnosticEngine;

namespace QualifierBits {
    constexpr uint32_t Async    = 1 << 0;   ///< ~async
    constexpr uint32_t Nullable = 1 << 1;   ///< ~nullable
    constexpr uint32_t Parallel = 1 << 2;   ///< ~parallel
    // Bits 3–31 reserved for future qualifiers
}

// ─────────────────────────────────────────────────────────────────────────────
// QualifierContext – where a qualifier may appear
// ─────────────────────────────────────────────────────────────────────────────
enum class QualifierContext : uint32_t {
    None      = 0,
    Function  = 1 << 0,   ///< On a function declaration
    Variable  = 1 << 1,   ///< On a variable (e.g., `let f ~nullable ...`)
    Struct    = 1 << 2,   ///< On a struct (e.g., `~packed struct`)
    Parameter = 1 << 3,   ///< In a function parameter type
    Return    = 1 << 4,   ///< In a return type
    TypeAlias = 1 << 5,   ///< In a type alias
};

inline QualifierContext operator|(QualifierContext a, QualifierContext b) {
    return static_cast<QualifierContext>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasFlag(QualifierContext value, QualifierContext flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// QualifierInfo – metadata about one built‑in qualifier
// ─────────────────────────────────────────────────────────────────────────────
struct QualifierInfo {
    InternedString id;                 ///< Interned key (filled by setStringPool)
    std::string_view name;             ///< Human‑readable name (without `~`)
    uint32_t bit;                      ///< Bitmask value (from QualifierBits)
    bool affectsTypeEquality;          ///< Does this qualifier affect type identity?
    QualifierContext validContexts;    ///< Where this qualifier may appear
    std::string_view description;      ///< Human‑readable description
};

// ─────────────────────────────────────────────────────────────────────────────
// QualifierRegistry – singleton providing qualifier metadata
// ─────────────────────────────────────────────────────────────────────────────
class QualifierRegistry {
public:
    static QualifierRegistry& instance();

    // Must be called once before any lookups (usually after creating StringPool)
    void setStringPool(StringPool& pool);
    
    // Call before destroying the StringPool to avoid dangling references
    void resetStringPool();

    // Lookup by interned ID (O(1))
    const QualifierInfo* lookup(InternedString id) const;

    // Lookup by name (interning on the fly)
    const QualifierInfo* lookup(std::string_view name) const;

    // Get bitmask for a qualifier (0 if unknown)
    uint32_t getBit(InternedString id) const;
    uint32_t getBit(std::string_view name) const;

    // Check if a qualifier is known
    bool isValid(InternedString id) const;
    bool isValid(std::string_view name) const;

    // Apply a qualifier to a bitmask (returns false if unknown)
    bool applyQualifier(uint32_t& mask, InternedString id) const;
    bool applyQualifier(uint32_t& mask, std::string_view name) const;

    // Validate that a qualifier can be used in a given context.
    // Returns true if valid; reports a diagnostic otherwise.
    bool validateUsage(const QualifierInfo& info,
                       QualifierContext ctx,
                       DiagnosticEngine& dc,
                       const SourceLocation& loc) const;

    // Get all qualifier names as a comma‑separated string (for error messages)
    std::string allNames() const;

    // Bitmask of qualifiers that affect type equality (pre‑computed)
    uint32_t equalityMask() const { return cachedEqualityMask; }

    // Convenience accessors for commonly used qualifiers (parser/codegen)
    InternedString getAsyncId()    const { return asyncId; }
    InternedString getNullableId() const { return nullableId; }
    InternedString getParallelId() const { return parallelId; }

private:
    QualifierRegistry();
    void buildMap();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, QualifierInfo> byId;
    std::unordered_map<std::string, InternedString> nameToId;

    // Pre‑interned IDs for well‑known qualifiers
    InternedString asyncId, nullableId, parallelId;
    uint32_t cachedEqualityMask = 0;
};