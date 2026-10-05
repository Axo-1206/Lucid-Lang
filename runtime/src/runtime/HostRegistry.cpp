/**
 * @file runtime/HostRegistry.cpp
 *
 * @responsibility The host registry implementation. A registry is a
 *                 pair of name-to-index maps (functions and types)
 *                 plus stable entry storage. A scoped view is a
 *                 registry whose lookups delegate to the parent,
 *                 filtered by the view's visible-name set.
 *
 * ─── Design: entries are stable ───────────────────────────────────────────
 * A registry's entries live in std::deque so that references to them
 * (returned by findFunction / findType) remain valid across subsequent
 * registrations. std::vector would invalidate them on reallocation.
 *
 * ─── Design: names and signatures are owned by the impl ───────────────────
 * The name stored in a FunctionEntry / TypeEntry is a string_view over
 * a heap-allocated string the impl owns. The signature pointer in a
 * FunctionEntry points into a deque of signatures the impl owns. Both
 * stay stable for the registry's lifetime.
 *
 * ─── Design: scoped views share the parent's storage ──────────────────────
 * A scoped view does not copy entries. It holds a pointer to the
 * parent and a set of visible names. findFunction / findType check
 * the visible set first; if the name is not visible, the lookup fails
 * even if the parent has it. typeAt / findFunction recurse into the
 * parent for indices or names the view itself does not own.
 *
 * ─── Design: type indices are assigned by the registry that owns the
 *                 type ─────────────────────────────────────────────────────
 * A type's `typeIndex` is a small integer unique within the registry
 * it was registered in (i.e. the registry whose `types` deque holds
 * it). A scoped view does not reassign indices; a handle created
 * against a view uses the parent's index. This is what makes a handle
 * shareable across views of the same parent.
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
    // ─── Entry storage ──────────────────────────────────────────────────
    //
    // Deques, not vectors: pushing a new entry must not invalidate the
    // addresses of existing entries, because callers hold pointers to
    // them.
    std::deque<FunctionEntry> functions;
    std::deque<TypeEntry>     types;

    // The names and signatures the entries point at.
    std::deque<std::string>                functionNames;
    std::deque<contract::FunctionSignature> functionSignatures;
    std::deque<std::string>                typeNames;

    // Name → index into the deques above.
    std::unordered_map<std::string, uint32_t> functionByName;
    std::unordered_map<std::string, uint32_t> typeByName;

    // ─── Scoped view ────────────────────────────────────────────────────
    const Impl* parent = nullptr;
    bool        hasFilter = false;
    std::unordered_set<std::string> visibleNames;

    // ─── Lookup helpers ─────────────────────────────────────────────────

    /// The index of a function name in *this* registry or its parents,
    /// or UINT32_MAX if not found or not visible to this view.
    uint32_t findFunctionIndex(std::string_view name) const noexcept {
        if (hasFilter && !visibleNames.count(std::string(name))) {
            return UINT32_MAX;
        }
        auto it = functionByName.find(std::string(name));
        if (it != functionByName.end()) return it->second;
        if (parent) return parent->findFunctionIndex(name);
        return UINT32_MAX;
    }

    uint32_t findTypeIndex(std::string_view name) const noexcept {
        if (hasFilter && !visibleNames.count(std::string(name))) {
            return UINT32_MAX;
        }
        auto it = typeByName.find(std::string(name));
        if (it != typeByName.end()) return it->second;
        if (parent) return parent->findTypeIndex(name);
        return UINT32_MAX;
    }

    /// Resolve a type index to a TypeEntry, walking up the parent
    /// chain until we find the deque that owns it.
    const TypeEntry* resolveTypeByIndex(uint32_t index) const noexcept {
        if (index < types.size()) return &types[index];
        if (parent) return parent->resolveTypeByIndex(index);
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────

HostRegistry::HostRegistry()
    : m_impl(std::make_unique<Impl>()) {}

HostRegistry::~HostRegistry() = default;

HostRegistry::HostRegistry(HostRegistry&&) noexcept = default;
HostRegistry& HostRegistry::operator=(HostRegistry&&) noexcept = default;

// ─────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────

bool HostRegistry::registerFunction(std::string name,
                                    HostFunctionPtr fn,
                                    contract::FunctionSignature signature) {
    if (m_impl->functionByName.count(name)) return false;

    const uint32_t index = static_cast<uint32_t>(m_impl->functions.size());

    m_impl->functionNames.push_back(std::move(name));
    m_impl->functionSignatures.push_back(std::move(signature));

    m_impl->functions.push_back(FunctionEntry{
        m_impl->functionNames.back(),
        fn,
        &m_impl->functionSignatures.back()
    });

    m_impl->functionByName.emplace(m_impl->functionNames.back(), index);
    return true;
}

bool HostRegistry::registerType(std::string name,
                                HostRetainFn retain,
                                HostReleaseFn release,
                                HostEqualsFn equals,
                                HostHashFn hash) {
    if (m_impl->typeByName.count(name)) return false;

    const uint32_t index = static_cast<uint32_t>(m_impl->types.size());

    m_impl->typeNames.push_back(std::move(name));
    m_impl->types.push_back(TypeEntry{
        m_impl->typeNames.back(),
        index,
        retain,
        release,
        equals,
        hash
    });

    m_impl->typeByName.emplace(m_impl->typeNames.back(), index);
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

// ─────────────────────────────────────────────────────────────────────────
// Lookup
// ─────────────────────────────────────────────────────────────────────────

const HostRegistry::FunctionEntry*
HostRegistry::findFunction(std::string_view name) const noexcept {
    const uint32_t index = m_impl->findFunctionIndex(name);
    if (index == UINT32_MAX) return nullptr;

    // Walk to the registry that owns this index. The view's own
    // function deque is at *this; the parent's is at parent. Since
    // indices are assigned by the owning registry, and a view has no
    // own registrations, the owner is reached by walking up.
    if (index < m_impl->functions.size()) return &m_impl->functions[index];
    if (m_impl->parent) {
        // Rebuild a lookup against the parent's storage.
        for (const Impl* p = m_impl->parent; p; p = p->parent) {
            if (index < p->functions.size()) return &p->functions[index];
        }
    }
    return nullptr;
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

// ─────────────────────────────────────────────────────────────────────────
// Introspection
// ─────────────────────────────────────────────────────────────────────────

uint32_t HostRegistry::functionCount() const noexcept {
    return static_cast<uint32_t>(m_impl->functions.size());
}

uint32_t HostRegistry::typeCount() const noexcept {
    return static_cast<uint32_t>(m_impl->types.size());
}

} // namespace lucid::runtime