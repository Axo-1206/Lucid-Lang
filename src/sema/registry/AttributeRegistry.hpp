/**
 * @file AttributesRegistry.hpp
 * @brief Validates `@[...]` attributes against what's legal on the
 *        declaration they're attached to.
 *
 * @responsibility Implements the two functions Sema.hpp declares under its
 *                 "Attributes" section — `validateAttributes()` and
 *                 `validateAttribute()`.
 *
 * @attribute_table (Grammar.md, "Compiler Directives: Attributes `@[]`")
 *   | Attribute               | Legal on                    | Notes                       |
 *   | ------------------------ | ---------------------------- | ---------------------------- |
 *   | `@[export]`              | top-level declaration only   | rejected inside a block      |
 *   | `@[foreign("abi")]`      | function declaration         | only `"C"` is a valid ABI    |
 *   | `@[link("name", ...)]`   | module or function decl      | 1+ string arguments          |
 *   | `@[deprecated("msg")]`   | any declaration               | warns at use sites           |
 *   | `@[inline]`              | function declaration         | hint only, never rejects use |
 *
 * @architectural_note Comparing InternedString, not text
 *   `attr->name` is already an `InternedString`. We intern each attribute
 *   name once per call and compare as uint32_t equality.
 *
 * @architectural_note Const-correctness
 *   Attributes and declarations are read-only. The parser created them.
 *   We only validate, never modify.
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Attribute Name Constants (interned once per call)
// ─────────────────────────────────────────────────────────────────────────────

namespace attr {

inline InternedString exportAttr(SemaContext& ctx) {
    return ctx.pool().intern("export");
}

inline InternedString foreignAttr(SemaContext& ctx) {
    return ctx.pool().intern("foreign");
}

inline InternedString linkAttr(SemaContext& ctx) {
    return ctx.pool().intern("link");
}

inline InternedString deprecatedAttr(SemaContext& ctx) {
    return ctx.pool().intern("deprecated");
}

inline InternedString inlineAttr(SemaContext& ctx) {
    return ctx.pool().intern("inline");
}

} // namespace attr

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Validate every attribute in `attrs` against what's legal on `owner`.
 *
 * @param attrs The attribute list as parsed.
 * @param owner The declaration the attributes are attached to.
 *              May be nullptr for module-level `@[link(...)]`.
 * @param ctx The semantic context.
 */
inline void validateAttributes(ArenaSpan<AttributePtr> attrs,
                               const DeclAST* owner,
                               SemaContext& ctx) {
    for (const AttributeAST* attr : attrs) {
        validateAttribute(attr, owner, ctx);
    }
}

/**
 * @brief Validate one attribute against `owner`'s declaration kind.
 *
 * Each branch checks:
 *   1. Is this attribute legal on this kind of declaration?
 *   2. Are its arguments shaped correctly?
 *
 * Unknown attribute names are an error.
 */
inline void validateAttribute(const AttributeAST* attr,
                              const DeclAST* owner,
                              SemaContext& ctx) {
    if (!attr) return;

    const InternedString name = attr->name;

    // Check if owner is a function (used by multiple attribute rules)
    const bool ownerIsFunc = (owner != nullptr) && owner->isa<FuncDeclAST>();

    // ─── @[export] — top-level only ─────────────────────────────────────
    if (name == attr::exportAttr(ctx)) {
        // The parser already rejects `@[export]` inside a block.
        // This is a second line of defense.
        if (!ctx.symbols.isAtModuleLevel()) {
            ctx.error(attr, DiagCode::E4001,
                      "attribute '@[export]' is not legal inside a block");
        }
        // @[export] takes no arguments - already enforced by parser
        return;
    }

    // ─── @[foreign("abi")] — function declarations only ──────────────────
    if (name == attr::foreignAttr(ctx)) {
        if (!ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001,
                      "attribute '@[foreign]' is only legal on function declarations");
            return;
        }

        if (attr->args.size() != 1) {
            ctx.error(attr, DiagCode::E4002,
                      "attribute '@[foreign]' expects exactly 1 argument (the ABI), got ",
                      std::to_string(attr->args.size()));
            return;
        }

        // Only "C" is supported - see Grammar.md's FFI section
        const std::string abi = ctx.pool().lookup(attr->args[0]->value);
        if (abi != "C") {
            ctx.error(attr, DiagCode::E4101,
                      "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
        }
        return;
    }

    // ─── @[link("name", ...)] — module level or function declaration ────
    if (name == attr::linkAttr(ctx)) {
        // owner == nullptr covers module-level @[link(...)]
        if (owner != nullptr && !ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001,
                      "attribute '@[link]' is only legal at module level or on function declarations");
            return;
        }

        if (attr->args.empty()) {
            ctx.error(attr, DiagCode::E4002,
                      "attribute '@[link]' expects at least 1 argument (library name), got 0");
        }
        // Arguments must be string literals - enforced by parser
        return;
    }

    // ─── @[deprecated("msg")] — legal everywhere ────────────────────────
    if (name == attr::deprecatedAttr(ctx)) {
        // Legal on any declaration. The message is optional (0 or 1 args).
        // If present, it must be a string literal - enforced by parser.
        // Warning emission happens at use sites, not here.
        if (attr->args.size() > 1) {
            ctx.error(attr, DiagCode::E4002,
                      "attribute '@[deprecated]' expects at most 1 argument (the message), got ",
                      std::to_string(attr->args.size()));
        }
        return;
    }

    // ─── @[inline] — function declarations only, hint-only ──────────────
    if (name == attr::inlineAttr(ctx)) {
        if (!ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001,
                      "attribute '@[inline]' is only legal on function declarations");
        }
        // @[inline] takes no arguments - enforced by parser
        return;
    }

    // ─── Unknown attribute ───────────────────────────────────────────────
    ctx.error(attr, DiagCode::E4003,
              "unknown attribute '@", ctx.pool().lookup(name), "'");
}

} // namespace sema