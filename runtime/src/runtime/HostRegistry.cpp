/**
 * @file runtime/HostRegistry.cpp
 *
 * @responsibility The host registry implementation. A registry is a
 *                 pair of name-to-entry maps: functions and types.
 *                 A scoped view is a registry whose maps delegate to
 *                 the parent, filtered by the visible-name set.
 *
 * ─── Design: entries are stable ───────────────────────────────────────────
 * A registry's entries live in std::deque-like storage so that
 * pointers to them (returned by findFunction / findType) remain valid
 * across subsequent registrations. std::vector would invalidate them
 * on reallocation; we use std::vector<std::unique_ptr<Entry>> and
 * stable indices instead, with the pointers being entry addresses
 * that never move.
 *
 * ─── Design: scoped views share the parent's storage ──────────────────────
 * A scoped view does not copy entries. It holds a pointer to the
 * parent and a set of visible names. findFunction / findType check
 * the visible set first; if the name is not visible, the lookup fails
 * even if the parent has it.
 *
 * ─── Design: type indices are assigned by the parent ──────────────────────
 * A type's `typeIndex` is a small integer unique within the registry
 * it was registered in. A scoped view does not reassign indices; a
 * handle created against a view uses the parent's index. This is what
 * makes a handle shareable across views of the same parent.
 */

#include "runtime/HostRegistry.hpp"

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lucid::runtime {

// ─────────────────────────────────────────────────────────────────────────
// Impl
// ─────────────────────────────────────────────────────────────────────────

struct HostRegistry::Impl {
    // ─── Entries ────────────────────────────────────────────────────────
    //
    // Stored in deques so that references to entries are stable across
    // insertion. A deque never invalidates references on push_back.
    std::deque<FunctionEntry> functions;
    std::deque<TypeEntry>     types;

    // Name → index into the deques above.
    std::unordered_map<std::string, uint32_t> functionByName;
    std::unordered_map<std::string, uint32_t> typeByName;

    // ─── Scoped view ────────────────────────────────────────────────────
    //
    // A view has a parent pointer and a visible-name filter. The
    // filter is a set of names; a lookup that is not in the set fails
    // even if the parent has it.
    const Impl* parent = nullptr;
    bool        hasFilter = false;
    std::unordered_set<std::string> visibleNames;

    // ─── Helpers ────────────────────────────────────────────────────────

    /// Look up a name in this registry, following the parent chain.
    /// Returns the entry's index, or UINT32_MAX if not found or not
    /// visible.
    uint32_t findFunctionIndex(std::string_view name) const noexcept {
        if (hasFilter) {
            if (!visibleNames.count(std::string(name))) return UINT32_MAX;
        }
        auto it = functionByName.find(std::string(name));
        if (it != functionByName.end()) return it->second;
        if (parent) return parent->findFunctionIndex(name);
        return UINT32_MAX;
    }

    uint32_t findTypeIndex(std::string_view name) const noexcept {
        if (hasFilter) {
            if (!visibleNames.count(std::string(name))) return UINT32_MAX;
        }
        auto it = typeByName.find(std::string(name));
        if (it != typeByName.end()) return it->second;
        if (parent) return parent->findTypeIndex(name);
        return UINT32_MAX;
    }

    /// Resolve a type by its global (parent-assigned) index, walking
    /// up the parent chain until we find the deque that owns it.
    const TypeEntry* resolveTypeByIndex(uint32_t index) const noexcept {
        if (index < types.size()) return &types[index];
        if (parent) return parent->resolveTypeByIndex(index);
        return nullptr;
    }

    /// The same for functions.
    const FunctionEntry* resolveFunctionByIndex(uint32_t index) const noexcept {
        if (index < functions.size()) return &functions[index];
        if (parent) return parent->resolveFunctionByIndex(index);
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────
// HostRegistry
// ─────────────────────────────────────────────────────────────────────────

HostRegistry::HostRegistry()
    : m_impl(std::make_unique<Impl>()) {}

HostRegistry::~HostRegistry() = default;

HostRegistry::HostRegistry(HostRegistry&&) noexcept = default;
HostRegistry& HostRegistry::operator=(HostRegistry&&) noexcept = default;

bool HostRegistry::registerFunction(std::string name,
                                    HostFunctionPtr fn,
                                    contract::FunctionSignature signature) {
    if (m_impl->functionByName.count(name)) return false;

    const uint32_t index = static_cast<uint32_t>(m_impl->functions.size());
    m_impl->functions.push_back(FunctionEntry{
        std::string_view{},  // filled below
        fn,
        nullptr              // filled below
    });
    FunctionEntry& e = m_impl->functions.back();

    // The name and signature must outlive the entry. We store them in
    // the impl-owned name/signature pools.
    //
    // To keep the header simple, we allocate the string and the
    // signature on the heap and stash them via a small side table.
    // (A future revision can move these into the impl directly.)
    //
    // For now: the name is copied into a stable string stored in the
    // functionByName key. This is the standard "the map owns the key,
    // the entry names it" pattern; the entry's view is over the key.
    auto [it, inserted] = m_impl->functionByName.emplace(
        std::move(name), index);
    e.name = it->first;  // view over the map's key

    // The signature is owned by a heap allocation; leaked on registry
    // destruction for simplicity in v1. (A future revision uses a
    // deque<FunctionSignature> and stores a pointer into it.)
    auto* sig = new contract::FunctionSignature(std::move(signature));
    e.signature = sig;

    return true;
}

bool HostRegistry::registerType(std::string name,
                                HostRetainFn retain,
                                HostReleaseFn release,
                                HostEqualsFn equals,
                                HostHashFn hash) {
    if (m_impl->typeByName.count(name)) return false;

    const uint32_t index = static_cast<uint32_t>(m_impl->types.size());
    m_impl->types.push_back(TypeEntry{
        std::string_view{},  // filled below
        index,
        retain,
        release,
        equals,
        hash
    });
    TypeEntry& e = m_impl->types.back();

    auto [it, inserted] = m_impl->typeByName.emplace(std::move(name), index);
    e.name = it->first;
    return true;
}

std::unique_ptr<HostRegistry> HostRegistry::scopedView(
    const std::vector<std::string>& visibleNames) const {
    auto view = std::make_unique<HostRegistry>();
    view->m_impl->parent = m_impl.get();
    view->m_impl->hasFilter = true;
    for (const auto& n : visibleNames) {
        view->m_impl->visibleNames.insert(n);
    }
    return view;
}

const HostRegistry::FunctionEntry*
HostRegistry::findFunction(std::string_view name) const noexcept {
    const uint32_t index = m_impl->findFunctionIndex(name);
    if (index == UINT32_MAX) return nullptr;
    return m_impl->resolveFunctionByIndex(index);
}

const HostRegistry::TypeEntry*
HostRegistry::findType(std::string_view name) const noexcept {
    const uint32_t index = m_impl->findTypeIndex(name);
    if (index == UINT32_MAX) return nullptr;
    return m_impl->resolveTypeByIndex(index);
}

const HostRegistry::TypeEntry*
HostRegistry::typeAt(uint32_t typeIndex) const noexcept {
    return m_impl->resolveTypeByIndex(typeIndex);
}

uint32_t HostRegistry::functionCount() const noexcept {
    return static_cast<uint32_t>(m_impl->functions.size());
}

uint32_t HostRegistry::typeCount() const noexcept {
    return static_cast<uint32_t>(m_impl->types.size());
}

} // namespace lucid::runtime