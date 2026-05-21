/**
 * @file IntrinsicRegistry.hpp
 * @brief Singleton registry for Luc '#' intrinsics.
 *
 * Provides metadata about each built‑in intrinsic: name, LLVM intrinsic ID,
 * argument kinds, return kind, and argument count range. Lookups are O(1)
 * using InternedString keys.
 *
 * @see IntrinsicRegistry.cpp
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class DiagnosticEngine;

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicArgKind – what kind of argument an intrinsic expects
// ─────────────────────────────────────────────────────────────────────────────
enum class IntrinsicArgKind {
    TypeArg,        ///< A type (e.g., #sizeof(T))
    AnyValue,       ///< Any value (type checking deferred to semantics)
    IntValue,       ///< Integer value (int, uint, etc.)
    FloatValue,     ///< Floating‑point value (float, double)
    PtrValue,       ///< Raw pointer value (*T)
    SizeValue,      ///< Size value (uint64, usually compile‑time)
    StringValue,    ///< String value (for future string intrinsics)
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicReturnKind – what kind of value an intrinsic returns
// ─────────────────────────────────────────────────────────────────────────────
enum class IntrinsicReturnKind {
    Void,           ///< No return value
    Uint64,         ///< 64‑bit unsigned integer
    Float32,        ///< 32‑bit float
    Float64,        ///< 64‑bit double
    SameAsArg0,     ///< Same as first value argument's type
    SameAsArg1,     ///< Same as second value argument's type
    RefOfTypeArg0,  ///< Reference to the type specified by the first type argument
    Int64,          ///< 64‑bit signed integer
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicEntry – metadata for a single built‑in intrinsic
// ─────────────────────────────────────────────────────────────────────────────
struct IntrinsicEntry {
    InternedString id;                          ///< Pre‑interned key (filled by setStringPool)
    std::string_view lucName;                   ///< Luc name (e.g., "sizeof")
    std::string_view llvmID;                    ///< Corresponding LLVM intrinsic ID (or "llvm.none")
    std::vector<IntrinsicArgKind> argKinds;     ///< Kinds of arguments in order
    IntrinsicReturnKind returnKind;             ///< Kind of return value
    bool isOverloaded;                          ///< Whether the intrinsic has type overloads
    int minArgs;                                ///< Minimum number of value arguments
    int maxArgs;                                ///< Maximum number of value arguments (-1 = unlimited)
    std::string_view notes;                     ///< Human‑readable description (for docs)
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry – singleton providing intrinsic metadata
// ─────────────────────────────────────────────────────────────────────────────
class IntrinsicRegistry {
public:
    static IntrinsicRegistry& instance();

    // Must be called once before any lookups (usually after creating StringPool)
    void setStringPool(StringPool& pool);
    
    // Call before destroying the StringPool to avoid dangling references
    void resetStringPool();

    // Lookup by interned ID (O(1))
    const IntrinsicEntry* lookup(InternedString id) const;

    // Lookup by name (interning on the fly)
    const IntrinsicEntry* lookup(std::string_view name) const;

    // Get the pre‑interned ID for a known intrinsic (or 0 if unknown)
    InternedString getId(std::string_view name) const;

    // Quick existence checks
    bool isKnown(InternedString id) const;
    bool isKnown(std::string_view name) const;

    // Returns a comma‑separated list of all intrinsic names (for error messages)
    std::string allNames() const;

    // Validate a call to an intrinsic (argument count and types)
    // Reports errors using the diagnostic engine and returns true if valid.
    bool validateCall(const IntrinsicEntry& entry,
                      size_t numValueArgs,
                      DiagnosticEngine& dc,
                      const SourceLocation& loc) const;

    // Convenience: pre‑interned IDs for the most common intrinsics
    InternedString getSizeofId() const   { return sizeofId; }
    InternedString getAlignofId() const  { return alignofId; }

private:
    IntrinsicRegistry();
    void buildMap();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, const IntrinsicEntry*> idToEntry;
    InternedString sizeofId, alignofId;

    static IntrinsicEntry kEntries[];
    static const size_t kEntryCount;
};