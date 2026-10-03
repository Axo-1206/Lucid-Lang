/// @file core/ast/ResourceKind.hpp
/// @brief The ResourceKind classification of a Lucid type.
///
/// ─── What this file is ────────────────────────────────────────────────────
///   1. `enum class ResourceKind` — the four-way classification of a type
///      by what heap resource a value of that type owns.
///   2. `classifyResourceKind(TypeAST*)` — the single function that
///      produces a ResourceKind from a type.
///   3. Small `constexpr` helpers for the enum.
///
/// ─── What this file is not ────────────────────────────────────────────────
/// It is not the ownership model. It answers "does a value of this type
/// own anything, and what kind?" — a fact about the type, independent of
/// any codegen run. The ownership model (in codegen's `Ownership`) decides
/// what to *do* with a value of a given kind: copy, retain, free, no-op.
///
/// It is not the cached per-declaration answer. `ValueDeclAST::resourceKind`
/// caches the classifier's result for a declaration. This file provides
/// the classifier that produces that result. The cached field is a cache,
/// not a second source of truth: if the field and the classifier ever
/// disagree, the classifier wins and the cache is a bug.
///
/// ─── string performance ──────────────────────────────────────────────────
/// `string` is classified as `OwnedBuffer`: a value owns its bytes, a copy
/// allocates, a drop frees. This is the design's value-semantics choice,
/// and it has a cost — every read of a string cell, every pass of a string
/// to a function, allocates.
///
/// Two performance-oriented alternatives exist:
///
///   - A `string_view` type (a non-owning view of a string's bytes). It
///     would need lifetime rules the design does not currently have.
///   - Copy-on-write strings (a shared buffer with a refcount; a copy is
///     a refcount increment, a mutation copies if shared). It is a
///     runtime change with no grammar impact, so it can be added later
///     without changing user code.
///
/// Neither is required for correctness. Programs that need to avoid
/// string copies use integer IDs or table row references in hot paths.
/// This is the design's expected pattern for a data-oriented scripting
/// language and is documented in the grammar's §5.5.
///
/// Because both alternatives are runtime optimizations that preserve the
/// `OwnedBuffer` classification, the enum and the classifier do not
/// change if either is adopted later. A `string` is always an
/// `OwnedBuffer` in kind; only its runtime representation is affected.

#pragma once

#include <cstdint>

struct TypeAST;

// ─────────────────────────────────────────────────────────────────────────────
// ResourceKind
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── What each kind means ─────────────────────────────────────────────────
//
//   None        — owns nothing. Copy is a bit copy; drop is a no-op.
//                 Primitives except string, references (both table and
//                 row), function values, unknown / error-recovery types.
//
//   Refcounted  — a host handle. The value names a host-owned object
//                 whose lifetime is managed by the host's refcount. Copy
//                 retains; drop releases. Every host-backed table
//                 (`TABLE X = host("...")`) is Refcounted by convention
//                 (grammar §4.1.2).
//
//   OwnedBuffer — a value that owns a heap-allocated backing buffer.
//                 Two cases: `string` (owns its bytes) and `[T]` (owns
//                 its element buffer). Copy deep-copies; drop frees.
//
//   Aggregate   — a value that owns its elements inline, with at least
//                 one element being a resource. The single case is a
//                 fixed array `[N, T]` where `T` is a resource. Copy is
//                 per-element; drop is per-element. Codegen generates
//                 the glue by walking the element type.
//
// ─── Why Aggregate still exists ───────────────────────────────────────────
// A fixed array of resources (`[3, string]`) is a value whose copy and
// drop are not simple: they walk the elements. A dynamic array or a
// string is a single buffer, so a copy or a drop is one operation on
// the buffer. The two shapes need different handling at every use site,
// and the classifier distinguishes them so CodeGen knows which path to
// emit.
//
// ─── Nullability does not change the kind ─────────────────────────────────
// A `T?` where `T` owns a resource is still that resource kind. A nil
// value has nothing to copy or drop, but the *kind* — what the value's
// type would own if present — is unchanged. So `string?` is
// `OwnedBuffer`, `[int]?` is `OwnedBuffer`, `[3, string]?` is
// `Aggregate`, and `SpriteRef?` is `Refcounted`.

enum class ResourceKind : uint8_t {
    None,
    Refcounted,
    OwnedBuffer,
    Aggregate,
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

constexpr bool isResourceKind(ResourceKind kind) {
    return kind != ResourceKind::None;
}

constexpr const char* resourceKindName(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::None:        return "None";
        case ResourceKind::Refcounted:  return "Refcounted";
        case ResourceKind::OwnedBuffer: return "OwnedBuffer";
        case ResourceKind::Aggregate:   return "Aggregate";
    }
    return "<unknown>";
}

// ─────────────────────────────────────────────────────────────────────────────
// The classifier
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Classify a Lucid type by what heap resource a value of that
///        type owns.
///
/// The classifier is a pure function of the type. It answers a single
/// question: when a binding of this type is copied or dropped, does the
/// operation need to do anything beyond a bit copy or a no-op?
///
/// The classifier is authoritative. `ValueDeclAST::resourceKind` caches
/// its output for a declaration; the cache is written once by Sema and
/// read by codegen. If the cache ever disagrees with a fresh classifier
/// call on the same type, the cache is a bug.
///
/// ─── Dispatch table ───────────────────────────────────────────────────────
///   NullableTypeAST(T)         →  classify(T)   (same kind as inner)
///   PrimitiveTypeAST(String)   →  OwnedBuffer
///   PrimitiveTypeAST(other)    →  None
///   FunctionTypeAST            →  None
///   RowRefTypeAST              →  None
///   ArrayTypeAST(Dynamic)      →  OwnedBuffer
///   ArrayTypeAST(Fixed)        →  Aggregate (if element is a resource)
///                              →  None otherwise
///   NamedTypeAST(host-backed)  →  Refcounted
///   NamedTypeAST(table)        →  None  (a reference; the sheet owns the rows)
///   UnknownTypeAST             →  None
///   nullptr                    →  None
///
/// ─── A note on tables ─────────────────────────────────────────────────────
/// A table is a reference type (grammar §5.1). A `Person` value is a
/// pointer to the `Person` sheet, not the sheet itself. Copying the value
/// copies the pointer; the sheet's rows are not copied and their
/// ownership is unchanged. The classifier therefore returns `None` for a
/// table reference, regardless of what resources the table's columns hold.
///
/// The resources owned by a table's *cells* are the responsibility of the
/// table's storage management, not of any binding whose type is `Person`.
///
/// ─── A note on host-backed tables ─────────────────────────────────────────
/// A host-backed table's value is an opaque handle managed by the host.
/// The convention (grammar §4.1.2) is that the language retains on copy
/// and releases on drop. The classifier returns `Refcounted` for these.
///
/// ─── A note on `NullableTypeAST` ──────────────────────────────────────────
/// The classifier recurses into `NullableTypeAST::inner`. A nil value of
/// a resource-typed binding has nothing to copy or drop, but the
/// binding's *kind* — the shape of what it would own if non-nil — is the
/// same as the inner type's kind. Codegen handles the nil case at use
/// sites; the classifier does not need a distinct kind for it.
///
/// ─── Null input ───────────────────────────────────────────────────────────
/// A null `TypeAST*` returns `ResourceKind::None`. This lets callers treat
/// a missing type as "owns nothing" without a separate null check.
///
/// ─── Preconditions ────────────────────────────────────────────────────────
/// For a `NamedTypeAST`, the classifier reads `resolvedDecl`. It returns
/// `None` if `resolvedDecl` is null, so a caller may invoke it before Sema
/// has resolved the type without crashing. The answer is only meaningful
/// after resolution.
ResourceKind classifyResourceKind(TypeAST* type);