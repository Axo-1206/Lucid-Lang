#pragma once

#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <string>
#include <unordered_map>
#include <cstdint>

namespace QualifierBits {
    constexpr uint32_t Async    = 1 << 0;   // ~async
    constexpr uint32_t Nullable = 1 << 1;   // ~nullable
    constexpr uint32_t Parallel = 1 << 2;   // ~parallel
    // Bits 3–31 reserved for future qualifiers
}

struct QualifierInfo {
    InternedString id;               // interned key (filled by setStringPool)
    std::string_view name;           // human‑readable name (view into pool)
    uint32_t bit;                    // bitmask value
    bool affectsTypeEquality;        // does this qualifier affect type identity?
    bool validOnFunction;
    bool validOnVariable;
    bool validOnStruct;
};

class QualifierRegistry {
public:
    static QualifierRegistry& instance();

    void setStringPool(StringPool& pool);
    void resetStringPool();

    const QualifierInfo* lookup(InternedString id) const;
    const QualifierInfo* lookup(const std::string& name) const;

    uint32_t getBit(InternedString id) const;
    uint32_t getBit(const std::string& name) const;

    bool isValid(InternedString id) const;
    bool isValid(const std::string& name) const;

    bool applyQualifier(uint32_t& mask, InternedString id) const;
    bool applyQualifier(uint32_t& mask, const std::string& name) const;

    std::string allNames() const;
    uint32_t equalityMask() const { return cachedEqualityMask; }

    // Convenience accessors for commonly used qualifiers (parser/codegen)
    InternedString getAsyncId()    const { return asyncId; }
    InternedString getNullableId() const { return nullableId; }
    InternedString getParallelId() const { return parallelId; }

private:
    QualifierRegistry();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, QualifierInfo> byId;
    std::unordered_map<std::string, InternedString> nameToId;

    // Pre‑interned IDs for well‑known qualifiers
    InternedString asyncId, nullableId, parallelId;
    uint32_t cachedEqualityMask = 0;
};