/**
 * @file Manifest.hpp
 *
 * @responsibility Module identity and import edges. What the compiler
 *                 saw, not what the host will do with it.
 *
 * ─── Design: no load order, no tier tags ──────────────────────────────────
 * Per §3.3 and §3.4, fixed tables have no load order and the Tier 1 /
 * Tier 2 split is a host decision. The manifest records the module's
 * path and its import aliases; nothing about who loads it or when.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief The manifest.
struct Manifest {
    /// The module's own path (its identity, §3.1).
    std::string modulePath;

    /// The module's import edges, in source order.
    struct Import {
        std::string alias;        ///< the local name
        std::string targetPath;   ///< the imported module's identity
    };
    std::vector<Import> imports;

    /// The names this module contributes to other modules' views
    /// (its @export'ed declarations, by mangled name).
    std::vector<std::string> exports;

    /// Invariants. A violation is a compiler bug.
    ///   - modulePath is non-empty.
    ///   - every import alias is non-empty and unique within the module.
    ///   - every import targetPath is non-empty.
    void checkInvariants() const;
};

} // namespace lucid::bytecode