/// @file registry/AttributeRegistry.hpp
/// @brief Pure data registry for Lucid attributes.
///
/// ─── What this file is ────────────────────────────────────────────────────
/// A lookup table for the language's built-in attributes. Given an
/// attribute's name, it answers:
///
///   1. Is this a recognized attribute?
///   2. What is the shape of its arguments?
///   3. Which declaration kinds may it attach to?
///
/// ─── What this file is not ────────────────────────────────────────────────
/// It does not validate semantics. The registry records that `@reserve`
/// takes one integer argument; whether the integer is positive, whether
/// it is within range, whether the target table is growing, and whether
/// `@reserve` and `@packed` are compatible are all Sema's job. The
/// registry is a table of syntactic facts.
///
/// It does not intern. The attribute names are a fixed set of well-known
/// strings. There is no reason to route them through a `StringPool`; a
/// `std::string_view` key is enough, and it makes the registry a value
/// that does not hold a pool reference. A `CompilationSession` may hold
/// one instance; a program that runs two sessions does not share state
/// between them by accident.
///
/// ─── Design: fixed table, compile-time known ──────────────────────────────
/// The attribute set is closed (grammar §4.1.4, §4.1.5, §4.2.6). It is
/// spelled out in a static table and never changes at runtime. Adding
/// an attribute is a change to the table plus a change to Sema's
/// handling of it. There is no runtime registration path — a host
/// cannot introduce new attributes.
///
/// ─── Design: argument shape is an enum, not a bool ────────────────────────
/// The grammar's attributes have three argument shapes:
///
///   - None:           `@export`, `@sequence`, `@readonly`, ...
///   - OneString:      `@deprecated("...")`
///   - OneInteger:     `@reserve(1000)`
///
/// The old design's `requiresStringArgs: bool` + `minArgs/maxArgs` was
/// too coarse to express this. A single enum value per attribute is
/// simpler and does not lose information.

#pragma once

#include "core/ast/BaseAST.hpp"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// AttrArgShape
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The argument shape an attribute accepts.
///
/// The parser produces the arguments it sees; the registry records what
/// shape each attribute accepts; Sema checks the two against each other.
enum class AttrArgShape : uint8_t {
    /// The attribute takes no arguments. `@export`, `@sequence`, ...
    None,

    /// The attribute takes exactly one string literal.
    /// `@deprecated("use X instead")`
    OneString,

    /// The attribute takes exactly one integer literal.
    /// `@reserve(1000)`.
    OneInteger,
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeInfo
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Information about one attribute: its name, its argument shape,
///        and the declaration kinds it may attach to.
struct AttributeInfo {
    /// The attribute's name, without the `@` prefix.
    std::string_view name;

    /// The shape of the attribute's argument list.
    AttrArgShape argShape = AttrArgShape::None;

    /// True for attributes that may be written more than once on the same
    /// declaration. Every attribute in the current grammar is unique per
    /// declaration, so this is `false` everywhere; kept as a field for a
    /// future attribute that might allow it.
    bool repeatable = false;

    /// The declaration kinds the attribute may attach to. An attribute
    /// that applies to tables and columns lists both `TableDecl` and
    /// `ColumnDecl`; the two are distinguished at the use site by where
    /// the attribute appears, not by the registry.
    ///
    /// The kinds are `ASTKind` values. Because the registry is a fixed
    /// table, the kinds are compile-time constants. The `ASTKind` enum is
    /// the source of truth for what "a declaration kind" means.
    std::initializer_list<ASTKind> allowedKinds;
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeRegistry
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The fixed table of built-in attributes.
///
/// A caller looks up an attribute by its (interned or plain) name and
/// receives an `AttributeInfo*`. A null return means "not a recognized
/// attribute"; Sema reports this as an unknown-attribute error.
///
/// The registry is cheap to construct and holds no per-session state.
/// A `CompilationSession` may hold one; a caller that needs a one-off
/// lookup may construct a temporary.
class AttributeRegistry {
public:
    AttributeRegistry();

    AttributeRegistry(const AttributeRegistry&)            = default;
    AttributeRegistry& operator=(const AttributeRegistry&) = default;
    AttributeRegistry(AttributeRegistry&&)                 = default;
    AttributeRegistry& operator=(AttributeRegistry&&)      = default;

    // ─── Lookup ──────────────────────────────────────────────────────────

    /// The info for an attribute, or nullptr if the name is not a
    /// recognized attribute.
    const AttributeInfo* getInfo(std::string_view name) const;

    /// True if the name is a recognized attribute.
    bool isRegistered(std::string_view name) const {
        return getInfo(name) != nullptr;
    }

    /// True if the attribute may attach to the given declaration kind.
    bool isAllowedOnDecl(std::string_view name, ASTKind declKind) const;

    // ─── Enumeration ────────────────────────────────────────────────────
    //
    // The registry's contents are exposed for tooling: an LSP that wants
    // to suggest attribute names, a doc generator that wants to list them,
    // a test that checks the table against the grammar.

    /// The names of all registered attributes, sorted lexicographically.
    std::vector<std::string_view> getAllNames() const;

    /// The number of registered attributes.
    size_t size() const noexcept { return m_attributes.size(); }

private:
    /// The lookup table. Keyed by the attribute's spelling. The keys
    /// point into `ATTRIBUTE_TABLE`'s string_views, which have static
    /// storage duration; the map holds views into that storage, not
    /// copies. `std::unordered_map` does not own its keys, so the
    /// views must remain valid for the registry's lifetime, which they
    /// do because they point into the static table.
    std::unordered_map<std::string_view, const AttributeInfo*> m_attributes;
};