/**
 * @file QualifierRegistry.hpp
 * @brief Registry for Luc qualifiers (`~async`, `~nullable`, `~parallel`).
 *
 * Provides metadata about each qualifier: name, bitmask, whether it affects
 * type equality, and valid declaration contexts. Lookups are O(1) using
 * InternedString keys.
 *
 * @see QualifierRegistry.cpp
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// QualifierBits – bitmask values for qualifiers
// ─────────────────────────────────────────────────────────────────────────────
namespace QualifierBits {
    constexpr uint32_t Async    = 1 << 0;   ///< ~async
    constexpr uint32_t Nullable = 1 << 1;   ///< ~nullable
    constexpr uint32_t Parallel = 1 << 2;   ///< ~parallel
    // Bits 3–31 reserved for future qualifiers
}

// ─────────────────────────────────────────────────────────────────────────────
// QualifierContext – where a qualifier may appear (bitmask)
// ─────────────────────────────────────────────────────────────────────────────
enum class QualifierContext : uint32_t {
    None      = 0,
    Function  = 1 << 0,   ///< On a function declaration
    Variable  = 1 << 1,   ///< On a variable (e.g., `let f ~nullable ...`)
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
// QualifierEntry – metadata for a single built‑in qualifier
// ─────────────────────────────────────────────────────────────────────────────
struct QualifierEntry {
    InternedString id;                 // Interned key (filled by initialize)
    std::string_view name;             // Human‑readable name (without `~`)
    uint32_t bit;                      // Bitmask value (from QualifierBits)
    bool affectsTypeEquality;          // Does this qualifier affect type identity?
    QualifierContext validContexts;    // Where this qualifier may appear
    DiagCode errorCode;                // primary diagnostic code for this qualifier
    std::string_view description;      // Human‑readable description
};

// ─────────────────────────────────────────────────────────────────────────────
// namespace `qualifier` – procedural interface for qualifier metadata
// ─────────────────────────────────────────────────────────────────────────────
namespace qualifier {

// Must be called once before any lookup (usually after creating StringPool)
void initialize(StringPool& pool);

// Call before destroying the StringPool to avoid dangling references
void shutdown();

// Lookup by interned ID (O(1))
const QualifierEntry* lookup(InternedString id);

// Lookup by name (interning on the fly)
const QualifierEntry* lookup(std::string_view name);

// Get the pre‑interned ID for a known qualifier (or invalid ID if unknown)
InternedString getId(std::string_view name);

// Quick existence checks
bool isKnown(InternedString id);
bool isKnown(std::string_view name);

// Get bitmask for a qualifier (0 if unknown)
uint32_t getBit(InternedString id);
uint32_t getBit(std::string_view name);

// Apply a qualifier to a bitmask (returns false if unknown)
bool applyQualifier(uint32_t& mask, InternedString id);
bool applyQualifier(uint32_t& mask, std::string_view name);

// Validate that a qualifier can be used in a given context.
// Returns true if valid; reports a diagnostic otherwise.
bool validateUsage(const QualifierEntry& entry,
                   QualifierContext ctx,
                   InternedString file,
                   const SourceLocation& loc);

// Get all qualifier names as a comma‑separated string (for error messages)
std::string allNames();

// Bitmask of qualifiers that affect type equality (pre‑computed)
uint32_t equalityMask();

// Convenience accessors for commonly used qualifiers (parser/codegen)
InternedString getAsyncId();
InternedString getNullableId();
InternedString getParallelId();

} // namespace qualifier