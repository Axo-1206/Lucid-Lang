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

#include "ArenaSpan.hpp"

#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <initializer_list>

// ─────────────────────────────────────────────────────────────────────────────
// ASTDeleter – No-op deleter for arena-backed unique_ptr
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief No-op deleter for arena-allocated objects.
 *
 * Since the arena reclaims all memory at once, deleting individual nodes
 * is unnecessary and would be incorrect. This deleter is used as the
 * second template argument to std::unique_ptr to suppress deallocation.
 */
struct ASTDeleter {
    void operator()(void*) const { /* no-op */ }
};

/**
 * @brief Alias for a unique_ptr that uses ASTDeleter.
 *
 * All owned AST node pointers should use this alias. It behaves exactly
 * like std::unique_ptr except that it does not call delete on the managed
 * object. This is safe because the ASTArena owns the memory and will
 * free it in bulk when the arena is destroyed.
 *
 * @tparam T The type of the managed object (must be a subclass of BaseAST).
 */
template <typename T>
using ASTPtr = std::unique_ptr<T, ASTDeleter>;

// ─────────────────────────────────────────────────────────────────────────────
// ASTArena – Bump-allocator for AST nodes
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Arena allocator for AST nodes using a bump-pointer strategy.
 *
 * Memory is allocated in 64 KB blocks. Each allocation is properly aligned
 * and constructed via placement new. The arena cannot be copied or moved.
 * All AST nodes for a single translation unit should be allocated from
 * the same ASTArena.
 */
class ASTArena {
    // ─────────────────────────────────────────────────────────────────────────
    // Private fields
    // ─────────────────────────────────────────────────────────────────────────

    /// Owned memory blocks. Each block is a contiguous chunk of characters.
    std::vector<std::unique_ptr<char[]>> blocks;

    /// Pointer to the current block being filled (last element of `blocks`).
    char* currentBlock = nullptr;

    /// Offset into the current block where the next allocation will start.
    size_t currentOffset = 0;

    /// Size of each memory block (64 KB).
    static constexpr size_t BLOCK_SIZE = 64 * 1024;

    // ─────────────────────────────────────────────────────────────────────────
    // Private allocation helpers
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Low‑level raw memory allocation with alignment.
     *
     * This is the heart of the bump allocator. It calculates required
     * padding for the requested alignment, checks if the current block
     * has enough space, and allocates a new block if needed.
     *
     * @param size Number of bytes to allocate.
     * @param align Required alignment (must be a power of two).
     * @return Pointer to the allocated memory (never null).
     */
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

    /**
     * @brief Allocate raw memory for an array of `T` without constructing elements.
     *
     * This method is used internally by `allocArray` and `SpanBuilder::build`
     * to obtain the raw storage. The caller is responsible for constructing
     * the elements (usually via placement new).
     *
     * @tparam T The element type.
     * @param size Number of elements.
     * @return Pointer to the allocated memory, or nullptr if size == 0.
     */
    template<typename T>
    T* allocArrayRaw(size_t size) {
        if (size == 0) return nullptr;
        char* ptr = allocRaw(sizeof(T) * size, alignof(T));
        return reinterpret_cast<T*>(ptr);
    }

public:
    // ─────────────────────────────────────────────────────────────────────────
    // Construction / destruction
    // ─────────────────────────────────────────────────────────────────────────

    /// Default constructor – creates an empty arena.
    ASTArena() = default;

    /// Destructor – all blocks are automatically freed.
    ~ASTArena() = default;

    // Disable copy and move to keep memory addresses stable.
    ASTArena(const ASTArena&) = delete;
    ASTArena& operator=(const ASTArena&) = delete;

    // ─────────────────────────────────────────────────────────────────────────
    // Single object allocation
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Allocate raw memory for a single object of type `T` and construct it.
     *
     * @tparam T The type of object to allocate.
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return Pointer to the newly constructed object.
     */
    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        char* ptr = allocRaw(sizeof(T), alignof(T));
        return ::new(ptr) T(std::forward<Args>(args)...);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Array allocation
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Allocate an array of `size` elements of type `T`, default‑constructing each.
     *
     * @tparam T The element type (must be default‑constructible).
     * @param size Number of elements.
     * @return ArenaSpan<T> referencing the allocated array (empty if size == 0).
     */
    template<typename T>
    ArenaSpan<T> allocArray(size_t size) {
        if (size == 0) return {};

        T* arr = allocArrayRaw<T>(size);
        for (size_t i = 0; i < size; ++i) {
            ::new(&arr[i]) T();
        }
        return ArenaSpan<T>(arr, size);
    }

    /**
     * @brief Allocate an array and copy‑construct elements from an initializer list.
     *
     * @tparam T The element type (must be copy‑constructible from the initializer values).
     * @param init Initializer list to copy from.
     * @return ArenaSpan<T> referencing the allocated array (empty if init.size() == 0).
     */
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
    // ASTPtr creation (single object)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Allocate and construct an object, returning an ASTPtr (unique_ptr with no‑op deleter).
     *
     * This is the primary method for allocating AST nodes.
     *
     * @tparam T The type of object to create.
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return ASTPtr<T> owning the new object.
     */
    template<typename T, typename... Args>
    ASTPtr<T> make(Args&&... args) {
        return ASTPtr<T>(alloc<T>(std::forward<Args>(args)...));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Dynamic array builder (SpanBuilder)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Helper class to build an ArenaSpan incrementally.
     *
     * The parser often does not know the final number of elements in advance.
     * SpanBuilder collects elements in a temporary std::vector, then when
     * `build()` is called, it copies/moves them into a contiguous arena block
     * and returns an ArenaSpan. This is the recommended way to construct
     * variable‑length child lists in the AST.
     *
     * @tparam T The element type (typically `ExprPtr`, `ParamPtr`, etc.).
     *
     * @code
     *   auto builder = arena.makeBuilder<ExprPtr>();
     *   while (hasMoreExprs()) {
     *       builder.push_back(parseExpr());
     *   }
     *   ArenaSpan<ExprPtr> exprs = builder.build();
     * @endcode
     */
    template<typename T>
    class SpanBuilder {
        ASTArena& arena;          ///< Reference to the arena that will own the final span.
        std::vector<T> temp;      ///< Temporary storage during building.

    public:
        /**
         * @brief Construct a builder associated with the given arena.
         * @param a The arena that will own the final array.
         */
        explicit SpanBuilder(ASTArena& a) : arena(a) {}

        /**
         * @brief Add an element by moving it into the builder.
         * @param value The element to add (will be moved from).
         */
        void push_back(T&& value) {
            temp.push_back(std::move(value));
        }

        /**
         * @brief Add an element by copying it into the builder.
         * @param value The element to copy.
         */
        void push_back(const T& value) {
            temp.push_back(value);
        }

        /**
         * @brief Construct a new element in‑place at the end of the builder.
         * @tparam Args Constructor argument types.
         * @param args Arguments forwarded to T's constructor.
         * @return Reference to the newly constructed element.
         */
        template<typename... Args>
        T& emplace_back(Args&&... args) {
            temp.emplace_back(std::forward<Args>(args)...);
            return temp.back();
        }

        /**
         * @brief Finalise the builder and transfer ownership to the arena.
         *
         * Allocates a contiguous array in the arena, moves each element from
         * the temporary vector into the arena, and clears the temporary.
         * The returned ArenaSpan remains valid for the lifetime of the arena.
         *
         * @return ArenaSpan<T> referencing the arena‑allocated array.
         */
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

        /// Returns the current number of elements in the builder.
        size_t size() const { return temp.size(); }

        /// Returns true if the builder is empty.
        bool empty() const { return temp.empty(); }

        /// Clears the builder without allocating arena memory.
        void clear() { temp.clear(); }

        /// Mutable access to an element in the temporary buffer.
        T& operator[](size_t idx) { return temp[idx]; }

        /// Const access to an element in the temporary buffer.
        const T& operator[](size_t idx) const { return temp[idx]; }

        /**
         * @brief Expose the temporary vector (for debugging or advanced use).
         * @return Reference to the internal std::vector.
         */
        std::vector<T>& getTemp() { return temp; }
    };

    /**
     * @brief Create a SpanBuilder for building a contiguous array incrementally.
     *
     * @tparam T The element type (e.g., `ExprPtr`, `ParamPtr`).
     * @return SpanBuilder<T> ready to accept elements.
     */
    template<typename T>
    SpanBuilder<T> makeBuilder() {
        return SpanBuilder<T>(*this);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Convenience aliases for different AST families
    // ─────────────────────────────────────────────────────────────────────────

    /// Allocate an expression node (ExprAST or its subclass).
    template<typename T, typename... Args>
    ASTPtr<T> makeExpr(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    /// Allocate a statement node (StmtAST or its subclass).
    template<typename T, typename... Args>
    ASTPtr<T> makeStmt(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    /// Allocate a declaration node (DeclAST or its subclass).
    template<typename T, typename... Args>
    ASTPtr<T> makeDecl(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    /// Allocate a type node (TypeAST or its subclass).
    template<typename T, typename... Args>
    ASTPtr<T> makeType(Args&&... args) { return make<T>(std::forward<Args>(args)...); }

    /// Allocate a pattern node (PatternAST or its subclass).
    template<typename T, typename... Args>
    ASTPtr<T> makePattern(Args&&... args) { return make<T>(std::forward<Args>(args)...); }
};