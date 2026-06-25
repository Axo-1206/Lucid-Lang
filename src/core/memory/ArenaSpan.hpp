/**
 * @file ArenaSpan.hpp
 * @brief A non-owning, arena-allocated contiguous view of elements (read‑only).
 *
 * ArenaSpan provides a lightweight, immutable view into a contiguous block of
 * arena-allocated memory. It replaces `std::vector` for storing child lists
 * in the AST, offering better cache locality and no per‑element heap overhead.
 *
 * ## Why ArenaSpan instead of std::vector?
 *
 * - **Memory locality**: All elements are stored in the same arena block,
 *   improving cache behaviour during AST traversal.
 * - **No per‑vector heap allocation**: `std::vector` allocates its own
 *   buffer on the heap, breaking arena contiguity.
 * - **Trivial destructor**: No need to destruct each element – the arena
 *   reclaims everything at once.
 * - **Immutability**: Once built, the span is read‑only, preventing
 *   accidental modification of AST data after construction.
 *
 * ## Usage Example
 *
 * @code
 *   ASTArena arena;
 *   auto builder = arena.makeBuilder<ExprPtr>();
 *   builder.push_back(parseExpr());
 *   builder.push_back(parseExpr());
 *   ArenaSpan<ExprPtr> elements = builder.build();
 *
 *   for (const ExprPtr& expr : elements) {
 *       // read‑only access
 *   }
 * @endcode
 *
 * @tparam T The type of elements stored in the span. Typically a pointer
 *           type (e.g., `ExprPtr`, `ParamPtr`) or a trivial value type.
 *
 * @note This class is intentionally minimal – it provides no modifying
 *       operations. Use `ASTArena::SpanBuilder` to build spans incrementally
 *       during parsing.
 */
#pragma once
#include <cstddef>

template <typename T>
class ArenaSpan {
    const T* data_ = nullptr;   ///< Pointer to the first element (read‑only)
    size_t size_ = 0;           ///< Number of elements in the span

public:
    // ─────────────────────────────────────────────────────────────────────────
    // Constructors
    // ─────────────────────────────────────────────────────────────────────────

    /// Default constructor – creates an empty span.
    ArenaSpan() = default;

    /**
     * @brief Construct a span from a pointer and a size.
     * @param data Pointer to the first element (must be valid for at least `size` elements).
     * @param size Number of elements.
     */
    ArenaSpan(const T* data, size_t size) : data_(data), size_(size) {}

    /**
     * @brief Construct a span from a mutable pointer (converts to const).
     * @param data Mutable pointer to the first element.
     * @param size Number of elements.
     */
    ArenaSpan(T* data, size_t size) : data_(data), size_(size) {}

    // Copy and move are defaulted – trivial because we hold only a pointer and size.
    ArenaSpan(const ArenaSpan&) = default;
    ArenaSpan& operator=(const ArenaSpan&) = default;
    ArenaSpan(ArenaSpan&&) = default;
    ArenaSpan& operator=(ArenaSpan&&) = default;

    // ─────────────────────────────────────────────────────────────────────────
    // Accessors
    // ─────────────────────────────────────────────────────────────────────────

    /// Returns a const pointer to the underlying data.
    const T* data() const { return data_; }

    /// Returns the number of elements in the span.
    size_t size() const { return size_; }

    /// Returns true if the span is empty.
    bool empty() const { return size_ == 0; }

    // ─────────────────────────────────────────────────────────────────────────
    // Element access (read‑only)
    // ─────────────────────────────────────────────────────────────────────────

    /// Const element access (no bounds checking).
    const T& operator[](size_t idx) const { return data_[idx]; }

    /// Const reference to the first element (undefined if empty).
    const T& front() const { return data_[0]; }

    /// Const reference to the last element (undefined if empty).
    const T& back() const { return data_[size_ - 1]; }

    // ─────────────────────────────────────────────────────────────────────────
    // Iterators (const only)
    // ─────────────────────────────────────────────────────────────────────────

    /// Const iterator to the beginning.
    const T* begin() const { return data_; }

    /// Const iterator to the end.
    const T* end() const { return data_ + size_; }

    // ─────────────────────────────────────────────────────────────────────────
    // Utility methods
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Checks if the span contains a value equal to `value`.
     * @param value The value to search for.
     * @return true if found, false otherwise.
     * @note Linear search – use sparingly.
     */
    bool contains(const T& value) const {
        for (size_t i = 0; i < size_; ++i) {
            if (data_[i] == value) return true;
        }
        return false;
    }

    /**
     * @brief Creates a subspan starting at `offset` with `count` elements.
     * @param offset Starting index (must be <= size()).
     * @param count Number of elements (clamped to the end of the span).
     * @return A new ArenaSpan covering the subrange.
     */
    ArenaSpan<T> subspan(size_t offset, size_t count) const {
        if (offset >= size_) return {};
        if (offset + count > size_) count = size_ - offset;
        return ArenaSpan<T>(data_ + offset, count);
    }

    /**
     * @brief Creates a subspan from `offset` to the end.
     * @param offset Starting index (must be <= size()).
     * @return A new ArenaSpan covering the range [offset, size()).
     */
    ArenaSpan<T> slice(size_t offset) const {
        if (offset >= size_) return {};
        return ArenaSpan<T>(data_ + offset, size_ - offset);
    }
};

/**
 * @brief Deduction guide for creating an ArenaSpan from a pointer and size.
 *
 * Allows the compiler to deduce `T` from both mutable and const pointers.
 */
template <typename T>
ArenaSpan(const T*, size_t) -> ArenaSpan<T>;