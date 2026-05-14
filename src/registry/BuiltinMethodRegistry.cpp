#include "BuiltinMethodRegistry.hpp"

namespace {
    struct BuiltinMethodEntry {
        const char* typeKey;
        const char* methodName;
        std::vector<BuiltinArgKind> argKinds;
        BuiltinReturnKind returnKind;
        const char* description;
    };

    const BuiltinMethodEntry kEntries[] = {
        // Fixed array [N]T
        { "fixed_array", "len",      {}, BuiltinReturnKind::IntType,      "Returns number of elements" },
        { "fixed_array", "isEmpty",  {}, BuiltinReturnKind::BoolType,     "Returns true if len() == 0" },
        { "fixed_array", "first",    {}, BuiltinReturnKind::ElementType,  "Returns first element (panics if empty)" },
        { "fixed_array", "last",     {}, BuiltinReturnKind::ElementType,  "Returns last element (panics if empty)" },

        // Slice []T
        { "slice",       "len",      {}, BuiltinReturnKind::IntType,      "Returns number of elements" },
        { "slice",       "isEmpty",  {}, BuiltinReturnKind::BoolType,     "Returns true if len() == 0" },
        { "slice",       "first",    {}, BuiltinReturnKind::ElementType,  "Returns first element (panics if empty)" },
        { "slice",       "last",     {}, BuiltinReturnKind::ElementType,  "Returns last element (panics if empty)" },
        { "slice",       "cap",      {}, BuiltinReturnKind::IntType,      "Returns allocated capacity" },

        // Dynamic array [*]T
        { "dynamic_array", "len",    {}, BuiltinReturnKind::IntType,      "Returns number of elements" },
        { "dynamic_array", "isEmpty", {}, BuiltinReturnKind::BoolType,     "Returns true if len() == 0" },
        { "dynamic_array", "first",  {}, BuiltinReturnKind::ElementType,  "Returns first element (panics if empty)" },
        { "dynamic_array", "last",   {}, BuiltinReturnKind::ElementType,  "Returns last element (panics if empty)" },
        { "dynamic_array", "cap",    {}, BuiltinReturnKind::IntType,      "Returns allocated capacity" },
        { "dynamic_array", "push",   {BuiltinArgKind::ElementType}, BuiltinReturnKind::Void, "Appends element to end" },
        { "dynamic_array", "pop",    {}, BuiltinReturnKind::ElementType,  "Removes and returns last element" },
        { "dynamic_array", "insert", {BuiltinArgKind::IntegerType, BuiltinArgKind::ElementType}, BuiltinReturnKind::Void, "Inserts element at index" },
        { "dynamic_array", "remove", {BuiltinArgKind::IntegerType}, BuiltinReturnKind::ElementType, "Removes element at index" },
        { "dynamic_array", "clear",  {}, BuiltinReturnKind::Void,         "Removes all elements" },
        { "dynamic_array", "reserve", {BuiltinArgKind::IntegerType}, BuiltinReturnKind::Void, "Pre‑allocates capacity" },
    };
    const size_t kNumEntries = sizeof(kEntries) / sizeof(kEntries[0]);
} // anonymous namespace

BuiltinMethodRegistry& BuiltinMethodRegistry::instance() {
    static BuiltinMethodRegistry registry;
    return registry;
}

BuiltinMethodRegistry::BuiltinMethodRegistry() = default;

void BuiltinMethodRegistry::setStringPool(StringPool& pool) {
    if (stringPool) return; // already initialised (allow re‑init if needed)
    stringPool = &pool;

    // Pre‑intern well‑known type keys
    fixedArrayId   = pool.intern("fixed_array");
    sliceId        = pool.intern("slice");
    dynamicArrayId = pool.intern("dynamic_array");

    // Build the registry from the static table
    registry_.clear();
    for (size_t i = 0; i < kNumEntries; ++i) {
        const auto& entry = kEntries[i];
        InternedString typeKey   = pool.intern(entry.typeKey);
        InternedString methodId  = pool.intern(entry.methodName);
        BuiltinMethodInfo info;
        info.id = methodId;
        info.name = entry.methodName;
        info.argKinds = entry.argKinds;
        info.returnKind = entry.returnKind;
        info.description = entry.description;
        registry_[typeKey][methodId] = std::move(info);
    }
}

void BuiltinMethodRegistry::resetStringPool() {
    stringPool = nullptr;
    registry_.clear();
    fixedArrayId = sliceId = dynamicArrayId = InternedString();
}

const BuiltinMethodInfo* BuiltinMethodRegistry::lookup(InternedString typeKey, InternedString methodName) const {
    if (!stringPool) return nullptr;
    auto typeIt = registry_.find(typeKey);
    if (typeIt == registry_.end()) return nullptr;
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    return &methodIt->second;
}

const BuiltinMethodInfo* BuiltinMethodRegistry::lookup(const std::string& typeKey, const std::string& methodName) const {
    if (!stringPool) return nullptr;
    return lookup(stringPool->intern(typeKey), stringPool->intern(methodName));
}