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
/// the classifier that produces that result.

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
//                 (`TABLE X = host("...")`) is Refcounted by convention.
//
//   OwnedBuffer — a value that owns a heap-allocated backing buffer.
//                 Two cases: `string` (owns its bytes) and `[T]` (owns
//                 its element buffer). Copy deep-copies; drop frees.
//
//   Aggregate   — a value that owns its elements inline, with at least
//                 one element being a resource. The single case is a
//                 fixed array `[N]T` where `T` is a resource. Copy is
//                 per-element; drop is per-element. Codegen generates
//                 the glue by walking the element type.
//
// ─── Why Aggregate still exists ───────────────────────────────────────────
// A fixed array of resources (`[3]string`) is a value whose copy and drop
// are not simple: they walk the elements. A dynamic array or a string is
// a single buffer, so a copy or a drop is one operation on the buffer.
// The two shapes need different handling at every use site, and the
// classifier distinguishes them so CodeGen knows which path to emit.

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
/// ─── Dispatch table ───────────────────────────────────────────────────────
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
/// ─── Null input ───────────────────────────────────────────────────────────
/// A null `TypeAST*` returns `ResourceKind::None`. This lets callers treat
/// a missing type as "owns nothing" without a separate null check.
ResourceKind classifyResourceKind(TypeAST* type);