/**
 * @file interp/Loader.hpp
 *
 * @responsibility Turn a Bytecode + a host registry into a
 *                 LoadedProgram: resolve host symbols, allocate live
 *                 static data, build the FunctionRef pool.
 *
 * ─── Design: the loader is where signature checks happen ──────────────────
 * The Bytecode's HostSymbolTable records each host("...") reference's
 * name and declared signature. The loader looks the name up in the
 * registry. If the name is missing, that's Host_SymbolNotRegistered
 * (5001). If the name is present but the registered signature differs
 * from the declared signature, that's Host_SymbolSignatureMismatch
 * (5002). Both are load-time errors: the LoadedProgram is not produced.
 *
 * There is no call-time signature check. The loader guarantees the
 * signature matches; the interpreter's Ext_CallHost casts the function
 * pointer to the registered signature and calls it.
 *
 * ─── Design: the loader returns a Result, not an exception ────────────────
 * A load error is not a panic — nothing has run yet, and the host is
 * configuring its engine. The loader returns a Result<LoadedProgram,
 * LoadError>; the host decides whether to abort, log, or fall back.
 * The interpreter's PanicException is for run-time panics only.
 */

#pragma once

#include "interp/LoadedProgram.hpp"
#include "runtime/Panic.hpp"

#include <memory>
#include <string>
#include <variant>

namespace lucid::bytecode {
    class Bytecode;
}

namespace lucid::runtime {
    class HostRegistry;
}

namespace lucid::interp {

/// @brief A load failure. Carries a DiagCode from the Bc_* or Host_*
///        bands, a message, and the symbol name that failed (empty if
///        the failure was not symbol-specific).
struct LoadError {
    diag::DiagCode code;
    std::string message;
    std::string symbol;
};

/// @brief Load a Bytecode against a host registry.
///
/// On success, the returned LoadedProgram owns the Bytecode and the
/// live static data. On failure, the returned LoadError names what
/// went wrong.
///
/// The registry may be null, in which case any HostSymbolTable entry
/// is a Host_SymbolNotRegistered error. This is useful for tests that
/// load a program with no host references.
///
/// The loader does not run any Lucid code. It resolves symbols,
/// allocates static storage, and builds the FunctionRef pool.
std::variant<std::unique_ptr<LoadedProgram>, LoadError>
load(std::unique_ptr<bytecode::Bytecode> bc,
     runtime::HostRegistry* registry);

} // namespace lucid::interp