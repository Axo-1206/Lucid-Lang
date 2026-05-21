/**
 * @file BuiltinMethodRegistry.cpp
 * @brief Implementation of the built‑in method registry.
 */

#include "BuiltinMethodRegistry.hpp"

namespace {

// Static table of built‑in methods.
// Note: Grammar rules:
//   - Fixed array [N]T: len, isEmpty, indexing, slicing (NO first/last/cap)
//   - Slice []T: len, isEmpty, first, last, cap, indexing, slicing
//   - Dynamic [*]T: all of slice + push, pop, insert, remove, clear, reserve
struct BuiltinMethodEntry {
    std::string_view typeKey;
    std::string_view methodName;
    std::vector<BuiltinArgKind> argKinds;
    BuiltinReturnKind returnKind;
    std::string_view description;
};

const BuiltinMethodEntry kEntries[] = {
    // Fixed array [N]T
    { "fixed_array", "len",      {}, BuiltinReturnKind::IntType,      "Returns number of elements" },
    { "fixed_array", "isEmpty",  {}, BuiltinReturnKind::BoolType,     "Returns true if len() == 0" },

    // Slice []T
    { "slice",       "len",      {}, BuiltinReturnKind::IntType,      "Returns number of elements" },
    { "slice",       "isEmpty",  {}, BuiltinReturnKind::BoolType,     "Returns true if len() == 0" },
    { "slice",       "first",    {}, BuiltinReturnKind::ElementType,  "Returns first element (panics if empty)" },
    { "slice",       "last",     {}, BuiltinReturnKind::ElementType,  "Returns last element (panics if empty)" },
    { "slice",       "cap",      {}, BuiltinReturnKind::IntType,      "Returns allocated capacity" },

    // Dynamic array [*]T (includes all slice methods)
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
    stringPool = &pool;

    // Clear existing registry (in case of re‑initialisation)
    registry_.clear();

    // Pre‑intern well‑known type keys
    fixedArrayId   = pool.intern("fixed_array");
    sliceId        = pool.intern("slice");
    dynamicArrayId = pool.intern("dynamic_array");

    // Register all built‑in methods from the static table
    for (const auto& entry : kEntries) {
        registerMethod(entry.typeKey, entry.methodName,
                       entry.argKinds, entry.returnKind,
                       entry.description);
    }
}

void BuiltinMethodRegistry::resetStringPool() {
    stringPool = nullptr;
    registry_.clear();
    fixedArrayId = sliceId = dynamicArrayId = InternedString();
}

void BuiltinMethodRegistry::registerMethod(std::string_view typeKey,
                                           std::string_view methodName,
                                           std::vector<BuiltinArgKind> argKinds,
                                           BuiltinReturnKind returnKind,
                                           std::string_view description) {
    InternedString typeId   = stringPool->intern(std::string(typeKey));
    InternedString methodId = stringPool->intern(std::string(methodName));

    BuiltinMethodInfo info;
    info.id = methodId;
    info.name = methodName;
    info.argKinds = std::move(argKinds);
    info.returnKind = returnKind;
    info.description = description;

    registry_[typeId][methodId] = std::move(info);
}

const BuiltinMethodInfo* BuiltinMethodRegistry::lookup(InternedString typeKey,
                                                       InternedString methodName) const {
    if (!stringPool) return nullptr;
    auto typeIt = registry_.find(typeKey);
    if (typeIt == registry_.end()) return nullptr;
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    return &methodIt->second;
}

const BuiltinMethodInfo* BuiltinMethodRegistry::lookup(const std::string& typeKey,
                                                       const std::string& methodName) const {
    if (!stringPool) return nullptr;
    return lookup(stringPool->intern(typeKey), stringPool->intern(methodName));
}