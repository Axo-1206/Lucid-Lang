/// @file core/Array.hpp

#pragma once

#include <cstddef>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ArrayKind
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The two array shapes.
///
/// - `Dynamic` — `[T]`. Grows and shrinks via `.ADD`/`.REMOVE`. Owns its
///   backing buffer.
/// - `Fixed`   — `[N, T]`. Compile-time length. Inline storage.
///
/// A slice (`[_]T`) existed in the old grammar; it is removed. A
/// non-owning view over an array is expressed by passing the array
/// itself, and the language does not distinguish view from owner at the
/// type level.
enum class ArrayKind : uint8_t {
    Dynamic,  // [T]
    Fixed,    // [N, T]
};
