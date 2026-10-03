/// @file StringPool.hpp
/// @brief Owns all interned string data for a compilation session.
///
/// A StringPool maps std::string_view inputs to stable InternedString
/// handles. It is the canonical store for every identifier, keyword, and
/// string literal seen during a compilation. The pool owns the bytes; the
/// handles are indices into the pool.
///
/// ─── Ownership ────────────────────────────────────────────────────────────
/// The pool is not a singleton. It is a value that a CompilationSession
/// owns, alongside the diagnostic engine and the AST arena. The compiler
/// pipeline takes a reference to the session, and every subsystem that
/// needs to intern or look up a string gets the pool through that
/// reference. Two compilations in one process get two pools and do not
/// share string identity.
///
/// This matters for the LSP and for hot reload: each analysis constructs
/// a fresh session and destroys it when the analysis is done. A singleton
/// would keep strings alive across analyses and would let an ID from one
/// program be valid in another, which is a correctness hazard.
///
/// ─── Single-threaded, on purpose ──────────────────────────────────────────
/// Like the diagnostic engine, the pool is not thread-safe. The compiler
/// pipeline is strictly sequential, and each session is confined to one
/// thread. A future stage that wants to intern from a worker would queue
/// the work and intern on the session's thread.
///
/// ─── The block invariant ──────────────────────────────────────────────────
/// Every std::string_view the pool hands out, and every key in the
/// intern map, points into one of the pool's own bump-allocated blocks —
/// never into a caller's buffer. `intern()` copies the input bytes into a
/// block before storing the view. A caller may pass a temporary, a view
/// into a stack buffer, or a view into another pool; none of these
/// dangle, because the pool never stores the caller's pointer.

#pragma once

#include "InternedString.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class StringPool {
public:
    StringPool();
    ~StringPool() = default;

    // A pool holds std::string_views into its own blocks. Copying or
    // moving would leave those views pointing at the old blocks.
    StringPool(const StringPool&)            = delete;
    StringPool& operator=(const StringPool&) = delete;
    StringPool(StringPool&&)                 = delete;
    StringPool& operator=(StringPool&&)      = delete;

    /// Intern `s` and return a stable handle.
    ///
    ///   • Empty input → ID 0.
    ///   • Already interned → the existing handle.
    ///   • New input → copied into a pool block, given the next ID.
    ///
    /// The input is copied before any view of it is stored, so the caller
    /// may pass a temporary or a view into memory that will not outlive
    /// the call.
    InternedString intern(std::string_view s);

    /// Recover the string for a handle, as a std::string.
    ///
    /// Allocates. Use `lookupView` when a view will do — most call sites
    /// want a view, and only a final formatting step wants the copy.
    std::string lookup(InternedString s) const;

    /// Recover the string for a handle, as a std::string_view.
    ///
    /// The view is valid for the lifetime of the pool. This is the
    /// preferred accessor for every caller that does not need to
    /// outlive the pool.
    std::string_view lookupView(InternedString s) const;

    /// The number of distinct strings interned, including the empty
    /// string at ID 0. Useful for diagnostics and tests.
    size_t size() const noexcept { return strings_.size(); }

    /// True if `s` names a string this pool has interned.
    ///
    /// This is a bounds check, not a content check: any ID the pool
    /// previously returned is valid here, whether or not the ID is still
    /// referenced by any AST node.
    bool contains(InternedString s) const noexcept {
        return s.id < strings_.size();
    }

private:
    /// Copy `s` into a bump-allocated block and return a view into it.
    std::string_view allocateString(std::string_view s);

    std::unordered_map<std::string_view, uint32_t> internMap_;
    std::vector<std::string_view>                  strings_;  // ID → text

    // Bump allocator for the string bytes. Blocks are never freed until
    // the pool dies; a view into a block is valid for the pool's lifetime.
    std::vector<std::unique_ptr<char[]>> blocks_;
    char*  currentBlock_  = nullptr;
    size_t currentOffset_ = 0;
    static constexpr size_t kBlockSize = 64 * 1024;  // 64 KiB
};