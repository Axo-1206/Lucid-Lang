/// @file SemaValidate.cpp
/// @brief Semantic validation rules that are neither predicates nor
///        equality checks.
///
/// ─── What lives here ──────────────────────────────────────────────────────
/// Rules that take a resolved type (or a declaration) and a context, and
/// emit a diagnostic if the combination is illegal. They are distinct from
/// `SemaTypePredicates.cpp` (which asks what a type is) and
/// `SemaTypeEquality.cpp` (which asks whether two types are the same or
/// assignable).
///
/// The largest family of validations in the new grammar — the table
/// constraint checks of §4.1.3–§4.1.5 — will live in `TableConstraintChecker`,
/// not here. This file is for the small, cross-cutting rules that don't
/// have a natural home elsewhere.
///
/// ─── What is deliberately not here ────────────────────────────────────────
/// No trait conformance, no generic-argument checks, no struct
/// self-reference checks, no borrowed-context checks, no FFI checks, no
/// Arena/SIMD checks. Every one of those corresponded to a feature the
/// new grammar does not have.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Const-binding validation
// ─────────────────────────────────────────────────────────────────────────────

/// Validate a `const` binding's type.
///
/// A `const` binding — a `const` variable, a `const` parameter, a
/// `@readonly` column, a `const` field in a fixed-table row — must have a
/// *definite* type. The rule is one-directional: `let` accepts any type,
/// `const` narrows the set.
///
/// The types a `const` cannot have:
///
///   - `T?` — a nullable type has a value that can become `nil`, and a
///     `const` binding must be readable as `T` without a narrowing check.
///     If a program wants a nullable value that is never reassigned, it
///     declares `let x: T? = ...` and simply does not reassign it.
///
///   - `&T` — a row reference is nilable by construction (§5.2). A `&T`
///     that is "const" means "this binding may not be reassigned", but the
///     row it points to can be removed, at which point the reference reads
///     as `nil`. That is not a definite value, so `const x: &Person = ...`
///     is rejected.
///
///     The exception: a `const` *parameter* of type `&T` is legal, because
///     `const` on a parameter means "the callee cannot mutate through this
///     parameter", not "the value is definite". The caller's binding is
///     what has to be `let`/`const`; the parameter's own const-ness is a
///     different rule and is enforced at the call site, not here. This
///     function is called for the *binding* form, not the parameter form.
///
/// @param type   The declaration's resolved type.
/// @param name   The declared name, for the diagnostic.
/// @param kind   A short noun phrase ("variable", "field"), interpolated
///               into the message.
///
/// @return true if the type is acceptable for a `const` binding. On false,
///         a diagnostic has been emitted and the caller skips the rest of
///         the declaration's checks.
bool validateConstType(TypeAST* type, InternedString name,
                       const char* kind, SemaContext& ctx) {
    if (!type) return false;

    // A resolve failure upstream already reported its own diagnostic.
    // Do not pile on.
    if (type->isa<UnknownTypeAST>()) return true;

    // ─── Nullable: no ──────────────────────────────────────────────────
    if (isNullableType(type)) {
        ctx.diagnostics.error(DiagCode::Mut_ConstAssignment, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' cannot have a nullable type (",
                              typeToString(type, ctx.pool), ")");
        ctx.diagnostics.note(type,
                             "A const binding must have a definite value. "
                             "Use 'let' if the value may be nil, or "
                             "narrow it with '?\?' to a non-nil value first.");
        return false;
    }

    // ─── Row reference: no ─────────────────────────────────────────────
    if (isRowRefType(type)) {
        ctx.diagnostics.error(DiagCode::Mut_ConstAssignment, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' cannot have a row-reference type (",
                              typeToString(type, ctx.pool), ")");
        ctx.diagnostics.note(type,
                             "A row reference is inherently nilable — the "
                             "referenced row may be removed. Use 'let' for "
                             "a row reference.");
        return false;
    }

    return true;
}

} // namespace lucid::sema