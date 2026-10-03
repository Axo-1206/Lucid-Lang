/**
 * @file Bytecode.hpp
 *
 * @responsibility The artifact. The single owned value the compiler
 *                 produces and the interpreter consumes. Fully
 *                 self-contained: no AST pointers, no host pointers,
 *                 no Sema pointers. Serializable.
 *
 * ─── What a Bytecode owns ─────────────────────────────────────────────────
 *   - Manifest           module identity and import edges
 *   - ConstantPool       every constant the code references
 *   - StaticData         baked fixed-table rows and top-level bindings
 *   - HostSymbolTable    the names of every host(...) reference
 *   - FunctionProto[]    one compiled function per Lucid-bodied FN
 *
 * ─── What a Bytecode does NOT own ─────────────────────────────────────────
 * Nothing from parser/, sema/, interp/, host/, runtime/, or runtime-abi/.
 * The interpreter includes exactly two headers from this folder:
 * Bytecode.hpp and Opcode.hpp. Everything else is compiler-internal.
 *
 * ─── Design: the artifact is a value ──────────────────────────────────────
 * A Bytecode is copyable, movable, and serializable. Two host sessions
 * can hold independent copies of the same .lucb without sharing state.
 * This is what makes hot reload, mod loading, and test isolation cheap.
 *
 * ─── Design: no pointers, only indices ────────────────────────────────────
 * A FunctionProto never stores a pointer to another FunctionProto; it
 * stores an index. A ConstantPool entry never stores a pointer to a
 * StaticData blob; it stores an offset. This is what makes the artifact
 * round-trippable through a byte stream and independent of allocator
 * addresses.
 */

#pragma once

#include "ConstantPool.hpp"
#include "FunctionProto.hpp"
#include "HostSymbolTable.hpp"
#include "Manifest.hpp"
#include "StaticData.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief The compiled form of one module set.
///
/// A Bytecode is the entire output of the compiler for one compilation
/// unit. It holds every function, every constant, every baked value, and
/// the list of host symbols the code references. Loading a Bytecode is
/// the interpreter's only input.
///
/// A Bytecode is *not* executable until it has been resolved against a
/// host registry: the HostSymbolTable holds names, and the load-time
/// resolution step turns those names into function pointers. A Bytecode
/// that has not been resolved is a valid artifact; calling into it is
/// what requires resolution.
class Bytecode {
public:
    Bytecode() = default;

    Bytecode(Manifest          manifest,
             ConstantPool      constants,
             StaticData        staticData,
             HostSymbolTable   hostSymbols,
             std::vector<FunctionProto> functions);

    Bytecode(const Bytecode&)            = default;
    Bytecode& operator=(const Bytecode&) = default;
    Bytecode(Bytecode&&)                 = default;
    Bytecode& operator=(Bytecode&&)      = default;
    ~Bytecode()                          = default;

    // ─── Component access ───────────────────────────────────────────────

    const Manifest&        manifest()     const noexcept { return m_manifest; }
    const ConstantPool&    constants()    const noexcept { return m_constants; }
    const StaticData&      staticData()   const noexcept { return m_staticData; }
    const HostSymbolTable& hostSymbols()  const noexcept { return m_hostSymbols; }

    const std::vector<FunctionProto>& functions() const noexcept {
        return m_functions;
    }

    // ─── Function lookup ────────────────────────────────────────────────

    /// The function at an index. Precondition: index < functions().size().
    /// Panics on an out-of-range index — that is a compiler or
    /// interpreter bug, not a user error.
    const FunctionProto& functionAt(uint32_t index) const;

    /// The index of the function with the given mangled name, or
    /// std::nullopt if no such function exists. Used by the embedding
    /// facade to resolve a call by name ("update", "onTick") to a
    /// function index.
    std::optional<uint32_t> findFunction(std::string_view mangledName) const;

    // ─── Invariants ─────────────────────────────────────────────────────
    //
    // Checked in the constructor. A violation means the compiler produced
    // a malformed artifact — it is a compiler bug, not a user error.
    //
    //   - Every FunctionProto has a non-empty name.
    //   - Every FunctionProto's code is non-empty.
    //   - Every FunctionProto's maxStackDepth is at least 1.
    //   - Every ConstantPool entry has a well-formed kind.
    //   - Every HostSymbolTable entry is a non-empty string.
    //   - Every entry in the manifest's import list resolves to a module
    //     path that appears in the manifest's module list, or is external.
    void checkInvariants() const;

private:
    Manifest                   m_manifest;
    ConstantPool               m_constants;
    StaticData                 m_staticData;
    HostSymbolTable            m_hostSymbols;
    std::vector<FunctionProto> m_functions;

    /// Mangled name → function index. Built in the constructor from
    /// m_functions; not serialized (rebuilt on load).
    std::unordered_map<std::string, uint32_t> m_byName;
};

} // namespace lucid::bytecode