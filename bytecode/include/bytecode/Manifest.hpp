/**
 * @file bytecode/Manifest.hpp
 *
 * @responsibility Module identity, import edges, and per-module
 *                 contribution lists. Because the artifact's
 *                 containers are flat (one FunctionProto[], one
 *                 ConstantPool, one StaticData, one HostSymbolTable),
 *                 the Manifest is the only place that knows which
 *                 mangled names belong to which module. Tools that
 *                 want a per-module view consult this.
 *
 * ─── Design: no load order, no tier tags ──────────────────────────────────
 * Per §3.3 and §3.4, fixed tables have no load order and the Tier 1 /
 * Tier 2 split is a host decision. The manifest records what the
 * compiler saw, nothing about who loads it or when.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::bytecode {

struct Manifest {
    /// One entry per module in the compilation.
    struct Module {
        /// The module's own path (its identity, §3.1).
        /// e.g. "core.math" or "entities.person"
        std::string modulePath;

        /// The module's import edges, in source order.
        struct Import {
            std::string alias;        ///< local name
            std::string targetPath;   ///< imported module's identity
        };
        std::vector<Import> imports;

        /// Mangled names of this module's contributions, by kind.
        /// Consulted by tooling for per-module views; not used by
        /// the interpreter.
        std::vector<std::string> functions;  ///< FN declarations
        std::vector<std::string> bindings;   ///< top-level let/const
        std::vector<std::string> tables;     ///< TABLE declarations
    };

    std::vector<Module> modules;

    /// The entry module, if the compiler was told which one to start
    /// from. Empty if no entry point was specified (the CLI sets this
    /// from --entry or the build manifest).
    std::string entryModulePath;

    /// Invariants. A violation is a compiler bug.
    ///   - every Module::modulePath is non-empty and unique.
    ///   - every Module::Import alias is non-empty and unique within
    ///     its module.
    ///   - every Module::Import targetPath is non-empty.
    ///   - every mangled name in functions/bindings/tables is
    ///     non-empty and globally unique across all modules.
    void checkInvariants() const;
};

} // namespace lucid::bytecode