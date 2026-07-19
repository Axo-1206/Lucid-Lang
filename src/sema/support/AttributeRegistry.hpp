/**
 * @file AttributesRegistry.hpp
 * @brief Validates `@[...]` attributes against what's legal on the
 *        declaration they're attached to.
 *
 * @responsibility Implements the two functions Sema.hpp declares under its
 *                 "Attributes" section — `validateAttributes()` and
 *                 `validateAttribute()`. Header-only: unlike IntrinsicRegistry
 *                 (a ~60-entry table checked on every intrinsic call, worth
 *                 pre-interning once — see IntrinsicRegistry.hpp's own note),
 *                 this validates against five fixed attribute names, checked
 *                 once per `@[...]` seen — re-interning through `ctx.pool`
 *                 on each call is cheap enough that caching adds complexity
 *                 without a measured need for it.
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
 *   `attr->name` is already an `InternedString`, interned by the parser
 *   against the session's `StringPool` (`ctx.pool` — see SemaContext.hpp).
 *   Each attribute name below is interned into `ctx.pool` and compared as
 *   an `InternedString` — a single `uint32_t` equality check, not a text
 *   compare — matching InternedString.hpp's whole reason for existing.
 *   `ctx.pool.intern()` is idempotent and hash-map backed (see
 *   StringPool.hpp's "Deduplication" note), so repeated calls for the same
 *   literal text are cheap and always resolve to the same ID within one
 *   pool.
 *
 * @architectural_note Composing multi-part messages
 *   `SemaContext::error()` (see its doc comment in SemaContext.hpp) folds
 *   every variadic argument into ONE string via `buildMessage()` before it
 *   ever reaches a `DiagCode`'s template — so a `DiagCode` used from Sema
 *   only ever fills a single `%s`. Every diagnostic below therefore passes
 *   several small literal + InternedString pieces to `ctx.error()`, which
 *   concatenates them (no separators inserted — each piece supplies its own
 *   spacing/punctuation), rather than relying on a template with several
 *   independent `%s` slots the way Parser-phase codes do.
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "../SemaContext.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

// Forward-declared so validateAttributes() can call it before its own
// definition below — mirrors how Sema.hpp declares both together.
inline void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/**
 * @brief Validate every attribute in `attrs` against what's legal on
 *        `owner`'s kind of declaration.
 *
 * Just forwards to validateAttribute() per entry. Deliberately does NOT
 * check for duplicates (e.g. `@[inline, inline]`) — a repeated attribute is
 * inert at lowering time, not a language rule, so rejecting it would be
 * noise rather than a real diagnostic.
 *
 * @param attrs The attribute list as parsed (see Parser.hpp's
 *        `parseAttributes()`).
 * @param owner The declaration the attributes are attached to. May be
 *        nullptr for a module-level `@[link(...)]` that isn't attached to
 *        any single declaration — see validateAttribute()'s handling of
 *        `@[link(...)]` below.
 */
inline void validateAttributes(ArenaSpan<AttributePtr> attrs, DeclAST* owner, SemaContext& ctx) {
    for (AttributeAST* attr : attrs) {
        validateAttribute(attr, owner, ctx);
    }
}

/**
 * @brief Validate one attribute against `owner`'s declaration kind.
 *
 * Each branch checks two things: is this attribute legal on this *kind* of
 * declaration (Grammar.md's "Legal on" column), and are its arguments
 * shaped the way that attribute requires. Unrecognized attribute names are
 * themselves an error — Lucid does not silently ignore an unknown `@[...]`.
 */
inline void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    const InternedString name = attr->name;

    // Every "legal on a function declaration" rule below reduces to this
    // one check — DeclAST has no separate "declaration kind" enum beyond
    // BaseAST::kind, so isa<FuncDeclAST>() (not RTTI/dynamic_cast — see
    // BaseAST::isa()'s own definition) is how the codebase asks this.
    const bool ownerIsFunc = (owner != nullptr) && owner->isa<FuncDeclAST>();

    // ─── @[export] — top-level only ─────────────────────────────────────
    if (name == ctx.pool.intern("export")) {
        // The parser already rejects `@[export]` inside a block (see
        // Grammar.md's "Visibility inside blocks" note) — this check is a
        // second line of defense, not the primary enforcement point, in
        // case that parser-level rule ever regresses.
        if (!ctx.isAtModuleLevel()) {
            ctx.error(attr, DiagCode::E4001, "attribute '", ctx.toString(name), "' is not legal here");
        }
        return;
    }

    // ─── @[foreign("abi")] — function declarations, ABI must be "C" ────
    if (name == ctx.pool.intern("foreign")) {
        if (!ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001, "attribute '", ctx.toString(name), "' is not legal here");
            return;
        }
        if (attr->args.size() != 1) {
            ctx.error(attr, DiagCode::E4002, "wrong number of arguments for attribute '",
                       ctx.toString(name), "': expected 1, found ", std::to_string(attr->args.size()));
            return;
        }
        // Only "C" is supported — see Grammar.md's "Foreign Function
        // Interface" section for why "C++" is deliberately rejected here
        // rather than accepted and mishandled.
        const std::string abi = ctx.toString(attr->args[0]->value);
        if (abi != "C") {
            ctx.error(attr, DiagCode::E4101, "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
        }
        return;
    }

    // ─── @[link(...)] — module level or a function declaration ─────────
    if (name == ctx.pool.intern("link")) {
        // `owner == nullptr` covers a module-level @[link(...)] that isn't
        // attached to any single declaration (see Grammar.md's `@[link(...)]`
        // examples).
        if (owner != nullptr && !ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001, "attribute '", ctx.toString(name), "' is not legal here");
            return;
        }
        if (attr->args.empty()) {
            ctx.error(attr, DiagCode::E4002, "wrong number of arguments for attribute '",
                       ctx.toString(name), "': expected at least 1, found 0");
        }
        return;
    }

    // ─── @[deprecated("msg")] — legal everywhere ────────────────────────
    if (name == ctx.pool.intern("deprecated")) {
        // Legal on any declaration per Grammar.md's table. The message
        // argument's shape is already enforced by the parser
        // (parseAttributeArgLiteral) — nothing left to validate here.
        // Emitting the actual "use of deprecated X" warning happens at
        // *use* sites (resolveValueOrError / resolveTypeNameOrError), not
        // here at the declaration site.
        return;
    }

    // ─── @[inline] — function declarations only, hint-only ─────────────
    if (name == ctx.pool.intern("inline")) {
        if (!ownerIsFunc) {
            ctx.error(attr, DiagCode::E4001, "attribute '", ctx.toString(name), "' is not legal here");
        }
        return;
    }

    // ─── Unknown attribute ───────────────────────────────────────────────
    ctx.error(attr, DiagCode::E4003, "unknown attribute '", ctx.toString(name), "'");
}