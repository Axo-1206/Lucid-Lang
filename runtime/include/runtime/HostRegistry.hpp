/**
 * @file runtime/HostRegistry.hpp
 *
 * @responsibility The contract between the host and the runtime. The
 *                 host constructs a HostRegistry at startup, registers
 *                 its native functions and types, and hands it to the
 *                 loader (for signature checking) and to the runtime
 *                 (for handle retain/release dispatch).
 *
 * ─── Design: a value the host constructs, not global state ────────────────
 * The registry is a C++ object the engine builds. There is exactly one
 * per process in the common case, but the type is not a singleton: a
 * test can build its own, and a Tier 2 mod's scoped view (grammar
 * §3.4) is a separate registry that shares the parent's storage.
 *
 * ─── Design: the registry owns names, not implementations ─────────────────
 * A registration records the name, a function pointer (for functions),
 * a signature, and (for types) the retain/release/equality function
 * pointers. The runtime never dereferences a function pointer at
 * registration time; the loader does it at load time, and the
 * interpreter does it at call time.
 *
 * ─── Design: registration is by name, lookup is by name ───────────────────
 * The host registers a function with registerFunction("draw_sprite",
 * &draw_sprite). The loader looks it up by the same name (from the
 * Bytecode's HostSymbolTable). The name is the contract; the function
 * pointer is the implementation. Two registrations with the same name
 * are a host bug; the registry reports it.
 *
 * ─── Design: scoped views ─────────────────────────────────────────────────
 * A Tier 2 mod gets a view onto a subset of the registry that Tier 1
 * populated (grammar §3.4). The view is a HostRegistry whose lookup
 * consults the parent, but whose visible-name set is restricted. The
 * loader uses the view for the mod's HostSymbolTable; the interpreter
 * uses the view's function pointers, which point at the same
 * implementations the parent registered.
 *
 * ─── Design: signature checking is the loader's job, not the registry's ───
 * The registry records signatures as opaque byte strings (or a
 * structured Signature value; see bytecode/Signature.hpp). It does not
 * compare them. The loader compares a Bytecode's declared signature
 * against the registry's stored signature; a mismatch is a load error
 * (Host_SymbolSignatureMismatch, 5002).
 */

#pragma once

#include "contract/Signature.hpp" // FunctionSignature

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lucid::runtime {

// ─────────────────────────────────────────────────────────────────────────────
// Host function callbacks
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The function pointer type the host registers for a native
///        function.
///
/// The interpreter calls this pointer with the resolved signature.
/// The exact calling convention is a runtime/host contract; the
/// interpreter casts the pointer to the signature it looked up. A
/// variadic host function is registered with a packed-array signature
/// (see the grammar's §4.2.4): `int32_t host_sum_ints(const int32_t*
/// data, size_t count)`.
///
/// The return value is opaque to the registry. The interpreter and the
/// host agree on the ABI through the Signature.
using HostFunctionPtr = void*;

// ─────────────────────────────────────────────────────────────────────────────
// Host type callbacks
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Called by retainHandle when a handle's refcount is
///        incremented. May be null (the default is no-op).
using HostRetainFn = void (*)(void* payload);

/// @brief Called by releaseHandle when a handle's refcount reaches
///        zero. The host frees the payload here. May be null (the
///        default is to do nothing, leaking the payload — a host
///        that allocates should always provide one).
using HostReleaseFn = void (*)(void* payload);

/// @brief Called by handleEquals for a host type that registered a
///        custom equality. Returns true if the two payloads are
///        equal. May be null (the default is wrapper identity).
using HostEqualsFn = bool (*)(const void* a, const void* b);

/// @brief Called by the runtime when a @primary column needs a hash
///        of a host-type key. May be null (the default is to hash the
///        payload pointer).
using HostHashFn = uint64_t (*)(const void* payload);

// ─────────────────────────────────────────────────────────────────────────────
// HostRegistry
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The host's registration table.
///
/// Constructed once by the host, populated with registerFunction /
/// registerType calls, then handed to the loader. Not thread-safe
/// during registration; thread-safe for concurrent lookups after
/// registration is complete (a host that registers from multiple
/// threads must serialize externally).
class HostRegistry {
public:
    HostRegistry();
    ~HostRegistry();

    HostRegistry(HostRegistry&&) noexcept;
    HostRegistry& operator=(HostRegistry&&) noexcept;
    HostRegistry(const HostRegistry&) = delete;
    HostRegistry& operator=(const HostRegistry&) = delete;

    // ─── Registration ───────────────────────────────────────────────────

    /// @brief Register a native function under `name`.
    ///
    /// `signature` describes the function's parameter and return
    /// types. It is stored verbatim; the loader compares a Bytecode's
    /// declared signature against it.
    ///
    /// Returns false if `name` is already registered. Re-registration
    /// is a host bug, not a runtime error; the caller decides what to
    /// do.
    bool registerFunction(std::string name,
                          HostFunctionPtr fn,
                          contract::FunctionSignature signature);

    /// @brief Register a host type under `name`, with the given
    ///        payload callbacks. A host type is the payload of a
    ///        HostHandle; the callbacks define its copy/drop/equality
    ///        behavior.
    ///
    /// Returns false if `name` is already registered.
    bool registerType(std::string name,
                      HostRetainFn retain,
                      HostReleaseFn release,
                      HostEqualsFn equals,
                      HostHashFn hash);

    /// @brief Create a scoped view of this registry, restricted to
    ///        the given names. The view shares this registry's
    ///        storage; it is not a copy. A name not in `visibleNames`
    ///        is invisible to a lookup through the view, even if it
    ///        is registered in the parent.
    ///
    /// This is the Tier 2 mod boundary (grammar §3.4).
    std::unique_ptr<HostRegistry> scopedView(
        const std::vector<std::string>& visibleNames) const;

    // ─── Lookup ─────────────────────────────────────────────────────────

    /// @brief Look up a registered function by name. Returns nullptr
    ///        if not found. The returned pointer is owned by the
    ///        registry and is valid for the registry's lifetime.
    struct FunctionEntry {
        std::string_view name;
        HostFunctionPtr fn;
        const contract::FunctionSignature* signature;
    };

    const FunctionEntry* findFunction(std::string_view name) const noexcept;

    /// @brief Look up a registered type by name. Returns nullptr if
    ///        not found.
    struct TypeEntry {
        std::string_view name;
        uint32_t typeIndex;        // stable index; used by HostHandle
        HostRetainFn retain;
        HostReleaseFn release;
        HostEqualsFn equals;
        HostHashFn hash;
    };

    const TypeEntry* findType(std::string_view name) const noexcept;

    /// @brief Look up a registered type by its stable index. Used by
    ///        the runtime to find a handle's callbacks.
    const TypeEntry* typeAt(uint32_t typeIndex) const noexcept;

    // ─── Introspection ──────────────────────────────────────────────────

    /// @brief The number of registered functions.
    uint32_t functionCount() const noexcept;

    /// @brief The number of registered types.
    uint32_t typeCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lucid::runtime