/// @file AttributeRegistry.hpp
/// @brief Validates `@[...]` attributes against what's legal on declarations.
///
/// @responsibility Validates attributes attached to declarations, checking:
///   - Attribute is known (not misspelled)
///   - Attribute is legal on this declaration kind
///   - Arguments are correctly shaped
///
/// @attribute_table (Grammar.md, "Compiler Directives: Attributes `@[]`")
///   | Attribute               | Legal on                    | Notes                       |
///   | ------------------------ | ---------------------------- | ---------------------------- |
///   | `@[export]`              | top-level declaration only   | rejected inside a block      |
///   | `@[foreign("abi")]`      | function declaration         | only `"C"` is a valid ABI    |
///   | `@[link("name", ...)]`   | module or function decl      | 1+ string arguments          |
///   | `@[deprecated("msg")]`   | any declaration              | warns at use sites           |
///   | `@[inline]`              | function declaration         | hint only, never rejects use |
///
/// @architectural_note Comparing InternedString, not text
///   Attribute names are InternedString. We compare as uint32_t equality.
///
/// @architectural_note Header-only design
///   All functions are inline because the registry has no state and is small
///   enough to be header-only. This simplifies usage and maintenance.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace sema {
namespace attr {

// ─────────────────────────────────────────────────────────────────────────────
// Attribute Name Constants
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Get the interned string for @[export].
inline InternedString kExport(SemaContext& ctx) {
    return ctx.pool().intern("export");
}

/// @brief Get the interned string for @[foreign].
inline InternedString kForeign(SemaContext& ctx) {
    return ctx.pool().intern("foreign");
}

/// @brief Get the interned string for @[link].
inline InternedString kLink(SemaContext& ctx) {
    return ctx.pool().intern("link");
}

/// @brief Get the interned string for @[deprecated].
inline InternedString kDeprecated(SemaContext& ctx) {
    return ctx.pool().intern("deprecated");
}

/// @brief Get the interned string for @[inline].
inline InternedString kInline(SemaContext& ctx) {
    return ctx.pool().intern("inline");
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute Queries
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a declaration has a specific attribute.
inline bool hasAttribute(ArenaSpan<AttributePtr> attrs, InternedString name) {
    for (const AttributeAST* attr : attrs) {
        if (attr->name == name) return true;
    }
    return false;
}

/// @brief Find a specific attribute by name.
inline const AttributeAST* findAttribute(ArenaSpan<AttributePtr> attrs,
                                          InternedString name) {
    for (const AttributeAST* attr : attrs) {
        if (attr->name == name) return attr;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute Validation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate all attributes on a declaration.
inline void validateAttributes(const DeclAST* owner,
                                SemaContext& ctx) {
    // Helper to check if owner is a function
    auto isFunctionOwner = [](const DeclAST* o) -> bool {
        return o != nullptr && o->isa<FuncDeclAST>();
    };

    // Helper to check if at module level
    auto isAtModuleLevel = [&ctx, owner]() -> bool {
        // If owner is nullptr, it's module-level (e.g., standalone @[link])
        if (owner == nullptr) return true;
        return ctx.symbols.isAtModuleLevel();
    };

    for (const AttributeAST* attr : owner->attributes) {
        if (!attr) continue;

        const InternedString name = attr->name;

        // ─── @[export] — top-level only ─────────────────────────────────────
        if (name == kExport(ctx)) {
            if (!attr->args.empty()) {
                ctx.error(attr, DiagCode::E4002,
                          "attribute '@[export]' takes no arguments");
            }
            if (!isAtModuleLevel()) {
                ctx.error(attr, DiagCode::E4001,
                          "attribute '@[export]' is only legal at module level");
            }
            continue;
        }

        // ─── @[foreign("abi")] — function declarations only ────────────────
        if (name == kForeign(ctx)) {
            if (!isFunctionOwner(owner)) {
                ctx.error(attr, DiagCode::E4001,
                          "attribute '@[foreign]' is only legal on function declarations");
                continue;
            }

            // @[foreign] expects exactly 1 argument (the ABI)
            if (attr->args.size() != 1) {
                ctx.error(attr, DiagCode::E4002,
                          "attribute '@[foreign]' expects exactly 1 argument (the ABI), got ",
                          std::to_string(attr->args.size()));
                continue;
            }

            // Only "C" is supported - see Grammar.md's FFI section
            const std::string abi = ctx.pool().lookup(attr->args[0]->value);
            if (abi != "C") {
                ctx.error(attr, DiagCode::E4101,
                          "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
            }
            continue;
        }

        // ─── @[link("name", ...)] — module or function declaration ────────
        if (name == kLink(ctx)) {
            if (owner != nullptr && !isFunctionOwner(owner)) {
                ctx.error(attr, DiagCode::E4001,
                          "attribute '@[link]' is only legal at module level or on function declarations");
                continue;
            }

            if (attr->args.empty()) {
                ctx.error(attr, DiagCode::E4002,
                          "attribute '@[link]' expects at least 1 argument (library name), got 0");
            }
            // Arguments must be string literals - enforced by parser
            continue;
        }

        // ─── @[deprecated("msg")] — legal everywhere ────────────────────────
        if (name == kDeprecated(ctx)) {
            if (attr->args.size() > 1) {
                ctx.error(attr, DiagCode::E4002,
                          "attribute '@[deprecated]' expects at most 1 argument (the message), got ",
                          std::to_string(attr->args.size()));
            }
            continue;
        }

        // ─── @[inline] — function declarations only ────────────────────────
        if (name == kInline(ctx)) {
            if (!isFunctionOwner(owner)) {
                ctx.error(attr, DiagCode::E4001,
                          "attribute '@[inline]' is only legal on function declarations");
                continue;
            }
            if (!attr->args.empty()) {
                ctx.error(attr, DiagCode::E4002,
                          "attribute '@[inline]' takes no arguments");
            }
            continue;
        }

        // ─── Unknown attribute ───────────────────────────────────────────────
        ctx.error(attr, DiagCode::E4003,
                  "unknown attribute '@", ctx.pool().lookup(name), "'");
    }
}

} // namespace attr
} // namespace sema