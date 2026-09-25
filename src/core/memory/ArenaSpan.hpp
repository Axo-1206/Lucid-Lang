/// @file ArenaSpan.hpp
/// @brief A non-owning, const view of a contiguous run of elements.
///
/// ArenaSpan replaces std::vector for storing child lists in the AST. It
/// holds a pointer and a count; it owns nothing, allocates nothing, and
/// has a trivial destructor.
///
/// ─── Why not std::vector ──────────────────────────────────────────────────
///   • Locality. All elements live in the same arena block, so a walk of
///     a node's children is a walk of contiguous memory.
///   • No per-container allocation. A std::vector allocates its own
///     buffer, breaking the arena's contiguity.
///   • Trivial destruction. The arena reclaims everything at once; the
///     span does not destruct what it views.
///   • Immutability. A span is const once built. Nothing downstream of
///     the parser can mutate a node's children.
///
/// ─── Building ─────────────────────────────────────────────────────────────
/// Spans are built through ASTArena::SpanBuilder, which accumulates
/// elements in a temporary std::vector and, on `build()`, copies them
/// into an arena block and returns a span over it. A span can also be
/// constructed directly from an existing (pointer, size) pair, which is
/// what the builder does internally and what a caller with an existing
/// arena-allocated buffer does.
///
/// ─── Element type ─────────────────────────────────────────────────────────
/// The element type is usually a pointer (`ExprAST*`, `ParamAST*`), but
/// nothing in this class requires that. A span of value types works as
/// long as the elements are constructible and copyable.

#pragma once

#include <cstddef>

namespace lucid {

template <typename T>
class ArenaSpan {
    const T* data_ = nullptr;
    size_t   size_ = 0;

public:
    // ─── Construction ─────────────────────────────────────────────────

    ArenaSpan() = default;

    ArenaSpan(const T* data, size_t size) noexcept
        : data_(data), size_(size) {}

    ArenaSpan(T* data, size_t size) noexcept
        : data_(data), size_(size) {}

    ArenaSpan(const ArenaSpan&)            = default;
    ArenaSpan& operator=(const ArenaSpan&) = default;
    ArenaSpan(ArenaSpan&&)                 = default;
    ArenaSpan& operator=(ArenaSpan&&)      = default;

    // ─── Size ─────────────────────────────────────────────────────────

    const T* data() const noexcept { return data_; }
    size_t   size() const noexcept { return size_; }
    bool     empty() const noexcept { return size_ == 0; }

    // ─── Element access ───────────────────────────────────────────────

    /// Element access. No bounds check; the caller is responsible.
    const T& operator[](size_t idx) const noexcept { return data_[idx]; }

    /// First element. Undefined if empty.
    const T& front() const noexcept { return data_[0]; }

    /// Last element. Undefined if empty.
    const T& back() const noexcept { return data_[size_ - 1]; }

    // ─── Iteration ────────────────────────────────────────────────────

    const T* begin() const noexcept { return data_; }
    const T* end()   const noexcept { return data_ + size_; }

    // ─── Utility ──────────────────────────────────────────────────────

    /// Linear search for a value. Use sparingly.
    bool contains(const T& value) const {
        for (size_t i = 0; i < size_; ++i) {
            if (data_[i] == value) return true;
        }
        return false;
    }

    /// A sub-span starting at `offset`, up to `count` elements.
    /// `count` is clamped to the end of the span.
    ArenaSpan<T> subspan(size_t offset, size_t count) const noexcept {
        if (offset >= size_) return {};
        if (offset + count > size_) count = size_ - offset;
        return ArenaSpan<T>{data_ + offset, count};
    }

    /// A sub-span from `offset` to the end.
    ArenaSpan<T> slice(size_t offset) const noexcept {
        if (offset >= size_) return {};
        return ArenaSpan<T>{data_ + offset, size_ - offset};
    }
};

/// Deduction guide: `ArenaSpan{ptr, n}` deduces T from the pointer type.
template <typename T>
ArenaSpan(const T*, size_t) -> ArenaSpan<T>;

} // namespace lucid