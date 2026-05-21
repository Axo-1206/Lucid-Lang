/**
 * @file QualifierRegistry.cpp
 * @brief Implementation of the QualifierRegistry singleton.
 */

#include "QualifierRegistry.hpp"
#include "diagnostics/DiagnosticEngine.hpp"

#include <sstream>

namespace {

struct BuiltinQualifier {
    std::string_view name;
    uint32_t bit;
    bool affectsTypeEquality;
    QualifierContext validContexts;
    std::string_view description;
};

const BuiltinQualifier kBuiltinQualifiers[] = {
    {
        "async",
        QualifierBits::Async,
        true,
        QualifierContext::Function | QualifierContext::Parameter |
        QualifierContext::Return | QualifierContext::TypeAlias,
        "Marks a function as asynchronous – call site must use `await`"
    },
    {
        "nullable",
        QualifierBits::Nullable,
        true,
        QualifierContext::Function | QualifierContext::Variable |
        QualifierContext::Parameter | QualifierContext::Return |
        QualifierContext::TypeAlias,
        "Marks a function binding as nullable – call site must guard against nil"
    },
    {
        "parallel",
        QualifierBits::Parallel,
        false,
        QualifierContext::Function | QualifierContext::Parameter,
        "Marks a function for parallel execution – body has restrictions (no return, no writes to outer scope)"
    },
};

const size_t kNumBuiltinQualifiers = sizeof(kBuiltinQualifiers) / sizeof(kBuiltinQualifiers[0]);

} // namespace

QualifierRegistry& QualifierRegistry::instance() {
    static QualifierRegistry registry;
    return registry;
}

QualifierRegistry::QualifierRegistry() = default;

void QualifierRegistry::setStringPool(StringPool& pool) {
    stringPool = &pool;

    // Clear existing maps (to support re‑initialisation)
    byId.clear();
    nameToId.clear();

    for (const auto& builtin : kBuiltinQualifiers) {
        InternedString id = pool.intern(std::string(builtin.name));
        std::string_view nameView = pool.lookup(id);

        QualifierInfo info;
        info.id = id;
        info.name = nameView;
        info.bit = builtin.bit;
        info.affectsTypeEquality = builtin.affectsTypeEquality;
        info.validContexts = builtin.validContexts;
        info.description = builtin.description;

        byId[id] = info;
        nameToId[std::string(builtin.name)] = id;
    }

    // Pre‑intern well‑known IDs for fast access
    asyncId    = pool.intern("async");
    nullableId = pool.intern("nullable");
    parallelId = pool.intern("parallel");

    // Pre‑compute equality mask
    cachedEqualityMask = 0;
    for (const auto& [_, info] : byId) {
        if (info.affectsTypeEquality) {
            cachedEqualityMask |= info.bit;
        }
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

const QualifierInfo* QualifierRegistry::lookup(std::string_view name) const {
    if (!stringPool) return nullptr;
    auto it = nameToId.find(std::string(name));
    if (it == nameToId.end()) return nullptr;
    return lookup(it->second);
}

uint32_t QualifierRegistry::getBit(InternedString id) const {
    const QualifierInfo* info = lookup(id);
    return info ? info->bit : 0;
}

uint32_t QualifierRegistry::getBit(std::string_view name) const {
    const QualifierInfo* info = lookup(name);
    return info ? info->bit : 0;
}

bool QualifierRegistry::isValid(InternedString id) const {
    return lookup(id) != nullptr;
}

bool QualifierRegistry::isValid(std::string_view name) const {
    return lookup(name) != nullptr;
}

bool QualifierRegistry::applyQualifier(uint32_t& mask, InternedString id) const {
    uint32_t bit = getBit(id);
    if (!bit) return false;
    mask |= bit;
    return true;
}

bool QualifierRegistry::applyQualifier(uint32_t& mask, std::string_view name) const {
    uint32_t bit = getBit(name);
    if (!bit) return false;
    mask |= bit;
    return true;
}

bool QualifierRegistry::validateUsage(const QualifierInfo& info,
                                      QualifierContext ctx,
                                      DiagnosticEngine& dc,
                                      const SourceLocation& loc) const {
    if (!hasFlag(info.validContexts, ctx)) {
        // Use diagnostic code E2015 for invalid qualifier context
        dc.error(DiagnosticCategory::Syntax, InternedString(), loc,
                 DiagCode::E2015, {std::string("~") + std::string(info.name)});
        return false;
    }
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