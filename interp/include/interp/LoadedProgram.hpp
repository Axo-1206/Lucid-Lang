/**
 * @file interp/LoadedProgram.hpp
 *
 * @responsibility The interpreter's view of a loaded program: the
 *                 Bytecode it owns, the resolved host-function
 *                 dispatch array, the live static data, the function
 *                 signature table, and the FunctionRef pool.
 *
 * ─── Design: the loader is the only thing that touches the registry ───────
 * The interpreter's hot loop never looks up a host name by string. The
 * loader resolves every HostSymbolTable entry against the host registry
 * once, producing a dispatch array of {function pointer, signature}
 * pairs. Ext_CallHost <index> indexes that array.
 *
 * ─── Design: static data is live, not a seed ──────────────────────────────
 * The Bytecode's StaticData is a *seed*. The loader allocates live
 * storage for each binding and each table, copies the seed in, and the
 * LoadedProgram owns the live storage.
 *
 * ─── Design: the FunctionRef pool is stable ───────────────────────────────
 * Every function value that appears in a constant is a FunctionRef. The
 * loader builds one FunctionRef per function (Lucid and host) and
 * every constant that names that function points at the same
 * FunctionRef. This makes function-value equality a pointer compare.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Value.hpp, interp/FunctionRef.hpp. The Bytecode and
 * FunctionProto definitions are needed only in the .cpp; the header
 * forward-declares. TableObject and HostRegistry are forward-declared.
 */

#pragma once

#include "core/diagnostics/DiagCode.hpp"
#include "interp/FunctionRef.hpp"
#include "runtime/Value.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace lucid::contract {
    struct FunctionSignature;
}
namespace lucid::bytecode {
    class Bytecode;
    struct FunctionProto;
}
namespace lucid::runtime {
    class HostRegistry;
}

namespace lucid::interp {

using lucid::runtime::Value;

class TableObject;

/// @brief A load failure.
struct LoadError {
    diag::DiagCode code;
    std::string    message;
    std::string    symbol;
};

/// @brief A resolved host function: the function pointer the host
///        registered, plus its signature. Produced by the loader.
struct ResolvedHostFunction {
    /// The function pointer. The interpreter casts it to the signature
    /// at the call site. A null pointer means the symbol was not
    /// resolved.
    void* fn = nullptr;

    /// Index into the signature table.
    uint32_t signatureIndex = 0;

    /// The name, retained for diagnostics. Owned by the
    /// LoadedProgram's string storage; not freed here.
    const char* name = nullptr;
};

/// @brief The interpreter's loaded view of a Bytecode.
class LoadedProgram {
public:
    LoadedProgram();
    ~LoadedProgram();

    LoadedProgram(LoadedProgram&&) noexcept;
    LoadedProgram& operator=(LoadedProgram&&) noexcept;
    LoadedProgram(const LoadedProgram&) = delete;
    LoadedProgram& operator=(const LoadedProgram&) = delete;

    // ─── Accessors ──────────────────────────────────────────────────────

    const bytecode::Bytecode* bytecode() const noexcept {
        return m_bytecode.get();
    }

    /// The function proto at the given index. Precondition: index is
    /// valid.
    const bytecode::FunctionProto* functionAt(uint32_t index) const noexcept;

    /// The FunctionRef for a function. Precondition: index is valid.
    FunctionRef* functionRefAt(uint32_t index) noexcept;

    /// The resolved host function at the given symbol index.
    const ResolvedHostFunction& hostFunctionAt(uint32_t index) const noexcept {
        return m_hostFunctions[index];
    }

    /// The signature at the given index.
    const contract::FunctionSignature& signatureAt(uint32_t index) const noexcept;

    // ─── Static data ────────────────────────────────────────────────────

    /// The live value of a top-level binding, by index.
    Value& topLevelBinding(uint32_t index) noexcept {
        return m_topLevelBindings[index];
    }

    /// A live table, by index into the Bytecode's table array.
    TableObject* tableAt(uint32_t index) noexcept {
        return m_tables[index].get();
    }

    // ─── Host registry (borrowed, not owned) ────────────────────────────

    runtime::HostRegistry* registry() const noexcept { return m_registry; }

private:
    friend std::variant<std::unique_ptr<LoadedProgram>, LoadError>
    load(std::unique_ptr<bytecode::Bytecode> bc,
         runtime::HostRegistry* registry);

    std::unique_ptr<bytecode::Bytecode> m_bytecode;

    /// One resolved host function per HostSymbolTable entry, in the
    /// same order. Ext_CallHost <index> indexes this array.
    std::vector<ResolvedHostFunction> m_hostFunctions;

    /// One FunctionRef per function (Lucid and host). Every constant
    /// that names a function points into this array.
    std::vector<std::unique_ptr<FunctionRef>> m_functionRefs;

    /// Live top-level binding storage, one entry per StaticData::bindings.
    std::vector<Value> m_topLevelBindings;

    /// Live tables, one per StaticData::tables.
    std::vector<std::unique_ptr<TableObject>> m_tables;

    /// Borrowed, not owned. Null if the LoadedProgram was constructed
    /// by the loader with a null registry (used in tests).
    runtime::HostRegistry* m_registry = nullptr;
};

} // namespace lucid::interp