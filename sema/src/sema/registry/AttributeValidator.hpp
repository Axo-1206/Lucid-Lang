/// @file registry/AttributeValidator.hpp
/// @brief Validation of attributes attached to declarations.
///
/// ─── Role ─────────────────────────────────────────────────────────────────
/// The parser attaches an `ArenaSpan<AttributeAST*>` to every declaration
/// that may carry attributes. This file walks that span and does four
/// things:
///
///   1. Confirms each attribute is one the language recognizes.
///   2. Confirms the attribute is legal on the declaration kind it is
///      attached to.
///   3. Confirms the attribute's argument list has the shape the
///      attribute requires, and its argument values are valid.
///   4. Writes the attribute's meaning onto the declaration as a decoded
///      field, so later passes read `decl->isExported` instead of
///      re-walking the span.
///
/// The span is the source of truth; the decoded fields are a cache of
/// its interpretation. If the two ever disagree, that is a Sema bug.
///
/// ─── What this file does NOT do ───────────────────────────────────────────
/// It does not check table-shape consistency. Rules that need the
/// table's columns, its `isHostBacked` flag, or the resolved types of
/// its columns belong to `TableConstraintChecker`, which runs after
/// this file has written the decoded flags.
///
/// It does not check sequence-body restrictions. "`@sequence` must
/// return `unit`", "`@sequence` cannot be host-bound", "`wait*` only
/// inside a `@sequence` body" — belong to `SequenceChecker` or to the
/// per-statement resolvers.
///
/// It does not check initializer-row consistency. "Duplicate
/// `@unique`/`@primary` values in an inline initializer", "every cell
/// is a `const_expr`" — belong to `TableConstraintChecker`.
///
/// It does not emit deprecation warnings. `@deprecated(msg)` records
/// the message on the declaration; the warning is emitted at each *use
/// site*.
///
/// ─── Design: the decoded fields are the only output ───────────────────────
/// Every attribute maps to one field on the declaration (or, for
/// `@primary` + `@unique`, two). The mapping is:
///
///   @export        → DeclAST::isExported
///   @deprecated(m) → DeclAST::deprecationMessage
///   @fixed         → TableDeclAST::isFixed
///   @readonly      → TableDeclAST::isReadonly  /  ColumnDeclAST::isReadonly
///   @packed        → TableDeclAST::isPacked
///   @reserve(N)    → TableDeclAST::isReserved, TableDeclAST::reservedCount
///   @columnar      → TableDeclAST::isColumnar
///   @request       → TableDeclAST::isRequest
///   @unique        → ColumnDeclAST::isUnique
///   @primary       → ColumnDeclAST::isPrimary  (and ColumnDeclAST::isUnique)
///   @sequence      → FnDeclAST::isSequence
///
/// The dispatch table lives in `validateAttribute`; the flag-setting is
/// in the same switch, so a reader sees the full attribute-to-field
/// mapping in one place.
///
/// ─── Design: cross-attribute checks in this file ──────────────────────────
/// Two rules are decided by the combination of attributes, not by any
/// single one:
///
///   - `@packed` requires `@fixed` or `@readonly`;
///   - `@fixed` and `@readonly` are mutually exclusive.
///
/// Both can be decided from the attribute span alone (no table shape
/// needed), so they live here rather than in `TableConstraintChecker`.
/// Keeping them here means every "which attributes are on this
/// declaration?" rule is in one file.
///
/// ─── Design: specific codes for specific attributes ───────────────────────
/// `@export` on a local declaration emits `Attr_ExportInLocalScope`;
/// `@deprecated` on a local declaration emits
/// `Attr_DeprecatedInLocalScope`; any other module-only attribute on a
/// local emits `Attr_NotAllowedOnLocal`. The three codes let a tool
/// filter for a specific placement error without matching on the
/// message text.

#pragma once

#include "../context/SemaContext.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/registry/AttributeRegistry.hpp"

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate every attribute on a declaration, then run
///        cross-attribute checks.
///
/// Walks `decl->attributes`, dispatching each attribute through
/// `validateAttribute`. On success, the declaration's decoded fields
/// (isExported, isFixed, isPacked, ...) reflect the span.
///
/// After the per-attribute pass, runs two checks that need the full
/// list rather than one attribute at a time:
///
///   - duplicate attribute names;
///   - `@fixed` + `@readonly` (mutually exclusive) and `@packed` without
///     `@fixed`/`@readonly` (an explicit requirement, never inferred).
///
/// @return true if every attribute is valid and every combination check
///         passed. On false, at least one diagnostic has been emitted.
///
/// A declaration with an empty attribute span is valid and returns true.
/// A `nullptr` declaration is valid and returns true.
bool validateAllAttributes(DeclAST* decl, SemaContext& ctx);

/// @brief Validate a single attribute against its owner.
///
/// Looks the attribute's name up in `ctx.attributeRegistry`, checks
/// that it is allowed on `owner->kind`, dispatches to the specific
/// validator if the attribute has non-trivial rules, and sets the
/// decoded field the attribute maps to.
///
/// @return true if the attribute is valid on its owner. On false, at
///         least one diagnostic has been emitted. The decoded field is
///         set only on success.
bool validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Per-attribute validators
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate `@export`.
///
/// Rules:
///   - No arguments.
///   - The declaration is at module level.
///
/// On success, the caller (`validateAttribute`) sets
/// `owner->isExported = true`.
bool validateExport(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate `@reserve(N)`.
///
/// Rules:
///   - Exactly one argument.
///   - The argument is a compile-time integer literal.
///   - The value is non-negative.
///
/// On success, this function writes `table->isReserved = true` and
/// `table->reservedCount = N`.
///
/// It does NOT check that the table is growing; that rule needs the
/// whole attribute list and lives in `TableConstraintChecker`.
bool validateReserve(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate `@deprecated(msg)`.
///
/// Rules:
///   - Zero or one argument. The no-argument form is legal and means
///     "deprecated, no message".
///   - If present, the argument is a string literal.
///   - The declaration is at module level.
///
/// On success with a message, this function writes
/// `owner->deprecationMessage = msg`.
bool validateDeprecated(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Is this declaration one of the module's top-level decls?
///
/// Checks the declaration's actual position in the module's `decls`
/// span, not the current scope. This is what distinguishes a top-level
/// `let` (which may carry `@export` or `@deprecated`) from a local one
/// (which may not).
bool isModuleLevelDeclaration(DeclAST* decl, SemaContext& ctx);

} // namespace lucid::sema