/**
 * @file SemaType.hpp
 *
 * @responsibility The type subsystem's surface: resolving a syntactic
 *                 type to a semantic one, asking what kind of type a
 *                 node is, comparing two types, and validating a
 *                 declaration's type against the rules.
 *
 * ─── Design: this header is for other Sema files ──────────────────────────
 * The callers of this file are the other Sema translation units:
 * `SemaDecl.cpp`, `SemaStmt.cpp`, `SemaExpr.cpp`, `AttributeValidator.cpp`,
 * `ConstEvaluator.cpp`, and the table/sequence checkers. Each of them
 * needs some subset of "resolve this type", "is this a primitive", "are
 * these two types the same", "is this assignable".
 *
 * The caller is not the pipeline. The pipeline uses `Sema.hpp`. The
 * split matters: `Sema.hpp` changes when the driver wants a new pass
 * shape; `SemaType.hpp` changes when the type system grows a new
 * predicate or a new comparison rule. Two different reasons to change.
 *
 * ─── Design: the per-form resolvers are internal ──────────────────────────
 * `resolvePrimitiveType`, `resolveNamedType`, `resolveArrayType`, and the
 * others are cases inside `resolveType`'s dispatch. No caller outside
 * `SemaType.cpp` needs them. They are `static` in the `.cpp`; only
 * `resolveType` is public.
 *
 * ─── Design: what is deliberately not here ────────────────────────────────
 * Expression-level rules — is this expression an lvalue, is this
 * expression a `const_expr`, is this expression assignable *in this
 * context* — belong in `SemaRules.hpp`. They take an `ExprAST*`, walk
 * it, and emit diagnostics. This file is about `TypeAST*` and nothing
 * else.
 *
 * Attribute argument validation (`validateStringArg`, `validateIntArg`,
 * `validateDottedNameArg`) lives in `ArgTypeValidators.hpp`. It is
 * type-adjacent but a different concern.
 *
 * ─── Design: everything takes a canonicalized type ────────────────────────
 * The predicates and `typesEqual` assume their arguments came from
 * `resolveType` or from a `SemaContext::get*Type` accessor. A
 * non-canonical `TypeAST*` — one built by hand without going through
 * the cache — will still work, but the pointer-equality fast path in
 * `typesEqual` will miss more often than it should. Do not build types
 * by hand; go through the accessors.
 */

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <cstdint>

namespace lucid::sema {

struct SemaContext;   // forward declaration; defined in SemaContext.hpp

// ─────────────────────────────────────────────────────────────────────────────
// Type resolution
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a syntactic type to a semantic one.
///
/// The input is a `TypeAST*` as the parser produced it. The output is a
/// canonicalized `TypeAST*` — the same pointer for structurally identical
/// types, obtained from the context's `TypeCache` — with every
/// `NamedTypeAST::resolvedDecl` filled in.
///
/// On error (an undefined table name, a `&` applied to a non-table type,
/// a `T?` applied to a bare table), emits a diagnostic and returns the
/// unknown-type singleton. The caller should test for `UnknownTypeAST`
/// before using the result.
///
/// Resolves recursively: an `ArrayTypeAST` has its element resolved; a
/// `RowRefTypeAST` has its inner resolved; a `FunctionTypeAST` has each
/// parameter type and its return type resolved.
TypeAST* resolveType(TypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// Every predicate is a pure function of the node's shape. None of them
// consults the context; none of them emits a diagnostic. A predicate on
// the unknown-type singleton returns false for every kind — callers that
// need to distinguish "unknown" from "some other type" test for
// `UnknownTypeAST` directly.

bool isBoolType     (TypeAST* type);
bool isCharType     (TypeAST* type);
bool isStringType   (TypeAST* type);
bool isUnitType     (TypeAST* type);

bool isIntegerType  (TypeAST* type);   // any signed or unsigned width
bool isFloatType    (TypeAST* type);   // float32 or float64
bool isNumericType  (TypeAST* type);   // isIntegerType || isFloatType

bool isPrimitiveType(TypeAST* type);   // any PrimitiveTypeAST
bool isArrayType    (TypeAST* type);   // dynamic or fixed
bool isRowRefType   (TypeAST* type);   // &T
bool isFunctionType (TypeAST* type);   // (T, U) -> R
bool isNamedType    (TypeAST* type);   // a table or host-backed table name
bool isNullableType (TypeAST* type);   // T?

// ─────────────────────────────────────────────────────────────────────────────
// Type classification
// ─────────────────────────────────────────────────────────────────────────────
//
// These consult the context because they need to follow a `NamedTypeAST`
// to its `resolvedDecl`.

/// True if `type` names a table that was declared with columns
/// (`TABLE X { ... }`). False for a host-backed table.
bool isColumnedTableType(TypeAST* type, SemaContext& ctx);

/// True if `type` names a host-backed table (`TABLE X = host("...")`).
/// False for a columned table. This replaces the old design's
/// `isValidFFIType` — a host-backed table *is* the FFI-visible type.
bool isHostBackedTableType(TypeAST* type, SemaContext& ctx);

/// True if `type` names a table of any kind (columned or host-backed).
bool isTableType(TypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type equality and assignability
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Structural equality.
///
/// Fast path: `a == b` (pointer equality). Because `TypeCache`
/// canonicalizes, this hits whenever both types came through the cache.
///
/// Slow path: structural comparison. Handles the cases the cache cannot
/// reach:
///   - two `NamedTypeAST` nodes for the same source name but resolved to
///     different declarations (the `weapons.Item` vs. `consumables.Item`
///     case of §5) are *not* equal;
///   - two `FunctionTypeAST` nodes whose parameter lists were built
///     independently compare element-wise.
bool typesEqual(TypeAST* a, TypeAST* b);

/// @brief Can a value of type `source` be assigned to a location of type
///        `target`?
///
/// The rules, in order:
///
///   1. `typesEqual(target, source)` → true.
///   2. `target` is a nullable type `T?` and `source` is `T` → true.
///      (`T` widens to `T?`; the reverse does not hold — a `T?` is not
///      assignable to a `T` without narrowing.)
///   3. `source` is an untyped integer literal and `target` is an integer
///      primitive → true. This is the §5.8 literal-adaptation rule.
///   4. `source` is an untyped float literal and `target` is a float
///      primitive → true.
///   5. `source` is `nil` and `target` is a row-reference or nullable
///      type → true.
///   6. `source` is a function value (a named `FN` or a lambda) and
///      `target` is a function type with the same signature → true.
///
/// Everything else → false. In particular, `int + float` is a mismatch:
/// no implicit coercion between two already-typed values.
///
/// The `source`'s literal-ness is read from the expression, not the type
/// — this function only sees the type. Call sites that need rules 3 and
/// 4 must call the expression-level form in `SemaRules.hpp`, which knows
/// the literal kinds. `isAssignable` here covers rules 1, 2, 5, and 6;
/// the literal cases are handled by `resolveExprWithTarget` before it
/// calls this.
bool isAssignable(TypeAST* target, TypeAST* source, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Unwrapping
// ─────────────────────────────────────────────────────────────────────────────

/// @brief `T?` → `T`. Any other type → itself.
///
/// Used by the narrowing resolver and by `??`. Only `T?` is unwrapped;
/// `&T` is already nilable at the type level and is not touched.
TypeAST* unwrapNullable(TypeAST* type);

// ─────────────────────────────────────────────────────────────────────────────
// Primitive-name lookup
// ─────────────────────────────────────────────────────────────────────────────
//
// The parser recognizes primitive type names lexically and produces a
// `PrimitiveTypeAST` directly. These helpers exist for the two places
// that need to ask "is this identifier a primitive type name?" without
// going through the parser: the type resolver, when a `NamedTypeAST`
// turns out to name a primitive; and diagnostics, when reporting a
// primitive-kind name back to the user.

/// True if `name` spells one of the primitive type keywords (§2.2).
bool isPrimitiveTypeName(InternedString name, StringPool& pool);

/// The `PrimitiveKind` for a primitive type name. The name must be a
/// primitive type name; the caller has already checked with
/// `isPrimitiveTypeName`. Precondition: `isPrimitiveTypeName(name, pool)`.
PrimitiveKind primitiveKindFromName(InternedString name, StringPool& pool);

// ─────────────────────────────────────────────────────────────────────────────
// Numeric helpers
// ─────────────────────────────────────────────────────────────────────────────

/// The bit width of a numeric primitive (`int8` → 8, `float64` → 64).
/// Precondition: `isNumericType(type)`.
size_t getNumericBitWidth(TypeAST* type);

/// @brief The result type of a binary arithmetic or bitwise operation
///        on two numeric operands of different types.
///
/// The rule is the usual one: if either side is a float, the result is
/// a float; otherwise the result is the wider integer. Both sides must
/// be numeric — the caller has already checked with `isNumericType`.
///
/// The returned type is canonicalized through `ctx`, so a caller may
/// rely on pointer equality against a type from the cache.
TypeAST* getLargerNumericType(TypeAST* a, TypeAST* b, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Validation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate a declaration's type against the rules for a `const`.
///
/// A `const` binding, `const` parameter, `const` field, or `const`
/// column must have a definite type: not `T?`, not a row reference.
/// The rule is one-directional — `let` accepts any type, `const`
/// narrows the set.
///
/// @param type   The declaration's resolved type.
/// @param name   The declared name, for the diagnostic message.
/// @param kind   A short noun phrase ("variable", "parameter", "field"),
///               used in the message: "const <kind> '<name>' must have
///               a definite type".
///
/// @return true if the type is acceptable. On false, a diagnostic has
///         been emitted and the caller should skip the declaration's
///         remaining checks.
bool validateConstType(TypeAST* type, InternedString name,
                       const char* kind, SemaContext& ctx);

} // namespace lucid::sema