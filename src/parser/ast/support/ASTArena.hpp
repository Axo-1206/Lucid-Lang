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
};