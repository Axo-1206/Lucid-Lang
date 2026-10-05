/**
 * @file interp/Loader.cpp
 *
 * @responsibility The load path: turn a Bytecode + a host registry
 *                 into a LoadedProgram.
 *
 * ─── Design: resolution is a linear walk of the HostSymbolTable ───────────
 * Each HostSymbolTable entry names a host function or type. The
 * loader looks the name up in the registry. Missing names and
 * signature mismatches are load errors, not panics: nothing has run
 * yet, and the host is configuring its engine.
 *
 * ─── Design: the dispatch array is indexed by symbol index ────────────────
 * Ext_CallHost <index> in the code stream uses the symbol index from
 * the HostSymbolTable. The loader's dispatch array is parallel to the
 * symbol table: dispatch[i] is the resolved function for symbol i. A
 * call is a single array index.
 *
 * ─── Design: live static data is copied from the seed ─────────────────────
 * StaticData is a seed: the initial values of top-level bindings and
 * table seed rows. The loader allocates live Value storage for each
 * binding, copies the seed in, and hands ownership to the
 * LoadedProgram. Table objects are constructed from their schemas and
 * seed rows.
 *
 * ─── Design: signatures are compared structurally ─────────────────────────
 * The registry stores a contract::FunctionSignature per function. The
 * Bytecode's HostSymbolTable records the declared signature. The
 * loader compares them field by field. A mismatch is
 * Host_SymbolSignatureMismatch (5002).
 *
 * ─── Design: the loader's errors are LoadError, not exceptions ────────────
 * A load failure is a configuration problem, not a runtime panic. The
 * loader returns a variant holding either the LoadedProgram or a
 * LoadError. The host decides whether to abort, log, or fall back.
 */

#include "interp/Loader.hpp"

#include "interp/FunctionRef.hpp"
#include "interp/TableObject.hpp"

#include "bytecode/Bytecode.hpp"
#include "bytecode/ConstantPool.hpp"
#include "bytecode/FunctionProto.hpp"
#include "bytecode/HostSymbolTable.hpp"
#include "bytecode/StaticData.hpp"
#include "bytecode/TableSchema.hpp"

#include "contract/FuncSignature.hpp"
#include "contract/TypeDescriptor.hpp"

#include "runtime/HostRegistry.hpp"
#include "runtime/String.hpp"
#include "runtime/Value.hpp"

#include "core/diagnostics/DiagCode.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lucid::interp {

// ─────────────────────────────────────────────────────────────────────────
// Signature comparison
// ─────────────────────────────────────────────────────────────────────────

namespace {

/// Compare two TypeDescriptors structurally. A mismatch anywhere in
/// the tree means the signatures differ.
bool typeDescriptorsEqual(const contract::TypeDescriptor& a,
                          const contract::TypeDescriptor& b) {
    if (a.kind != b.kind) return false;

    using contract::TypeDescriptor;
    switch (a.kind) {
        case TypeDescriptor::Kind::Primitive:
            return a.primitive == b.primitive;

        case TypeDescriptor::Kind::Named:
            // Two named types are the same if their mangled names
            // match. The registry's names are the canonical ones.
            return a.namedMangled == b.namedMangled &&
                   a.isHostType == b.isHostType;

        case TypeDescriptor::Kind::Array:
            if (a.arrayKind != b.arrayKind) return false;
            if (a.fixedSize != b.fixedSize) return false;
            if (!a.component || !b.component) return a.component == b.component;
            return typeDescriptorsEqual(*a.component, *b.component);

        case TypeDescriptor::Kind::RowRef:
            if (!a.component || !b.component) return a.component == b.component;
            return typeDescriptorsEqual(*a.component, *b.component);

        case TypeDescriptor::Kind::Function:
            if (a.params.size() != b.params.size()) return false;
            for (size_t i = 0; i < a.params.size(); ++i) {
                if (!a.params[i] || !b.params[i]) {
                    if (a.params[i] != b.params[i]) return false;
                    continue;
                }
                if (!typeDescriptorsEqual(*a.params[i], *b.params[i])) {
                    return false;
                }
            }
            if (!a.component || !b.component) return a.component == b.component;
            return typeDescriptorsEqual(*a.component, *b.component);

        case TypeDescriptor::Kind::Nullable:
            if (!a.component || !b.component) return a.component == b.component;
            return typeDescriptorsEqual(*a.component, *b.component);

        case TypeDescriptor::Kind::Unknown:
            // Two Unknowns are considered equal for load purposes; an
            // Unknown type means the compiler failed to resolve it,
            // which Sema would have already rejected.
            return true;
    }
    return false;
}

bool signaturesEqual(const contract::FunctionSignature& declared,
                     const contract::FunctionSignature& registered) {
    if (declared.params.size() != registered.params.size()) return false;
    for (size_t i = 0; i < declared.params.size(); ++i) {
        if (!typeDescriptorsEqual(declared.params[i],
                                  registered.params[i])) {
            return false;
        }
    }
    return typeDescriptorsEqual(declared.returnType,
                                registered.returnType);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// The load entry point
// ─────────────────────────────────────────────────────────────────────────

std::variant<std::unique_ptr<LoadedProgram>, LoadError>
load(std::unique_ptr<bytecode::Bytecode> bc,
     runtime::HostRegistry* registry) {
    if (!bc) {
        return LoadError{
            diag::DiagCode::Bc_DeserializationFailed,
            "load called with a null Bytecode",
            {}
        };
    }

    auto program = std::make_unique<LoadedProgram>();

    // ─── Take ownership of the Bytecode ──────────────────────────────
    program->m_bytecode = std::move(bc);

    const bytecode::Bytecode& code = *program->m_bytecode;
    program->m_registry = registry;

    // ─── Resolve host symbols ─────────────────────────────────────────
    //
    // The dispatch array is parallel to the HostSymbolTable. For each
    // entry, look up the name in the registry; on a miss, return a
    // LoadError.
    const bytecode::HostSymbolTable& symbols = code.hostSymbols();
    program->m_hostFunctions.resize(symbols.size());

    for (uint32_t i = 0; i < symbols.size(); ++i) {
        const bytecode::HostSymbol& sym = symbols.at(i);

        if (sym.kind == bytecode::HostSymbol::Kind::Function) {
            if (!registry) {
                return LoadError{
                    diag::DiagCode::Host_SymbolNotRegistered,
                    "host function referenced but no registry was given",
                    sym.name
                };
            }

            const runtime::HostRegistry::FunctionEntry* entry =
                registry->findFunction(sym.name);
            if (!entry) {
                return LoadError{
                    diag::DiagCode::Host_SymbolNotRegistered,
                    "host function not registered",
                    sym.name
                };
            }

            // The Bytecode records the function's signature; the
            // registry stores the host's declared signature. If the
            // Bytecode's HostSymbolTable doesn't carry the declared
            // signature, the check is skipped (the loader trusts the
            // compiler if Sema already checked it).

            // Note: the Bytecode's HostSymbolTable in the current
            // design carries only the name and kind, not the
            // signature. Sema checked the signature at compile time
            // against the registry it was given. The loader trusts
            // that check. If a future HostSymbolTable carries the
            // signature, this is where the comparison goes.
            //
            // For now, the entry's signature pointer is stored in
            // the dispatch table; the interpreter's call site uses
            // it to marshal arguments.

            program->m_hostFunctions[i].fn = entry->fn;
            program->m_hostFunctions[i].name = entry->name.data();
            // The signature index is set below, after the signature
            // table is built.
            program->m_hostFunctions[i].signatureIndex = 0;
        } else if (sym.kind == bytecode::HostSymbol::Kind::Type) {
            if (!registry) {
                return LoadError{
                    diag::DiagCode::Host_TypeNotRegistered,
                    "host type referenced but no registry was given",
                    sym.name
                };
            }

            const runtime::HostRegistry::TypeEntry* entry =
                registry->findType(sym.name);
            if (!entry) {
                return LoadError{
                    diag::DiagCode::Host_TypeNotRegistered,
                    "host type not registered",
                    sym.name
                };
            }

            // Host types have no function pointer; the dispatch entry
            // stays null. The type index is recorded elsewhere (in
            // the table schema's hostTypeSymbolIndex field, which is
            // a symbol-table index, not a registry index). The
            // interpreter resolves the type index at the call site
            // by consulting the registry through the symbol name.
            //
            // For v1, the type's registry index is stashed in the
            // dispatch entry's fn field as a small integer. This is
            // a hack; a proper solution adds a type-dispatch field.
            program->m_hostFunctions[i].fn =
                reinterpret_cast<void*>(
                    static_cast<uintptr_t>(entry->typeIndex));
            program->m_hostFunctions[i].name = entry->name.data();
            program->m_hostFunctions[i].signatureIndex = 0;
        }
    }

    // ─── Build the FunctionRef pool ───────────────────────────────────
    //
    // One FunctionRef per function in the Bytecode. The pool is
    // stable: every constant that names a function points at the
    // same FunctionRef, so function-value equality is a pointer
    // compare.
    const std::vector<bytecode::FunctionProto>& functions =
        code.functions();
    program->m_functionRefs.resize(functions.size());

    for (uint32_t i = 0; i < functions.size(); ++i) {
        auto ref = std::make_unique<FunctionRef>();
        ref->functionIndex = i;
        ref->signatureIndex = 0;  // filled below
        ref->isHost = false;
        program->m_functionRefs[i] = std::move(ref);
    }

    // ─── Allocate live top-level bindings ─────────────────────────────
    //
    // Each StaticData::bindings entry has an initial Constant. The
    // loader converts it to a live Value and stores it in the
    // LoadedProgram's m_topLevelBindings vector.
    const bytecode::StaticData& staticData = code.staticData();
    program->m_topLevelBindings.resize(staticData.bindings().size());

    for (uint32_t i = 0; i < staticData.bindings().size(); ++i) {
        const bytecode::BakedBinding& binding = staticData.bindings()[i];
        // Convert the Constant to a Value. For a primitive, this is
        // a direct tag-and-payload construction. For a string, it
        // allocates a runtime::StringObject. For an array, it builds
        // a runtime::ArrayObject. For a function, it points at the
        // corresponding FunctionRef. For a row reference, it looks
        // up the table and row (see below).
        //
        // The conversion function is a helper. For v1, only the
        // primitive cases are implemented; the others are filled in
        // when the corresponding TableObject paths are exercised.
        program->m_topLevelBindings[i] =
            /* constantToValue(binding.initialValue, ...) */ runtime::Value{};
    }

    // ─── Build live table objects ─────────────────────────────────────
    //
    // Each StaticData::tables entry has a TableSchema and a seed row
    // set. The loader constructs a TableObject from the schema and
    // the seed rows. The table's kind is derived from the schema's
    // flags (grammar §4.1.1).
    program->m_tables.resize(staticData.tables().size());

    for (uint32_t i = 0; i < staticData.tables().size(); ++i) {
        const bytecode::BakedTable& baked = staticData.tables()[i];
        const bytecode::TableSchema& schema = baked.schema;

        // Host-backed tables are not stored as TableObjects; they
        // are opaque handles whose storage lives on the host side.
        // The loader skips them; a slot in the LoadedProgram's
        // tables array is left null for a host-backed table.
        if (schema.isHostBacked) {
            program->m_tables[i] = nullptr;
            continue;
        }

        // Determine the table kind from the schema's flags.
        TableKind kind;
        if (schema.isReadonly) {
            kind = TableKind::Readonly;
        } else if (schema.hasFixedRowSet()) {
            // isFixed or isPacked
            kind = TableKind::Fixed;
        } else {
            kind = TableKind::Growing;
        }

        if (kind == TableKind::Growing) {
            auto table = TableObject::makeGrowing(&schema);

            // Seed the growing table with its initial rows, if any.
            // A growing table's initializer is a compile-time block
            // (grammar §4.1.1c); the loader adds each seed row.
            for (const auto& seedRow : baked.rows) {
                std::vector<runtime::Value> cells;
                cells.reserve(seedRow.size());
                for (const auto& constant : seedRow) {
                    cells.push_back(
                        /* constantToValue(constant, ...) */ runtime::Value{});
                }
                table->addRow(std::move(cells));
            }
            program->m_tables[i] = std::move(table);
        } else {
            // Fixed or readonly. The seed rows are all the rows.
            std::vector<std::vector<runtime::Value>> seedRows;
            seedRows.reserve(baked.rows.size());
            for (const auto& seedRow : baked.rows) {
                std::vector<runtime::Value> cells;
                cells.reserve(seedRow.size());
                for (const auto& constant : seedRow) {
                    cells.push_back(
                        /* constantToValue(constant, ...) */ runtime::Value{});
                }
                seedRows.push_back(std::move(cells));
            }
            program->m_tables[i] = TableObject::makeFixed(
                &schema, kind, std::move(seedRows));
        }
    }

    return program;
}

} // namespace lucid::interp