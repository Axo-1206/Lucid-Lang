/**
 * @file QualifierRegistry.cpp
 * @brief Implementation of the qualifier registry namespace.
 */

#include "QualifierRegistry.hpp"
#include "diagnostics/Diagnostic.hpp"
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// kQualifiers – static table of all built‑in qualifiers
// ─────────────────────────────────────────────────────────────────────────────
static QualifierEntry kQualifiers[] = {
    {
        InternedString(),               // id (filled later)
        "async",
        QualifierBits::Async,
        true,                           // affectsTypeEquality
        QualifierContext::Function | QualifierContext::Parameter |
        QualifierContext::Return | QualifierContext::TypeAlias,
        "Marks a function as asynchronous – call site must use `await`"
    },
    {
        InternedString(),
        "nullable",
        QualifierBits::Nullable,
        true,
        QualifierContext::Function | QualifierContext::Variable |
        QualifierContext::Parameter | QualifierContext::Return |
        QualifierContext::TypeAlias,
        "Marks a function binding as nullable – call site must guard against nil"
    },
    {
        InternedString(),
        "parallel",
        QualifierBits::Parallel,
        false,                          // does NOT affect type equality
        QualifierContext::Function | QualifierContext::Parameter,
        "Marks a function for parallel execution – body has restrictions (no return, no writes to outer scope)"
    },
};

static const size_t kQualifierCount = sizeof(kQualifiers) / sizeof(kQualifiers[0]);

// ─────────────────────────────────────────────────────────────────────────────
// Static state
// ─────────────────────────────────────────────────────────────────────────────
static StringPool* stringPool = nullptr;
static std::unordered_map<InternedString, const QualifierEntry*> idToEntry;
static uint32_t cachedEqualityMask = 0;

// Pre‑interned IDs for well‑known qualifiers
static InternedString asyncId;
static InternedString nullableId;
static InternedString parallelId;

// ─────────────────────────────────────────────────────────────────────────────
// Implementation of namespace functions
// ─────────────────────────────────────────────────────────────────────────────
namespace qualifier {

void initialize(StringPool& pool) {
    stringPool = &pool;
    idToEntry.clear();

    for (size_t i = 0; i < kQualifierCount; ++i) {
        auto& entry = kQualifiers[i];
        entry.id = pool.intern(std::string(entry.name));
        idToEntry[entry.id] = &entry;
    }

    // Pre‑intern well‑known IDs
    asyncId    = pool.intern("async");
    nullableId = pool.intern("nullable");
    parallelId = pool.intern("parallel");

    // Pre‑compute equality mask
    cachedEqualityMask = 0;
    for (size_t i = 0; i < kQualifierCount; ++i) {
        if (kQualifiers[i].affectsTypeEquality) {
            cachedEqualityMask |= kQualifiers[i].bit;
        }
    }
}

void shutdown() {
    stringPool = nullptr;
    idToEntry.clear();
    asyncId = nullableId = parallelId = InternedString();
    cachedEqualityMask = 0;
}

const QualifierEntry* lookup(InternedString id) {
    if (!stringPool) return nullptr;
    auto it = idToEntry.find(id);
    return (it != idToEntry.end()) ? it->second : nullptr;
}

const QualifierEntry* lookup(std::string_view name) {
    if (!stringPool) return nullptr;
    return lookup(stringPool->intern(std::string(name)));
}

InternedString getId(std::string_view name) {
    const QualifierEntry* entry = lookup(name);
    return entry ? entry->id : InternedString();
}

bool isKnown(InternedString id) {
    return lookup(id) != nullptr;
}

bool isKnown(std::string_view name) {
    return lookup(name) != nullptr;
}

uint32_t getBit(InternedString id) {
    const QualifierEntry* entry = lookup(id);
    return entry ? entry->bit : 0;
}

uint32_t getBit(std::string_view name) {
    const QualifierEntry* entry = lookup(name);
    return entry ? entry->bit : 0;
}

bool applyQualifier(uint32_t& mask, InternedString id) {
    uint32_t bit = getBit(id);
    if (!bit) return false;
    mask |= bit;
    return true;
}

bool applyQualifier(uint32_t& mask, std::string_view name) {
    uint32_t bit = getBit(name);
    if (!bit) return false;
    mask |= bit;
    return true;
}

bool validateUsage(const QualifierEntry& entry,
                   QualifierContext ctx,
                   InternedString file,
                   const SourceLocation& loc) {
    if (!hasFlag(entry.validContexts, ctx)) {
        diagnostic::error(DiagnosticCategory::Syntax, file, loc,
                         DiagCode::E2015,
                         {std::string("~") + std::string(entry.name)});
        return false;
    }
    return true;
}

std::string allNames() {
    std::string result;
    for (size_t i = 0; i < kQualifierCount; ++i) {
        if (i) result += ", ";
        result += "~";
        result += kQualifiers[i].name;
    }
    return result;
}

uint32_t equalityMask() {
    return cachedEqualityMask;
}

InternedString getAsyncId()    { return asyncId; }
InternedString getNullableId() { return nullableId; }
InternedString getParallelId() { return parallelId; }

} // namespace qualifier