/**
 * @file BuiltinMethodRegistry.hpp
 * @brief Registry for built‑in methods on array types.
 *
 * Arrays (fixed, slice, dynamic) have a fixed set of methods that are
 * provided by the compiler. This registry maps type keys and method names
 * to metadata used during semantic checking and code generation.
 */

#pragma once

#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinArgKind – what kind of argument a built‑in method expects
// ─────────────────────────────────────────────────────────────────────────────
enum class BuiltinArgKind {
    ElementType,    ///< Argument must match the array's element type
    IntegerType,    ///< Argument must be an integer (e.g., index or capacity)
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinReturnKind – what kind of value a built‑in method returns
// ─────────────────────────────────────────────────────────────────────────────
enum class BuiltinReturnKind {
    Void,           ///< Returns nothing
    IntType,        ///< Returns an integer (e.g., length, capacity)
    BoolType,       ///< Returns a boolean (e.g., isEmpty)
    ElementType,    ///< Returns the array's element type (e.g., first, last, pop)
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinMethodInfo – metadata about one built‑in array method
// ─────────────────────────────────────────────────────────────────────────────
struct BuiltinMethodInfo {
    InternedString id;                       ///< Interned method name (key)
    std::string_view name;                   ///< Original name (for debugging)
    std::vector<BuiltinArgKind> argKinds;    ///< Types of arguments (in order)
    BuiltinReturnKind returnKind;            ///< Type of return value
    std::string_view description;            ///< Human‑readable description
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinMethodRegistry – singleton providing built‑in array method metadata
// ─────────────────────────────────────────────────────────────────────────────
class BuiltinMethodRegistry {
public:
    static BuiltinMethodRegistry& instance();

    // Must be called once before any lookups (usually after creating StringPool)
    void setStringPool(StringPool& pool);
    
    // Call before destroying the StringPool to avoid dangling references
    void resetStringPool();

    // Lookup by interned type key and interned method name (fast)
    const BuiltinMethodInfo* lookup(InternedString typeKey, InternedString methodName) const;

    // Convenience – interns the strings on the fly
    const BuiltinMethodInfo* lookup(const std::string& typeKey, const std::string& methodName) const;

    // Pre‑interned well‑known type keys (zero‑cost after setStringPool)
    InternedString getFixedArrayKey()   const { return fixedArrayId; }
    InternedString getSliceKey()        const { return sliceId; }
    InternedString getDynamicArrayKey() const { return dynamicArrayId; }

private:
    BuiltinMethodRegistry();

    // Helper to register a single built‑in method from a static entry
    void registerMethod(std::string_view typeKey,
                        std::string_view methodName,
                        std::vector<BuiltinArgKind> argKinds,
                        BuiltinReturnKind returnKind,
                        std::string_view description);

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, std::unordered_map<InternedString, BuiltinMethodInfo>> registry_;
    
    // Pre‑interned IDs for well‑known type keys
    InternedString fixedArrayId, sliceId, dynamicArrayId;
};