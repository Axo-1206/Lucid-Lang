/// @file registry/AttributeValidator.cpp
/// @brief Implementation of attribute validation.

#include "AttributeValidator.hpp"

#include "ArgTypeValidators.hpp"
#include "sema/const_eval/ConstEvaluator.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/ExprAST.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_set>

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// Internal helpers
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// Set a no-argument attribute's decoded flag on its owner.
///
/// Every "flag" attribute — `@fixed`, `@readonly`, `@packed`,
/// `@columnar`, `@request`, `@unique`, `@primary`, `@sequence` — has
/// the same shape: no arguments, and on success, one field on the
/// declaration becomes true. This helper encodes that shape once.
///
/// The `setter` callable writes the field. The dispatcher passes a
/// lambda per attribute; the lambda is the only thing that differs
/// between the eight flag attributes.
template <typename Setter>
bool setFlagWithNoArgs(AttributeAST* attr, DeclAST* owner,
                       SemaContext& ctx, Setter&& setter) {
    if (!attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgCount, attr,
                              "attribute '@", ctx.pool.lookup(attr->name),
                              "' takes no arguments");
        return false;
    }
    setter(owner);
    return true;
}

/// Look up an attribute's registry entry.
///
/// Emits `Attr_Unknown` and returns null if the name is not recognized.
const AttributeInfo* lookupAttribute(AttributeAST* attr, SemaContext& ctx) {
    std::string_view name = ctx.pool.lookupView(attr->name);
    const AttributeInfo* info = ctx.attributeRegistry.getInfo(name);
    if (!info) {
        ctx.diagnostics.error(DiagCode::Attr_Unknown, attr,
                              "unknown attribute '@",
                              ctx.pool.lookup(attr->name), "'");
    }
    return info;
}

/// Check that the attribute is legal on this declaration kind.
///
/// Emits `Attr_NotApplicable` and returns false if not.
bool checkAllowedOnDecl(const AttributeInfo* info, AttributeAST* attr,
                        DeclAST* owner, SemaContext& ctx) {
    if (ctx.attributeRegistry.isAllowedOnDecl(info->name, owner->kind)) {
        return true;
    }
    ctx.diagnostics.error(DiagCode::Attr_NotApplicable, attr,
                          "attribute '@", ctx.pool.lookup(attr->name),
                          "' cannot be applied to '",
                          astKindToString(owner->kind), "'");
    return false;
}

/// Emit the correct local-placement diagnostic for a module-only
/// attribute used on a local declaration.
///
/// Three codes exist so a tool can filter for a specific placement
/// error without matching on message text:
///
///   - `@export` on a local   → `Attr_ExportInLocalScope`
///   - `@deprecated` on a local → `Attr_DeprecatedInLocalScope`
///   - any other module-only attribute → `Attr_NotAllowedOnLocal`
void emitLocalPlacementError(AttributeAST* attr, SemaContext& ctx) {
    std::string_view name = ctx.pool.lookupView(attr->name);

    DiagCode code = DiagCode::Attr_NotAllowedOnLocal;
    if (name == "export")         code = DiagCode::Attr_ExportInLocalScope;
    else if (name == "deprecated") code = DiagCode::Attr_DeprecatedInLocalScope;

    ctx.diagnostics.error(code, attr,
                          "'@", name, "' is only legal on a top-level "
                          "declaration");
    ctx.diagnostics.note(attr,
                         "a local 'let'/'const' is visible only inside the "
                         "block that declares it — there is no outer scope "
                         "for '@", name, "' to reach");
}

/// The two cross-attribute checks on a table.
///
/// Both can be decided from the decoded flags that pass 1 set.
/// `@packed` requires `@fixed` or `@readonly`; `@fixed` and `@readonly`
/// are mutually exclusive.
bool checkTableAttributeCombinations(TableDeclAST* table, SemaContext& ctx) {
    bool allValid = true;

    // ─── @packed requires @fixed or @readonly ───────────────────────────
    //
    // §4.1.4: a packed table's storage has no slack; its row set cannot
    // change. The requirement is explicit and never inferred — a bare
    // `@packed` is an error even though the compiler could technically
    // imply one of the two fixed-row-set attributes.
    if (table->isPacked && !table->hasFixedRowSet()) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidCombination, table,
                              "table '", ctx.pool.lookup(table->name),
                              "' carries '@packed' without '@fixed' or "
                              "'@readonly' — a packed table's row set "
                              "cannot change, so it must say which kind "
                              "of fixed table it is");
        ctx.diagnostics.note(table,
                             "write '@packed @fixed' for writable cells, "
                             "or '@packed @readonly' for a frozen table");
        allValid = false;
    }

    // ─── @fixed and @readonly are mutually exclusive ────────────────────
    //
    // §4.1.4: `@fixed` is redundant with `@readonly`, which already
    // forbids everything `@fixed` does. Writing both is a user error
    // even though the combination would be semantically consistent.
    if (table->isFixed && table->isReadonly) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidCombination, table,
                              "table '", ctx.pool.lookup(table->name),
                              "' carries both '@fixed' and '@readonly' — "
                              "'@fixed' is redundant, since '@readonly' "
                              "already forbids everything '@fixed' does");
        ctx.diagnostics.note(table,
                             "use '@readonly' alone for a fully frozen "
                             "table, or '@fixed' alone when the cells "
                             "remain writable");
        allValid = false;
    }

    return allValid;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// validateAllAttributes
// ═════════════════════════════════════════════════════════════════════════════

bool validateAllAttributes(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return true;
    if (decl->hasSyntaxError) return true;

    bool allValid = true;

    // ─── Pass 1: per-attribute validation ───────────────────────────────
    //
    // Each attribute is validated independently and sets its decoded
    // field on success. A failure here does not short-circuit the loop:
    // if three attributes on a declaration are wrong, the user sees
    // three diagnostics.
    for (AttributeAST* attr : decl->attributes) {
        if (!attr) continue;
        if (!validateAttribute(attr, decl, ctx)) {
            allValid = false;
        }
    }

    // ─── Pass 2: duplicate detection ────────────────────────────────────
    //
    // A declaration may not carry the same attribute twice. Every
    // attribute in the grammar is `repeatable = false`, so this check
    // is unconditional.
    std::unordered_set<InternedString> seen;
    for (AttributeAST* attr : decl->attributes) {
        if (!attr) continue;
        if (seen.count(attr->name)) {
            ctx.diagnostics.error(DiagCode::Attr_Duplicate, attr,
                                  "duplicate attribute '@",
                                  ctx.pool.lookup(attr->name),
                                  "' on '", ctx.pool.lookup(decl->name), "'");
            allValid = false;
        }
        seen.insert(attr->name);
    }

    // ─── Pass 3: cross-attribute checks ─────────────────────────────────
    //
    // These rules are decided by the combination of attributes, not by
    // any single one. They run after pass 1 so the decoded flags are
    // populated.
    if (decl->isa<TableDeclAST>()) {
        if (!checkTableAttributeCombinations(decl->as<TableDeclAST>(), ctx)) {
            allValid = false;
        }
    }

    return allValid;
}

// ═════════════════════════════════════════════════════════════════════════════
// validateAttribute — the dispatcher
// ═════════════════════════════════════════════════════════════════════════════

bool validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    if (!attr) return false;

    // ─── Step 1: is this a recognized attribute? ────────────────────────
    const AttributeInfo* info = lookupAttribute(attr, ctx);
    if (!info) return false;

    // ─── Step 2: is it allowed on this declaration kind? ────────────────
    if (!checkAllowedOnDecl(info, attr, owner, ctx)) return false;

    // ─── Step 3: dispatch ───────────────────────────────────────────────
    //
    // The names are compared as `std::string_view` against the literals
    // below. `ctx.attributeRegistry` uses the same spellings; the two
    // must agree, and a mismatch here is a compiler bug, not a user
    // error. The trailing `AST_ASSERT_MSG` catches it.
    //
    // Three attributes have a dedicated validator: their rules are not
    // expressible as "check args are empty; set a bool". The rest do,
    // and are handled inline.
    //
    // The `@primary` case sets both `isPrimary` and `isUnique` —
    // grammar §4.1.5 says `@primary` implies `@unique`, and every
    // consumer wants `isUnique` to mean "this column forbids
    // duplicates" regardless of which attribute caused it.

    std::string_view name = info->name;

    // ─── @export ────────────────────────────────────────────────────────
    if (name == "export") {
        if (!validateExport(attr, owner, ctx)) return false;
        owner->isExported = true;
        return true;
    }

    // ─── @deprecated(msg) ───────────────────────────────────────────────
    if (name == "deprecated") {
        return validateDeprecated(attr, owner, ctx);
    }

    // ─── @reserve(N) ────────────────────────────────────────────────────
    if (name == "reserve") {
        return validateReserve(attr, owner, ctx);
    }

    // ─── @fixed (table) ─────────────────────────────────────────────────
    if (name == "fixed") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<TableDeclAST>()->isFixed = true;
        });
    }

    // ─── @readonly (table or column) ────────────────────────────────────
    if (name == "readonly") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            if (d->isa<TableDeclAST>()) {
                d->as<TableDeclAST>()->isReadonly = true;
            } else {
                d->as<ColumnDeclAST>()->isReadonly = true;
            }
        });
    }

    // ─── @packed (table) ────────────────────────────────────────────────
    if (name == "packed") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<TableDeclAST>()->isPacked = true;
        });
    }

    // ─── @columnar (table) ──────────────────────────────────────────────
    if (name == "columnar") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<TableDeclAST>()->isColumnar = true;
        });
    }

    // ─── @request (table) ───────────────────────────────────────────────
    if (name == "request") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<TableDeclAST>()->isRequest = true;
        });
    }

    // ─── @unique (column) ───────────────────────────────────────────────
    if (name == "unique") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<ColumnDeclAST>()->isUnique = true;
        });
    }

    // ─── @primary (column) ──────────────────────────────────────────────
    // @primary implies @unique (§4.1.5). Write both flags.
    if (name == "primary") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            ColumnDeclAST* c = d->as<ColumnDeclAST>();
            c->isPrimary = true;
            c->isUnique  = true;
        });
    }

    // ─── @sequence (function) ───────────────────────────────────────────
    if (name == "sequence") {
        return setFlagWithNoArgs(attr, owner, ctx, [](DeclAST* d) {
            d->as<FnDeclAST>()->isSequence = true;
        });
    }

    // ─── Unreachable ────────────────────────────────────────────────────
    //
    // The registry and the dispatch above must agree on the attribute
    // set. Reaching this point means the registry has an entry the
    // dispatcher does not handle — a compiler bug, not a user error.
    AST_ASSERT_MSG(false,
                   "attribute registry and validator dispatch disagree: "
                   "a registered attribute has no handler");
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Dedicated validators
// ═════════════════════════════════════════════════════════════════════════════

bool validateExport(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // No arguments.
    if (!attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgCount, attr,
                              "attribute '@export' takes no arguments");
        return false;
    }

    // Module-level only.
    if (!isModuleLevelDeclaration(owner, ctx)) {
        emitLocalPlacementError(attr, ctx);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

bool validateReserve(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // Exactly one argument.
    if (attr->args.size() != 1) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgCount, attr,
                              "attribute '@reserve' expects exactly one "
                              "integer argument, got ", attr->args.size());
        return false;
    }

    // The argument must be a compile-time integer literal.
    //
    // `validateIntArg` performs the shape check (it is a literal) and
    // the value read (through `ConstEvaluator::evaluateLiteral`). It
    // returns an `optional<int64_t>`: nullopt on failure (with a
    // diagnostic already emitted), a value on success.
    auto count = validateIntArg(attr->args[0], "reserve count", ctx);
    if (!count.has_value()) {
        return false;
    }

    if (*count < 0) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, attr->args[0],
                              "'@reserve' count must be non-negative, got ",
                              *count);
        return false;
    }

    // Write the decoded fields. This is the one place an attribute's
    // decoded field depends on an argument value; the flag-style
    // attributes let the dispatcher set their bool.
    TableDeclAST* table = owner->as<TableDeclAST>();
    table->isReserved    = true;
    table->reservedCount = static_cast<uint64_t>(*count);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

bool validateDeprecated(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // Zero or one argument.
    if (attr->args.size() > 1) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgCount, attr,
                              "attribute '@deprecated' expects at most one "
                              "string argument, got ", attr->args.size());
        return false;
    }

    // Module-level only.
    if (!isModuleLevelDeclaration(owner, ctx)) {
        emitLocalPlacementError(attr, ctx);
        return false;
    }

    // No message: legal, marker form. The field stays invalid; the
    // warning emitter falls back to a default message.
    if (attr->args.empty()) {
        return true;
    }

    // With a message: must be a string literal. `validateStringArg`
    // performs the check and returns the interned lexeme on success.
    auto msg = validateStringArg(attr->args[0], "deprecation message", ctx);
    if (!msg.has_value()) {
        return false;
    }

    owner->deprecationMessage = *msg;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Helper: isModuleLevelDeclaration
// ═════════════════════════════════════════════════════════════════════════════

bool isModuleLevelDeclaration(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return false;
    if (!ctx.currentModule) return false;

    // Structural check: walk the module's top-level `decls` span and
    // compare pointers. This asks "was this declaration one of the
    // module's top-level declarations?" — a fact about the source,
    // not about the current scope depth.
    //
    // The alternative — `ctx.isAtModuleLevel()` — asks "is the scope
    // stack empty right now?", which is a fact about *when* the check
    // runs. A local `let` resolved during body resolution has a
    // non-empty scope; a top-level `let` resolved during declaration
    // pass has an empty one. But a caller that invokes
    // `validateAllAttributes` at the wrong moment could see the wrong
    // scope state and report the wrong answer. The structural check
    // cannot be fooled.
    for (DeclAST* d : ctx.currentModule->decls) {
        if (d == decl) return true;
    }
    return false;
}

} // namespace lucid::sema