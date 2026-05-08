/**
 * @file ASTArena.hpp
 *
 * @brief Arena allocator for AST nodes.
 *
 * The ASTArena provides fast, contiguous allocation for all AST nodes.
 * It uses a bump-pointer strategy within 64 KB blocks. Memory is never
 * freed individually; the entire arena is released when the compilation
 * unit is destroyed.
 *
 * ## Why this exists
 *
 * - **Performance**: all nodes for a translation unit are allocated from
 *   the same arena, which improves cache locality during traversal.
 * - **Simplicity**: no need to track individual node lifetimes; the arena
 *   reclaims everything at once.
 * - **Ownership semantics**: AST nodes are still managed through
 *   std::unique_ptr with a custom no-op deleter, so the rest of the
 *   codebase does not need to change pointer handling.
 *
 * ## Usage
 *
 * @code
 *   ASTArena arena;
 *   auto ptr = arena.makeExpr<BinaryExprAST>(...);
 *   // ptr is ASTPtr<BinaryExprAST> (unique_ptr with no-op deleter)
 *   // The node lives as long as the arena.
 * @endcode
 *
 * @note Because the arena never destroys individual nodes, any non-trivial
 *       destructors on AST nodes will not be called. This is acceptable
 *       because AST nodes contain only data and no owned resources (all
 *       strings are interned via StringPool, and child nodes are themselves
 *       arena-allocated and managed by unique_ptr with the same no-op
 *       deleter).
 *
 * @see InternedString, StringPool
 */

#pragma once

#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ASTDeleter – No-op deleter for arena-backed unique_ptr
//
// Since the arena reclaims all memory at once, deleting individual nodes
// is unnecessary and would be incorrect. This deleter is used as the
// second template argument to std::unique_ptr to suppress deallocation.
// ─────────────────────────────────────────────────────────────────────────────
struct ASTDeleter {
    void operator()(void*) const { /* no-op */ }
};

// ─────────────────────────────────────────────────────────────────────────────
// ASTPtr – unique_ptr alias with arena-compatible deleter
//
// Every owned AST node pointer should use this alias. It behaves exactly
// like std::unique_ptr except that it does not call delete on the managed
// object.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
using ASTPtr = std::unique_ptr<T, ASTDeleter>;

// ─────────────────────────────────────────────────────────────────────────────
// ASTArena – Bump-allocator for AST nodes
//
// Allocates memory in 64 KB blocks. Each allocation is properly aligned
// and placed via placement new. The arena cannot be copied or moved.
// ─────────────────────────────────────────────────────────────────────────────
class ASTArena {
    std::vector<std::unique_ptr<char[]>> blocks; ///< Owned memory blocks.
    char* currentBlock = nullptr;                ///< Current block being filled.
    size_t currentOffset = 0;                   ///< Offset into the current block.
    static constexpr size_t BLOCK_SIZE = 64 * 1024; ///< Size of each block.

public:
    ASTArena() = default;
    ~ASTArena() = default;

    // Disallow copy/move to ensure stable addresses.
    ASTArena(const ASTArena&) = delete;
    ASTArena& operator=(const ASTArena&) = delete;

    /**
     * @brief Allocates raw memory for an object of type T and constructs it.
     *
     * @tparam T      The type of object to allocate.
     * @tparam Args   Constructor argument types.
     * @param args    Arguments forwarded to T's constructor.
     * @return        Pointer to the newly constructed object.
     */
    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        constexpr size_t size = sizeof(T);
        constexpr size_t align = alignof(T);

        // Calculate padding needed for alignment.
        size_t padding = 0;
        if (currentBlock) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(currentBlock + currentOffset);
            padding = (align - (addr % align)) % align;
        }

        // If no block or not enough space, allocate a new block.
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

        return ::new(ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Allocates and constructs an object, returning it inside an ASTPtr.
     *
     * @tparam T      The type of object to create.
     * @tparam Args   Constructor argument types.
     * @param args    Arguments forwarded to T's constructor.
     * @return        ASTPtr<T> owning the new object (with no-op deleter).
     */
    template<typename T, typename... Args>
    ASTPtr<T> make(Args&&... args) {
        return ASTPtr<T>(alloc<T>(std::forward<Args>(args)...));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Convenience aliases for specific AST families
    // ─────────────────────────────────────────────────────────────────────────

    template<typename T, typename... Args>
    ASTPtr<T> makeExpr(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    ASTPtr<T> makeStmt(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    ASTPtr<T> makeDecl(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    ASTPtr<T> makeType(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    template<typename T, typename... Args>
    ASTPtr<T> makePattern(Args&&... args) { return make<T>(std::forward<Args>(args)...); }
};