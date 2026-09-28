/// @file registry/BuiltinMethodRegistry.cpp
/// @brief Implementation of the built-in method registry.

#include "BuiltinMethodRegistry.hpp"

#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// The method table
// ─────────────────────────────────────────────────────────────────────────────
//
// One entry per built-in method name. The set is fixed; the table is
// the source of truth for "which built-in method names exist". It is
// reflected in the grammar's tables (§7.1, §7.4, §8).
//
// Adding a method to the language means:
//
//   1. Adding an entry here.
//   2. Teaching Sema to typecheck and lower it.
//   3. Updating the grammar's tables.
//
// The order of the entries is not significant; the registry sorts names
// when asked to enumerate.

namespace {

const BuiltinMethodInfo METHOD_TABLE[] = {
    // ─── Methods shared by tables and arrays ───────────────────────────

    // `ADD`: a table adds a row (one argument per column); an array
    // appends an element. The arg shape is `AddValue`, whose exact
    // behavior Sema derives from the receiver kind.
    {
        "ADD",
        { ReceiverKind::TableSheet, ReceiverKind::DynamicArray },
        MethodArgShape::AddValue,
        MethodResultShape::RowRef,
    },

    // `REMOVE`: removes the row/element at the given index.
    // `[N, T]` (fixed-size) does not have `REMOVE`, because its length
    // is fixed.
    {
        "REMOVE",
        { ReceiverKind::TableSheet, ReceiverKind::DynamicArray },
        MethodArgShape::OneIndex,
        MethodResultShape::Unit,
    },

    // `CLEAR`: removes every row/element and keeps the capacity.
    // Not on `[N, T]`.
    {
        "CLEAR",
        { ReceiverKind::TableSheet, ReceiverKind::DynamicArray },
        MethodArgShape::None,
        MethodResultShape::Unit,
    },

    // `COUNT`: number of live rows / elements. Available on every
    // receiver that has a count.
    {
        "COUNT",
        { ReceiverKind::TableSheet,
          ReceiverKind::DynamicArray,
          ReceiverKind::FixedArray },
        MethodArgShape::None,
        MethodResultShape::Primitive,
    },

    // ─── Table-only methods ────────────────────────────────────────────

    // `SHRINK`: best-effort release of unused trailing storage.
    {
        "SHRINK",
        { ReceiverKind::TableSheet },
        MethodArgShape::None,
        MethodResultShape::Unit,
    },

    // `AT`: row at slot i; returns nil on out-of-bounds/dead slot.
    // The uppercase `AT` distinguishes it from lowercase `at` in
    // prose; the source is `T.AT(i)`.
    {
        "AT",
        { ReceiverKind::TableSheet },
        MethodArgShape::OneIndex,
        MethodResultShape::RowRef,
    },

    // `VERSION`: structural version counter. Available on every table,
    // including FIXED (always 0).
    {
        "VERSION",
        { ReceiverKind::TableSheet },
        MethodArgShape::None,
        MethodResultShape::Primitive,
    },

    // `FIND`: a live view of rows matching a predicate.
    {
        "FIND",
        { ReceiverKind::TableSheet },
        MethodArgShape::OnePredicate,
        MethodResultShape::View,
    },

    // ─── Column-view methods ───────────────────────────────────────────

    // `SUM`, `AVG`: aggregations over a column view. Result type is
    // derived from the element type.
    {
        "SUM",
        { ReceiverKind::ColumnView },
        MethodArgShape::None,
        MethodResultShape::Primitive,
    },
    {
        "AVG",
        { ReceiverKind::ColumnView },
        MethodArgShape::None,
        MethodResultShape::Primitive,
    },

    // `TOARRAY`: converts a column view to a real `[T]` copy.
    {
        "TOARRAY",
        { ReceiverKind::ColumnView },
        MethodArgShape::None,
        MethodResultShape::ArrayCopy,
    },

    // ─── Array-only methods ────────────────────────────────────────────

    // `CONTAINS`: linear scan for an element. Available on both
    // dynamic and fixed-size arrays.
    {
        "CONTAINS",
        { ReceiverKind::DynamicArray, ReceiverKind::FixedArray },
        MethodArgShape::OneElement,
        MethodResultShape::Bool,
    },

    // `SORT`: reorders in place. `SORT()` uses the element's natural
    // order; `SORT(less)` takes a comparator. Available on both
    // dynamic and fixed-size arrays.
    {
        "SORT",
        { ReceiverKind::DynamicArray, ReceiverKind::FixedArray },
        MethodArgShape::ZeroOrOneComparator,
        MethodResultShape::Unit,
    },
};

constexpr size_t METHOD_COUNT =
    sizeof(METHOD_TABLE) / sizeof(METHOD_TABLE[0]);

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

BuiltinMethodRegistry::BuiltinMethodRegistry() {
    m_methods.reserve(METHOD_COUNT);

    for (size_t i = 0; i < METHOD_COUNT; ++i) {
        const BuiltinMethodInfo& info = METHOD_TABLE[i];
        // The key is a view into the static table, which has static
        // storage duration. The registry never owns the strings, so a
        // string_view key is safe for as long as the table exists
        // (which is the program's lifetime).
        m_methods.emplace(info.name, &info);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lookup
// ─────────────────────────────────────────────────────────────────────────────

const BuiltinMethodInfo* BuiltinMethodRegistry::getInfo(
    std::string_view name) const {
    auto it = m_methods.find(name);
    return it != m_methods.end() ? it->second : nullptr;
}

bool BuiltinMethodRegistry::isForReceiver(std::string_view name,
                                          ReceiverKind kind) const {
    const BuiltinMethodInfo* info = getInfo(name);
    if (info == nullptr) return false;

    for (ReceiverKind r : info->receivers) {
        if (r == kind) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// `by<Column>`
// ─────────────────────────────────────────────────────────────────────────────

bool BuiltinMethodRegistry::isPrimaryLookupName(std::string_view name) {
    // Must start with "by" and have at least one more character.
    return name.size() > 2 &&
           name[0] == 'b' &&
           name[1] == 'y';
}

std::string_view BuiltinMethodRegistry::primaryLookupColumnName(
    std::string_view name) {
    // Precondition: isPrimaryLookupName(name). Strip the "by" prefix.
    // The suffix is the column's name with its first letter uppercased
    // (per §4.1.5's rule: "the method name is `by` plus the column's
    // name with its first letter uppercased"). The caller lowercases
    // the first character to obtain the column's declared spelling.
    //
    // The return value is a view into `name`; it does not outlive the
    // input.
    return name.substr(2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Enumeration
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string_view> BuiltinMethodRegistry::getAllNames() const {
    std::vector<std::string_view> names;
    names.reserve(m_methods.size());
    for (const auto& pair : m_methods) {
        names.push_back(pair.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}