/**
 * @file ASTArena.hpp
 * @brief Arena allocator for AST nodes using raw pointers.
 *
 * All AST nodes are allocated from the arena and never freed individually.
 * The entire arena is destroyed at the end of the compilation unit.
 * Nodes are accessed via raw pointers, which are safe because the arena
 * owns the memory and never moves it.
 */

#pragma once

#include "ArenaSpan.hpp"
#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <initializer_list>

// ─────────────────────────────────────────────────────────────────────────────
// ASTArena – Bump-allocator for AST nodes returning raw pointers
// ─────────────────────────────────────────────────────────────────────────────

class ASTArena {
    // Private fields
    std::vector<std::unique_ptr<char[]>> blocks;
    char* currentBlock = nullptr;
    size_t currentOffset = 0;
    static constexpr size_t BLOCK_SIZE = 64 * 1024;

    char* allocRaw(size_t size, size_t align) {
        size_t padding = 0;
        if (currentBlock) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(currentBlock + currentOffset);
            padding = (align - (addr % align)) % align;
        }

        if (!currentBlock || currentOffset + padding + size > BLOCK_SIZE) {
            size_t allocSize = std::max(BLOCK_SIZE, size + align);
            blocks.push_back(std::make_unique<char[]>(allocSize));
            currentBlock = blocks.back().get();
            currentOffset = 0;

            uintptr_t addr = reinterpret_cast<uintptr_t>(currentBlock);
            padding = (align - (addr % align)) % align;
        }

        char* ptr = currentBlock + currentOffset + padding;
        currentOffset += padding + size;
        return ptr;
    }

    template<typename T>
    T* allocArrayRaw(size_t size) {
        if (size == 0) return nullptr;
        char* ptr = allocRaw(sizeof(T) * size, alignof(T));
        return reinterpret_cast<T*>(ptr);
    }

public:
    ASTArena() = default;
    ~ASTArena() = default;
    ASTArena(const ASTArena&) = delete;
    ASTArena& operator=(const ASTArena&) = delete;

    // ─────────────────────────────────────────────────────────────────────────
    // Single object allocation (returns raw pointer)
    // ─────────────────────────────────────────────────────────────────────────

    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        char* ptr = allocRaw(sizeof(T), alignof(T));
        return ::new(ptr) T(std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    T* make(Args&&... args) {
        return alloc<T>(std::forward<Args>(args)...);
    }

    // Convenience aliases
    template<typename T, typename... Args>
    T* makeExpr(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    T* makeStmt(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    T* makeDecl(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    T* makeType(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    T* makePattern(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    // ─────────────────────────────────────────────────────────────────────────
    // Array allocation
    // ─────────────────────────────────────────────────────────────────────────

    template<typename T>
    ArenaSpan<T> allocArray(size_t size) {
        if (size == 0) return {};
        T* arr = allocArrayRaw<T>(size);
        for (size_t i = 0; i < size; ++i) {
            ::new(&arr[i]) T();
        }
        return ArenaSpan<T>(arr, size);
    }

    template<typename T>
    ArenaSpan<T> allocArray(std::initializer_list<T> init) {
        if (init.size() == 0) return {};
        T* arr = allocArrayRaw<T>(init.size());
        size_t i = 0;
        for (const auto& val : init) {
            ::new(&arr[i]) T(val);
            ++i;
        }
        return ArenaSpan<T>(arr, init.size());
    }

    // ─────────────────────────────────────────────────────────────────────────
    // SpanBuilder (unchanged – works with any movable type, including raw pointers)
    // ─────────────────────────────────────────────────────────────────────────

    template<typename T>
    class SpanBuilder {
        ASTArena& arena;
        std::vector<T> temp;

    public:
        explicit SpanBuilder(ASTArena& a) : arena(a) {}

        void push_back(T&& value) { temp.push_back(std::move(value)); }
        void push_back(const T& value) { temp.push_back(value); }

        template<typename... Args>
        T& emplace_back(Args&&... args) {
            temp.emplace_back(std::forward<Args>(args)...);
            return temp.back();
        }

        ArenaSpan<T> build() {
            if (temp.empty()) return {};
            size_t size = temp.size();
            T* dst = arena.allocArrayRaw<T>(size);
            for (size_t i = 0; i < size; ++i) {
                dst[i] = std::move(temp[i]);
            }
            temp.clear();
            return ArenaSpan<T>(dst, size);
        }

        size_t size() const { return temp.size(); }
        bool empty() const { return temp.empty(); }
        void clear() { temp.clear(); }
        T& operator[](size_t idx) { return temp[idx]; }
        const T& operator[](size_t idx) const { return temp[idx]; }
        std::vector<T>& getTemp() { return temp; }
    };

    template<typename T>
    SpanBuilder<T> makeBuilder() {
        return SpanBuilder<T>(*this);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Convenience helpers for ArenaSpan construction
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Create an empty ArenaSpan.
    template<typename T>
    static ArenaSpan<T> emptySpan() {
        return ArenaSpan<T>();
    }

    /// @brief Create an ArenaSpan from a single element.
    template<typename T>
    ArenaSpan<T> makeSpan(const T& value) {
        auto builder = makeBuilder<T>();
        builder.push_back(value);
        return builder.build();
    }

    /// @brief Create an ArenaSpan from an initializer list.
    template<typename T>
    ArenaSpan<T> makeSpan(std::initializer_list<T> init) {
        if (init.size() == 0) return {};
        auto builder = makeBuilder<T>();
        for (const auto& item : init) {
            builder.push_back(item);
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan from a vector of items.
    template<typename T>
    ArenaSpan<T> makeSpan(const std::vector<T>& items) {
        if (items.empty()) return {};
        auto builder = makeBuilder<T>();
        for (const auto& item : items) {
            builder.push_back(item);
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan from a span of items (copies them).
    template<typename T>
    ArenaSpan<T> makeSpan(const ArenaSpan<T>& items) {
        if (items.empty()) return {};
        auto builder = makeBuilder<T>();
        for (const auto& item : items) {
            builder.push_back(item);
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan from a pointer and size.
    template<typename T>
    ArenaSpan<T> makeSpan(const T* data, size_t size) {
        if (size == 0 || !data) return {};
        auto builder = makeBuilder<T>();
        for (size_t i = 0; i < size; ++i) {
            builder.push_back(data[i]);
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan by transforming a range.
    /// 
    /// @tparam T The element type of the resulting span.
    /// @tparam Iter The iterator type.
    /// @tparam Fn The transformation function type (T(*)(const U&)).
    /// @param begin Start iterator.
    /// @param end End iterator.
    /// @param transform Transformation function.
    /// @return An ArenaSpan containing the transformed elements.
    template<typename T, typename Iter, typename Fn>
    ArenaSpan<T> makeSpan(Iter begin, Iter end, Fn transform) {
        if (begin == end) return {};
        auto builder = makeBuilder<T>();
        for (auto it = begin; it != end; ++it) {
            builder.push_back(transform(*it));
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan by transforming a vector.
    template<typename T, typename U, typename Fn>
    ArenaSpan<T> makeSpan(const std::vector<U>& items, Fn transform) {
        if (items.empty()) return {};
        auto builder = makeBuilder<T>();
        for (const auto& item : items) {
            builder.push_back(transform(item));
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan by transforming a span.
    template<typename T, typename U, typename Fn>
    ArenaSpan<T> makeSpan(const ArenaSpan<U>& items, Fn transform) {
        if (items.empty()) return {};
        auto builder = makeBuilder<T>();
        for (const auto& item : items) {
            builder.push_back(transform(item));
        }
        return builder.build();
    }

    /// @brief Create an ArenaSpan by mapping a range with a transform.
    /// 
    /// This is a convenience overload that forwards to the transform version.
    template<typename T, typename U, typename Fn>
    ArenaSpan<T> mapSpan(const ArenaSpan<U>& items, Fn transform) {
        return makeSpan<T>(items, transform);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Concatenation helpers
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Concatenate multiple spans into a single ArenaSpan.
    template<typename T>
    ArenaSpan<T> concatSpans(std::initializer_list<ArenaSpan<T>> spans) {
        size_t totalSize = 0;
        for (const auto& span : spans) {
            totalSize += span.size();
        }
        if (totalSize == 0) return {};

        auto builder = makeBuilder<T>();
        for (const auto& span : spans) {
            for (const auto& item : span) {
                builder.push_back(item);
            }
        }
        return builder.build();
    }

    /// @brief Concatenate two spans.
    template<typename T>
    ArenaSpan<T> concatSpans(const ArenaSpan<T>& a, const ArenaSpan<T>& b) {
        return concatSpans({a, b});
    }

    /// @brief Append one span to another and return the result.
    template<typename T>
    ArenaSpan<T> appendSpan(const ArenaSpan<T>& base, const T& item) {
        auto builder = makeBuilder<T>();
        for (const auto& existing : base) {
            builder.push_back(existing);
        }
        builder.push_back(item);
        return builder.build();
    }

    /// @brief Append multiple items to a span.
    template<typename T>
    ArenaSpan<T> appendSpan(const ArenaSpan<T>& base, std::initializer_list<T> items) {
        auto builder = makeBuilder<T>();
        for (const auto& existing : base) {
            builder.push_back(existing);
        }
        for (const auto& item : items) {
            builder.push_back(item);
        }
        return builder.build();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Utility to check if a span is valid
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Check if a span is valid (non-null and non-empty).
    template<typename T>
    static bool isValidSpan(const ArenaSpan<T>& span) {
        return !span.empty() && span.data() != nullptr;
    }

    /// @brief Get the size of a span, or 0 if null.
    template<typename T>
    static size_t spanSize(const ArenaSpan<T>& span) {
        return span.size();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Builder with reserve capacity
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief A builder that pre-reserves capacity for better performance.
    template<typename T>
    class ReservedSpanBuilder {
        ASTArena& arena;
        std::vector<T> temp;

    public:
        ReservedSpanBuilder(ASTArena& a, size_t reserveSize) : arena(a) {
            temp.reserve(reserveSize);
        }

        void push_back(T&& value) { temp.push_back(std::move(value)); }
        void push_back(const T& value) { temp.push_back(value); }

        template<typename... Args>
        T& emplace_back(Args&&... args) {
            temp.emplace_back(std::forward<Args>(args)...);
            return temp.back();
        }

        ArenaSpan<T> build() {
            if (temp.empty()) return {};
            size_t size = temp.size();
            T* dst = arena.allocArrayRaw<T>(size);
            for (size_t i = 0; i < size; ++i) {
                dst[i] = std::move(temp[i]);
            }
            temp.clear();
            return ArenaSpan<T>(dst, size);
        }

        size_t size() const { return temp.size(); }
        bool empty() const { return temp.empty(); }
        size_t capacity() const { return temp.capacity(); }
        void clear() { temp.clear(); }
        T& operator[](size_t idx) { return temp[idx]; }
        const T& operator[](size_t idx) const { return temp[idx]; }
        std::vector<T>& getTemp() { return temp; }
    };

    /// @brief Create a builder with pre-reserved capacity.
    template<typename T>
    ReservedSpanBuilder<T> makeBuilder(size_t reserveSize) {
        return ReservedSpanBuilder<T>(*this, reserveSize);
    }

    /// @brief Create a builder with pre-reserved capacity (overload for clarity).
    template<typename T>
    ReservedSpanBuilder<T> makeReservedBuilder(size_t reserveSize) {
        return ReservedSpanBuilder<T>(*this, reserveSize);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience function: Create an ArenaSpan from existing data without copying?
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create an ArenaSpan from existing data WITHOUT copying.
/// 
/// This is useful when you already have arena-allocated data and just want
/// to create a view. Note: The caller must ensure the data outlives the span.
template<typename T>
ArenaSpan<T> makeSpanFromRaw(T* data, size_t size) {
    return ArenaSpan<T>(data, size);
}

/// @brief Create an ArenaSpan from const data WITHOUT copying.
template<typename T>
ArenaSpan<T> makeSpanFromRaw(const T* data, size_t size) {
    return ArenaSpan<T>(data, size);
}