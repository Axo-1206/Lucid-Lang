#pragma once

#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <string>
#include <unordered_map>
#include <vector>

enum class BuiltinArgKind {
    ElementType,    // Argument must match the array's element type
    IntegerType,    // Argument must be an integer (e.g., for capacity or index)
};

enum class BuiltinReturnKind {
    Void,           // Returns nothing
    IntType,        // Returns an integer (e.g., length, capacity)
    BoolType,       // Returns a boolean (e.g., isEmpty)
    ElementType,    // Returns the array's element type (e.g., first, last, pop)
};

struct BuiltinMethodInfo {
    InternedString id;               // interned method name
    const char* name;                // original string (for debugging)
    std::vector<BuiltinArgKind> argKinds;
    BuiltinReturnKind returnKind;
    const char* description;
};

class BuiltinMethodRegistry {
public:
    static BuiltinMethodRegistry& instance();

    // Must be called once before any lookups
    void setStringPool(StringPool& pool);
    void resetStringPool();

    // Lookup by interned type key and interned method name
    const BuiltinMethodInfo* lookup(InternedString typeKey, InternedString methodName) const;

    // Convenience – interns the strings on the fly
    const BuiltinMethodInfo* lookup(const std::string& typeKey, const std::string& methodName) const;

    // Pre‑interned well‑known type keys
    InternedString getFixedArrayKey()   const { return fixedArrayId; }
    InternedString getSliceKey()        const { return sliceId; }
    InternedString getDynamicArrayKey() const { return dynamicArrayId; }

private:
    BuiltinMethodRegistry();
    void registerBuiltins();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, std::unordered_map<InternedString, BuiltinMethodInfo>> registry_;
    // Pre‑interned IDs for well‑known type keys
    InternedString fixedArrayId, sliceId, dynamicArrayId;
};