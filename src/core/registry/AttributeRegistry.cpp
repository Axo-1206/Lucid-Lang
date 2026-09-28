/// @file registry/AttributeRegistry.cpp
/// @brief Implementation of the attribute registry.

#include "AttributeRegistry.hpp"

#include <algorithm>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
// The attribute table
// ─────────────────────────────────────────────────────────────────────────────
//
// One entry per attribute in the language. The set is fixed: the table
// is the source of truth for "which attributes exist", and it is
// reflected in the grammar's attribute lists (§4.1.4, §4.1.5, §4.2.6).
//
// Adding an attribute to the language means:
//
//   1. Adding an entry here.
//   2. Teaching Sema to decode it (into whatever field on the declaration
//      it corresponds to).
//   3. Updating the grammar's attribute list for the relevant declaration
//      form.
//
// The order of the entries is not significant; the registry sorts names
// when asked to enumerate.

namespace {

const AttributeInfo ATTRIBUTE_TABLE[] = {
    // ─── Attributes usable on any top-level declaration ────────────────
    //
    // `@export` marks a declaration as visible outside its module. It
    // applies to tables, functions, and top-level variables. It does not
    // apply to imports (an import is a directive, not a declaration that
    // introduces a name into the exporting namespace).
    {
        "export",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
            ASTKind::FnDecl,
            ASTKind::VarDecl,
        }
    },

    // ─── Table attributes ──────────────────────────────────────────────

    // `@readonly`: no ADD, no REMOVE, no cell writes.
    {
        "readonly",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
            ASTKind::ColumnDecl,
        }
    },

    // `@immutable`: no ADD/REMOVE after initialization; cell writes
    // allowed. On a FIXED table it is redundant; Sema warns.
    {
        "immutable",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
        }
    },

    // `@packed`: contiguous storage with no slack; implies @immutable.
    {
        "packed",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
        }
    },

    // `@reserve(N)`: a storage hint. Not a policy limit.
    {
        "reserve",
        AttrArgShape::OneInteger,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
        }
    },

    // `@columnar`: store columns in separate contiguous buffers.
    {
        "columnar",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
        }
    },

    // `@request`: a host-backed table that represents a single async
    // operation, usable with waitForRequest.
    {
        "request",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::TableDecl,
        }
    },

    // ─── Column attributes ─────────────────────────────────────────────

    // `@unique`: no two rows share a value in this column.
    {
        "unique",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::ColumnDecl,
        }
    },

    // `@primary`: implies @unique; generates a by<Column> lookup.
    {
        "primary",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::ColumnDecl,
        }
    },

    // ─── Function attributes ───────────────────────────────────────────

    // `@deprecated("message")`: using the declaration produces a warning.
    {
        "deprecated",
        AttrArgShape::OneString,
        /*repeatable=*/false,
        {
            ASTKind::FnDecl,
            ASTKind::TableDecl,
            ASTKind::VarDecl,
        }
    },

    // `@on(EventKind.Member)`: registers the function as a callback for
    // the named event kind. Requires @export.
    {
        "on",
        AttrArgShape::OneDottedName,
        /*repeatable=*/false,
        {
            ASTKind::FnDecl,
        }
    },

    // `@sequence`: declares a suspension-capable function.
    {
        "sequence",
        AttrArgShape::None,
        /*repeatable=*/false,
        {
            ASTKind::FnDecl,
        }
    },
};

constexpr size_t ATTRIBUTE_COUNT =
    sizeof(ATTRIBUTE_TABLE) / sizeof(ATTRIBUTE_TABLE[0]);

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

AttributeRegistry::AttributeRegistry() {
    m_attributes.reserve(ATTRIBUTE_COUNT);

    for (size_t i = 0; i < ATTRIBUTE_COUNT; ++i) {
        const AttributeInfo& info = ATTRIBUTE_TABLE[i];
        // The key is a view into the static table, which has static
        // storage duration. The registry never owns the strings, so a
        // string_view key is safe for as long as the table exists
        // (which is the program's lifetime).
        m_attributes.emplace(info.name, &info);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lookup
// ─────────────────────────────────────────────────────────────────────────────

const AttributeInfo* AttributeRegistry::getInfo(std::string_view name) const {
    auto it = m_attributes.find(name);
    return it != m_attributes.end() ? it->second : nullptr;
}

bool AttributeRegistry::isAllowedOnDecl(std::string_view name,
                                        ASTKind declKind) const {
    const AttributeInfo* info = getInfo(name);
    if (info == nullptr) return false;

    for (ASTKind allowed : info->allowedKinds) {
        if (allowed == declKind) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enumeration
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string_view> AttributeRegistry::getAllNames() const {
    std::vector<std::string_view> names;
    names.reserve(m_attributes.size());
    for (const auto& pair : m_attributes) {
        names.push_back(pair.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}