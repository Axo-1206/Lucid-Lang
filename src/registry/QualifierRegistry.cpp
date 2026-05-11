#include "QualifierRegistry.hpp"
#include "diagnostics/DiagnosticEngine.hpp" // for assert-like logging (optional)
#include <cassert>
#include <sstream>

namespace {
    struct BuiltinQualifier {
        std::string_view name;
        uint32_t bit;
        bool affectsTypeEquality;
        bool validOnFunction;
        bool validOnVariable;
        bool validOnStruct;
    };

    const BuiltinQualifier kBuiltinQualifiers[] = {
        { "async",    QualifierBits::Async,    true,  true,  false, false },
        { "nullable", QualifierBits::Nullable, true,  true,  false, false },
        { "parallel", QualifierBits::Parallel, true,  true,  false, false },
    };
    const size_t kNumBuiltinQualifiers = sizeof(kBuiltinQualifiers) / sizeof(kBuiltinQualifiers[0]);
}

QualifierRegistry& QualifierRegistry::instance() {
    static QualifierRegistry registry;
    return registry;
}

QualifierRegistry::QualifierRegistry() = default;

void QualifierRegistry::setStringPool(StringPool& pool) {
    if (stringPool) return; // already initialised (but we may still rebuild? Keep guard for now)
    stringPool = &pool;

    byId.clear();
    nameToId.clear();

    for (const auto& builtin : kBuiltinQualifiers) {
        std::string nameStr(builtin.name);
        InternedString id = pool.intern(nameStr);
        std::string_view nameView = pool.lookup(id);

        QualifierInfo info;
        info.id = id;
        info.name = nameView;
        info.bit = builtin.bit;
        info.affectsTypeEquality = builtin.affectsTypeEquality;
        info.validOnFunction = builtin.validOnFunction;
        info.validOnVariable = builtin.validOnVariable;
        info.validOnStruct = builtin.validOnStruct;

        byId[id] = info;
        nameToId[nameStr] = id;
    }

    // Pre‑intern well‑known IDs for fast access
    asyncId    = pool.intern("async");
    nullableId = pool.intern("nullable");
    parallelId = pool.intern("parallel");

    // Compute equality mask once
    cachedEqualityMask = 0;
    for (const auto& [_, info] : byId) {
        if (info.affectsTypeEquality)
            cachedEqualityMask |= info.bit;
    }
}

void QualifierRegistry::resetStringPool() {
    stringPool = nullptr;
    byId.clear();
    nameToId.clear();
    asyncId = InternedString();
    nullableId = InternedString();
    parallelId = InternedString();
    cachedEqualityMask = 0;
}

const QualifierInfo* QualifierRegistry::lookup(InternedString id) const {
    if (!stringPool) return nullptr;
    auto it = byId.find(id);
    return (it != byId.end()) ? &it->second : nullptr;
}

const QualifierInfo* QualifierRegistry::lookup(const std::string& name) const {
    if (!stringPool) return nullptr;
    auto it = nameToId.find(name);
    if (it == nameToId.end()) return nullptr;
    return lookup(it->second);
}

uint32_t QualifierRegistry::getBit(InternedString id) const {
    const QualifierInfo* info = lookup(id);
    return info ? info->bit : 0;
}

uint32_t QualifierRegistry::getBit(const std::string& name) const {
    const QualifierInfo* info = lookup(name);
    return info ? info->bit : 0;
}

bool QualifierRegistry::isValid(InternedString id) const {
    return lookup(id) != nullptr;
}

bool QualifierRegistry::isValid(const std::string& name) const {
    return lookup(name) != nullptr;
}

bool QualifierRegistry::applyQualifier(uint32_t& mask, InternedString id) const {
    uint32_t bit = getBit(id);
    if (!bit) return false;
    mask |= bit;
    return true;
}

bool QualifierRegistry::applyQualifier(uint32_t& mask, const std::string& name) const {
    uint32_t bit = getBit(name);
    if (!bit) return false;
    mask |= bit;
    return true;
}

std::string QualifierRegistry::allNames() const {
    std::stringstream ss;
    bool first = true;
    for (const auto& [name, _] : nameToId) {
        if (!first) ss << ", ";
        ss << "~" << name;
        first = false;
    }
    return ss.str();
}