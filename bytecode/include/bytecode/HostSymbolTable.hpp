/**
 * @file HostSymbolTable.hpp
 *
 * @responsibility The names of every host(...) reference this Bytecode
 *                 makes. Names only — no signatures, no pointers.
 *
 * ─── Design: names are the only stable identifier ─────────────────────────
 * A .lucb is built independently of the host binary that will load it.
 * It cannot store pointers (the host may be on another machine, or
 * rebuilt after the .lucb was compiled). It stores names, which survive
 * serialization and host rebuilds.
 *
 * ─── Design: resolution happens at load time, in the interpreter ──────────
 * The interpreter walks this table, asks the host registry to resolve
 * each name, and builds a dispatch array (symbol index → function
 * pointer). A CallHost opcode carries a symbol index; the interpreter
 * indexes the dispatch array. No name lookup happens per call.
 *
 * ─── Design: the kind tag distinguishes a function from a type ────────────
 * A host(...) reference can be a function (FN x = host("...")) or a type
 * (TABLE X = host("...")). The kind tag lets the load-time resolver
 * route the two cases to the registry's function-resolution and
 * type-resolution paths. Without the tag, the resolver would have to
 * try both.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode {

/// @brief One host symbol entry.
struct HostSymbol {
    enum class Kind : uint8_t {
        Function,   ///< FN x = host("...")
        Type,       ///< TABLE X = host("...")
    };

    Kind         kind = Kind::Function;
    std::string  name;    ///< the string inside host("...")
};

/// @brief The table.
class HostSymbolTable {
public:
    HostSymbolTable() = default;

    /// @brief Register a host symbol. Returns its index.
    /// Deduplicates by (kind, name).
    uint32_t add(HostSymbol sym);

    /// @brief The entry at an index. Panics on out-of-range.
    const HostSymbol& at(uint32_t index) const;

    /// @brief The index of a symbol, or nullopt if absent.
    std::optional<uint32_t> find(HostSymbol::Kind kind,
                                 std::string_view name) const;

    size_t size() const noexcept { return m_symbols.size(); }
    const std::vector<HostSymbol>& all() const noexcept { return m_symbols; }

    /// @brief Invariants. A violation is a compiler bug.
    ///   - every entry's name is non-empty.
    void checkInvariants() const;

private:
    std::vector<HostSymbol>                   m_symbols;
    std::unordered_map<std::string, uint32_t> m_index;
        // key = (kind-byte, name) concatenated; see the .cpp
};

} // namespace lucid::bytecode