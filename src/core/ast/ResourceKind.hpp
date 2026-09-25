/// @file core/ast/ResourceKind.hpp
/// @brief The ResourceKind classification of a Lucid type.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// Two things, plus a set of small helpers:
///
///   1. `enum class ResourceKind` — the six-way classification of a type
///      by the resource it owns. Used by CodeGen at every allocation and
///      free site, by Sema during Phase 1 to populate
///      `ValueDeclAST::resourceKind`, and by capture analysis to populate
///      `CapturedVariable::resourceKind`.
///
///   2. `classifyResourceKind(TypeAST*)` — the single function that
///      produces a `ResourceKind` from a type. A pure function of the
///      type; no state, no dependencies beyond the AST.
///
///   3. Small `constexpr` helpers for the enum: `isResourceKind` and
///      `resourceKindName`.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the ownership model. `Ownership::drop` and
/// `Ownership::intoOwned` in `codegen/ownership/Ownership.cpp` decide
/// *what to do* with a value of a given kind (retain, deep-copy, free,
/// no-op). This file decides *what kind* the value is. The decision is a
/// fact about the type; the action is a fact about codegen's behaviour.
///
/// It is NOT the cached per-declaration answer. `ValueDeclAST::resourceKind`
/// stores the classifier's result for a declaration, populated by Sema in
/// Phase 1. This file provides the classifier that produces that result,
/// and the same function is used by CodeGen at sites that don't have a
/// declaration (a struct field, a temporary, an expression's resolved
/// type).
///
/// ─── Why the Classifier Is a Free Function ────────────────────────────────
/// The classifier takes a `TypeAST*` and returns a value. It could be a
/// method on `TypeAST` (`type->resourceKind()`), but the method body would
/// need to downcast to the concrete subclasses (`FuncTypeAST`,
/// `PrimitiveTypeAST`, ...), and those subclass definitions live in
/// `TypeAST.hpp` *after* `TypeAST` itself. A method on `TypeAST` would
/// therefore be unable to see the subclass definitions.
///
/// A free function in a separate translation unit sidesteps the ordering
/// problem: `ResourceKind.cpp` includes the full `TypeAST.hpp` (with all
/// subclasses visible) and does the downcasts.
///
/// ─── Sema and CodeGen Share This Function ─────────────────────────────────
/// Before this file existed, the classifier was a member of
/// `SemaContext` (`SemaContext::classifyResourceKind`), called only from
/// Sema. Codegen's `Ownership` and `DropGlue` reached Sema through
/// nothing, so they either reimplemented the classifier or failed to
/// compile.
///
/// This file is the single implementation. Sema's call sites call
/// `classifyResourceKind` directly. Codegen's call sites do the same.
/// The two sides can't drift, because there's only one function.

#pragma once

#include <cstdint>

struct TypeAST;

// ─────────────────────────────────────────────────────────────────────────────
// ResourceKind — what kind of heap resource does a type own?
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── What Each Kind Means ─────────────────────────────────────────────────
//
//   None        — owns nothing. Copy is a bit copy. Drop is a no-op.
//                 Primitives (except strings), references, enum variants,
//                 and (until Phase 4) aggregates whose resource-ness
//                 isn't yet computed.
//
//   Refcounted  — a function value's environment. Every function value is
//                 a fat pointer `{ code, env }`; the environment is
//                 refcounted when the function captures, and null
//                 otherwise. Copy retains. Drop releases (a null
//                 environment is a no-op). A move zeroes the source.
//
//   OwnedBuffer — a string or a dynamic array. The value owns its buffer
//                 outright. Copy deep-copies. Drop frees the buffer,
//                 unless the buffer is empty (a static or empty string).
//
//   Arena       — a bump allocator. Sema rejects copy. Drop frees the base
//                 pointer. A move zeroes the source. Under the new
//                 grammar, Arena is a core-script type (a `NamedTypeAST`
//                 resolving to a `HostTypeDeclAST`), not a boot-level
//                 AST node; the classifier recognizes it by resolving
//                 the named type.
//
//   Handle      — a `Deferred<T>`. Linear: Sema rejects copy, and the
//                 only legal drop is consumption by `await` or `cancel`.
//                 Reaching scope exit with a live handle is a Sema
//                 error, so CodeGen's drop is a no-op that exists only
//                 so the switch is exhaustive. Like Arena, Deferred is
//                 a `NamedTypeAST` resolving to a `HostTypeDeclAST`.
//
//   Aggregate   — a struct, tuple, `T?`, `T!`, or fixed array that
//                 contains at least one resource. Copy and drop are
//                 per-field, generated lazily by `Ownership` as
//                 `__copy_<type>` and `__drop_<type>`. Sema's job is only
//                 to answer "yes, this owns something"; CodeGen derives
//                 the glue by walking the type.
//
//                 Phase 4 implements the walk. Until then, the classifier
//                 returns `None` for every type that would be an
//                 aggregate, because no compilable program can construct
//                 one yet: the resource kinds that would make a type an
//                 aggregate are handled above, and `Handle` is linear and
//                 rejected from aggregates by Sema.
//
// ─── Why Aggregate Is Not "the Field Kinds, OR'd Together" ────────────────
// Because "does this struct own anything" and "what is the glue for this
// struct" are different questions with different answers. A struct with a
// `Refcounted` field and a struct with two `Refcounted` fields are both
// `Aggregate`; their glue differs. CodeGen's drop-glue generator walks the
// field list to produce the right sequence, and it needs to know only
// that it should walk — the per-field kinds it reads from the field
// declarations themselves.

enum class ResourceKind : uint8_t {
    None,          // owns nothing
    Refcounted,    // a function value's refcounted environment
    OwnedBuffer,   // string or dynamic array
    Arena,         // arena memory pool
    Handle,        // Deferred<T>, linear, consumed by await/cancel
    Aggregate,     // struct/tuple/T?/T!/fixed-array containing a resource
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief True if the kind owns a heap resource.
///
/// `None` is the only kind that owns nothing. Every other kind has at
/// least one resource whose lifetime the binding is responsible for.
///
/// This is the boolean form of the classifier's answer. Code that only
/// needs "does this binding need cleanup?" can call this instead of
/// comparing against `None` directly. It's `constexpr` so it can be used
/// in `static_assert`s and constant expressions.
constexpr bool isResourceKind(ResourceKind kind) {
    return kind != ResourceKind::None;
}

/// @brief Human-readable name for a kind, for diagnostics and tracing.
///
/// Returns a string literal — no allocation. The names match the enum
/// values, so a diagnostic that prints the kind is self-documenting.
constexpr const char* resourceKindName(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::None:        return "None";
        case ResourceKind::Refcounted:  return "Refcounted";
        case ResourceKind::OwnedBuffer: return "OwnedBuffer";
        case ResourceKind::Arena:       return "Arena";
        case ResourceKind::Handle:      return "Handle";
        case ResourceKind::Aggregate:   return "Aggregate";
    }
    return "<unknown>";
}

// ─────────────────────────────────────────────────────────────────────────────
// The classifier
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Classify a Lucid type into its resource kind.
///
/// The single source of truth for "does this type own a heap resource, and
/// if so, which kind?" Answers the same question Sema answers during
/// Phase 1 when it populates `ValueDeclAST::resourceKind`, but as a pure
/// function of the type rather than a method on `SemaContext`.
///
/// ─── Dispatch Table ──────────────────────────────────────────────────────
///   FuncTypeAST                    →  Refcounted
///   PrimitiveTypeAST(String)       →  OwnedBuffer
///   ArrayTypeAST(isDynamic)        →  OwnedBuffer
///   NamedTypeAST(Deferred)         →  Handle
///   NamedTypeAST(Arena)            →  Arena
///   (everything else)              →  None
///
/// The `NamedTypeAST` cases are resolved by consulting the named type's
/// `resolvedDecl`. If the declaration is a core-script `HostTypeDeclAST`
/// whose `targetName` is a known kind ("LucidDeferred" or "LucidArena"),
/// the classifier returns the corresponding kind. This is done here
/// rather than at each use site so that the special-casing is confined to
/// one place.
///
/// The `Aggregate` case is a Phase 4 stub. Until then, `None` is correct:
/// no program that compiles today can construct a resource-owning
/// aggregate, because the resource kinds that would make it one are
/// handled above, and `Handle` is linear and rejected from aggregates by
/// Sema.
///
/// ─── Null Input ──────────────────────────────────────────────────────────
/// A null `TypeAST*` returns `ResourceKind::None`. This lets callers treat
/// a missing type as "owns nothing" without a separate null check at
/// every call site. The alternative — asserting non-null — would make
/// every caller defensive against a case that is handled uniformly by
/// returning the safe answer.
///
/// Defined in `ResourceKind.cpp`, which includes the full `TypeAST.hpp`
/// so the classifier can downcast to the concrete type subclasses.
ResourceKind classifyResourceKind(TypeAST* type);