/// @file ASTArena.hpp
/// @brief Bump allocator for AST nodes.
///
/// Every AST node is allocated from the arena and is never freed
/// individually. The whole arena dies at once, when the session that owns
/// it dies. Nodes are referenced by raw pointer, which is safe because the
/// arena owns the memory and never moves it.
///
/// ─── Ownership ────────────────────────────────────────────────────────────
/// The arena is not global. A CompilationSession owns one, alongside the
/// StringPool and the diagnostic engine. The parser allocates through the
/// session's arena; Sema and the bytecode compiler read what the parser
/// built. Nothing frees a node; the whole arena is reclaimed when the
/// session is destroyed.
///
/// ─── Why raw pointers ─────────────────────────────────────────────────────
/// A bump allocator never moves what it has allocated, so a pointer into
/// the arena is stable for the arena's lifetime. There is nothing to
/// refcount and nothing to own; the arena owns everything.
///
/// ─── No reset ─────────────────────────────────────────────────────────────
/// There is deliberately no `reset()` method. Resetting the arena would
/// invalidate every pointer the AST holds, and the AST holds pointers
/// into the arena for as long as it lives. Reclaiming memory mid-session
/// is not a supported operation; the correct way to discard an AST is to
/// discard the session and construct a fresh one.

#pragma once

#include "ArenaSpan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

class ASTArena {
public:
    ASTArena() = default;
    ~ASTArena() = default;

    // The arena owns its blocks; copying or moving would leave every
    // pointer the AST holds pointing at the old arena.
    ASTArena(const ASTArena&)            = delete;
    ASTArena& operator=(const ASTArena&) = delete;
    ASTArena(ASTArena&&)                 = delete;
    ASTArena& operator=(ASTArena&&)      = delete;

    // ─── Single object allocation ─────────────────────────────────────

    /// Allocate one T, constructed from `args`.
    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        char* p = allocRaw(sizeof(T), alignof(T));
        return ::new (p) T(std::forward<Args>(args)...);
    }

    /// Alias for `alloc`, for call sites that read better with `make`.
    template <typename T, typename... Args>
    T* make(Args&&... args) {
        return alloc<T>(std::forward<Args>(args)...);
    }

    // Family-specific aliases. They are identical to `make`; they exist
    // so a call site can read `arena.makeExpr(...)` and mean it.

    template <typename T, typename... Args>
    T* makeExpr(Args&&... args) { return alloc<T>(std::forward<Args>(args)...); }

    template <typename T, typename... Args>
    T* makeStmt(Args&&... args) { return alloc<T>(std::forward<Args>(args)...); }

    template <typename T, typename... Args>
    T* makeDecl(Args&&... args) { return alloc<T>(std::forward<Args>(args)...); }

    template <typename T, typename... Args>
    T* makeType(Args&&... args) { return alloc<T>(std::forward<Args>(args)...); }

    // ─── Array allocation ─────────────────────────────────────────────
    //
    // There is no `allocArray<T>(count)`. A count-based allocation would
    // need to default-construct each element, and AST nodes do not have
    // default constructors. Spans of AST elements are built through
    // SpanBuilder, which constructs each element from a temporary.

    /// Allocate a fixed list of elements. Intended for pointer and trivial
    /// element types. For AST elements, prefer SpanBuilder.
    template <typename T>
    ArenaSpan<T> allocArray(std::initializer_list<T> init) {
        if (init.size() == 0) return {};
        T* arr = allocArrayRaw<T>(init.size());
        size_t i = 0;
        for (const auto& v : init) {
            ::new (&arr[i]) T(v);
            ++i;
        }
        return ArenaSpan<T>{arr, init.size()};
    }

    // ─── SpanBuilder ──────────────────────────────────────────────────
    //
    // The supported way to build an ArenaSpan. Elements are accumulated in
    // a temporary std::vector, then copied into an arena block. The
    // temporary is cleared after `build()`.

    template <typename T>
    class SpanBuilder {
        ASTArena&      arena_;
        std::vector<T> temp_;

    public:
        explicit SpanBuilder(ASTArena& a) : arena_(a) {}

        void push_back(T&& v)      { temp_.push_back(std::move(v)); }
        void push_back(const T& v) { temp_.push_back(v); }

        template <typename... Args>
        T& emplace_back(Args&&... args) {
            temp_.emplace_back(std::forward<Args>(args)...);
            return temp_.back();
        }

        ArenaSpan<T> build() {
            if (temp_.empty()) return {};
            const size_t n = temp_.size();
            T* dst = arena_.allocArrayRaw<T>(n);
            for (size_t i = 0; i < n; ++i) {
                // Placement-new, not assignment. The destination has not
                // been constructed; assigning into raw storage is UB for
                // any non-trivial type.
                ::new (&dst[i]) T(std::move(temp_[i]));
            }
            temp_.clear();
            return ArenaSpan<T>{dst, n};
        }

        size_t size()  const { return temp_.size(); }
        bool   empty() const { return temp_.empty(); }
        void   clear()       { temp_.clear(); }

        T&       operator[](size_t i)       { return temp_[i]; }
        const T& operator[](size_t i) const { return temp_[i]; }
    };

    template <typename T>
    SpanBuilder<T> makeBuilder() { return SpanBuilder<T>(*this); }

    // ─── ReservedSpanBuilder ──────────────────────────────────────────
    //
    // A builder that pre-reserves the temporary vector. Useful when the
    // element count is known ahead of time (a function's parameter list,
    // a struct's field list).

    template <typename T>
    class ReservedSpanBuilder {
        ASTArena&      arena_;
        std::vector<T> temp_;

    public:
        ReservedSpanBuilder(ASTArena& a, size_t reserve)
            : arena_(a) { temp_.reserve(reserve); }

        void push_back(T&& v)      { temp_.push_back(std::move(v)); }
        void push_back(const T& v) { temp_.push_back(v); }

        template <typename... Args>
        T& emplace_back(Args&&... args) {
            temp_.emplace_back(std::forward<Args>(args)...);
            return temp_.back();
        }

        ArenaSpan<T> build() {
            if (temp_.empty()) return {};
            const size_t n = temp_.size();
            T* dst = arena_.allocArrayRaw<T>(n);
            for (size_t i = 0; i < n; ++i) {
                ::new (&dst[i]) T(std::move(temp_[i]));
            }
            temp_.clear();
            return ArenaSpan<T>{dst, n};
        }

        size_t size()     const { return temp_.size(); }
        bool   empty()    const { return temp_.empty(); }
        size_t capacity() const { return temp_.capacity(); }
        void   clear()          { temp_.clear(); }

        T&       operator[](size_t i)       { return temp_[i]; }
        const T& operator[](size_t i) const { return temp_[i]; }
    };

    template <typename T>
    ReservedSpanBuilder<T> makeBuilder(size_t reserve) {
        return ReservedSpanBuilder<T>(*this, reserve);
    }

    template <typename T>
    ReservedSpanBuilder<T> makeReservedBuilder(size_t reserve) {
        return ReservedSpanBuilder<T>(*this, reserve);
    }

    // ─── Convenience span constructors ────────────────────────────────
    //
    // These all go through SpanBuilder, so the placement-new path is
    // shared. The element type must be copy-constructible (SpanBuilder
    // copies into the arena).

    /// An empty span.
    template <typename T>
    static ArenaSpan<T> emptySpan() { return {}; }

    /// A span of one element.
    template <typename T>
    ArenaSpan<T> makeSpan(const T& v) {
        auto b = makeBuilder<T>();
        b.push_back(v);
        return b.build();
    }

    /// A span from an initializer list.
    template <typename T>
    ArenaSpan<T> makeSpan(std::initializer_list<T> init) {
        if (init.size() == 0) return {};
        auto b = makeBuilder<T>(init.size());
        for (const auto& v : init) b.push_back(v);
        return b.build();
    }

    /// A span copied from a vector.
    template <typename T>
    ArenaSpan<T> makeSpan(const std::vector<T>& items) {
        if (items.empty()) return {};
        auto b = makeBuilder<T>(items.size());
        for (const auto& v : items) b.push_back(v);
        return b.build();
    }

    /// A span copied from another span.
    template <typename T>
    ArenaSpan<T> makeSpan(const ArenaSpan<T>& items) {
        if (items.empty()) return {};
        auto b = makeBuilder<T>(items.size());
        for (const auto& v : items) b.push_back(v);
        return b.build();
    }

    /// A span built by transforming a range.
    template <typename T, typename Iter, typename Fn>
    ArenaSpan<T> makeSpan(Iter begin, Iter end, Fn transform) {
        if (begin == end) return {};
        auto b = makeBuilder<T>();
        for (auto it = begin; it != end; ++it) b.push_back(transform(*it));
        return b.build();
    }

    /// A span built by transforming a vector.
    template <typename T, typename U, typename Fn>
    ArenaSpan<T> makeSpan(const std::vector<U>& items, Fn transform) {
        if (items.empty()) return {};
        auto b = makeBuilder<T>(items.size());
        for (const auto& v : items) b.push_back(transform(v));
        return b.build();
    }

    /// A span built by transforming a span.
    template <typename T, typename U, typename Fn>
    ArenaSpan<T> makeSpan(const ArenaSpan<U>& items, Fn transform) {
        if (items.empty()) return {};
        auto b = makeBuilder<T>(items.size());
        for (const auto& v : items) b.push_back(transform(v));
        return b.build();
    }

    // ─── Concatenation ────────────────────────────────────────────────

    template <typename T>
    ArenaSpan<T> concatSpans(std::initializer_list<ArenaSpan<T>> spans) {
        size_t total = 0;
        for (const auto& s : spans) total += s.size();
        if (total == 0) return {};
        auto b = makeBuilder<T>(total);
        for (const auto& s : spans) {
            for (const auto& v : s) b.push_back(v);
        }
        return b.build();
    }

    template <typename T>
    ArenaSpan<T> concatSpans(const ArenaSpan<T>& a, const ArenaSpan<T>& b) {
        return concatSpans<T>({a, b});
    }

    template <typename T>
    ArenaSpan<T> appendSpan(const ArenaSpan<T>& base, const T& item) {
        auto b = makeBuilder<T>(base.size() + 1);
        for (const auto& v : base) b.push_back(v);
        b.push_back(item);
        return b.build();
    }

    template <typename T>
    ArenaSpan<T> appendSpan(const ArenaSpan<T>& base,
                            std::initializer_list<T> items) {
        auto b = makeBuilder<T>(base.size() + items.size());
        for (const auto& v : base) b.push_back(v);
        for (const auto& v : items) b.push_back(v);
        return b.build();
    }

private:
    // ─── The allocator ────────────────────────────────────────────────

    char* allocRaw(size_t size, size_t align) {
        size_t padding = 0;
        if (currentBlock_) {
            const uintptr_t addr =
                reinterpret_cast<uintptr_t>(currentBlock_ + currentOffset_);
            padding = (align - (addr % align)) % align;
        }

        if (!currentBlock_ ||
            currentOffset_ + padding + size > kBlockSize) {
            const size_t allocSize = std::max(kBlockSize, size + align);
            blocks_.push_back(std::make_unique<char[]>(allocSize));
            currentBlock_  = blocks_.back().get();
            currentOffset_ = 0;

            const uintptr_t addr = reinterpret_cast<uintptr_t>(currentBlock_);
            padding = (align - (addr % align)) % align;
        }

        char* p = currentBlock_ + currentOffset_ + padding;
        currentOffset_ += padding + size;
        return p;
    }

    template <typename T>
    T* allocArrayRaw(size_t n) {
        if (n == 0) return nullptr;
        return reinterpret_cast<T*>(allocRaw(sizeof(T) * n, alignof(T)));
    }

    static constexpr size_t kBlockSize = 64 * 1024;

    std::vector<std::unique_ptr<char[]>> blocks_;
    char*  currentBlock_  = nullptr;
    size_t currentOffset_ = 0;
};