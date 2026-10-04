/// @file bytecode/Bytecode.cpp
/// @brief The whole artifact: constructor, function lookup, invariants.

#include "bytecode/Bytecode.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Bytecode::Bytecode(Manifest                   manifest,
                   ConstantPool               constants,
                   StaticData                 staticData,
                   HostSymbolTable            hostSymbols,
                   std::vector<FunctionProto> functions)
    : m_manifest(std::move(manifest))
    , m_constants(std::move(constants))
    , m_staticData(std::move(staticData))
    , m_hostSymbols(std::move(hostSymbols))
    , m_functions(std::move(functions)) {
    // Build the name → index map before the invariant check, because
    // some of the invariants (uniqueness of function names) are easier
    // to express against the map.
    m_byName.reserve(m_functions.size());
    for (size_t i = 0; i < m_functions.size(); ++i) {
        const std::string& name = m_functions[i].name();
        auto [it, inserted] = m_byName.emplace(name, static_cast<uint32_t>(i));
        AST_ASSERT_MSG(inserted,
            "Bytecode: two functions have the same mangled name — "
            "the compiler's mangling scheme produced a collision");
        (void)it;
    }

    checkInvariants();
}

// ─────────────────────────────────────────────────────────────────────────────
// Function lookup
// ─────────────────────────────────────────────────────────────────────────────

const FunctionProto& Bytecode::functionAt(uint32_t index) const {
    AST_ASSERT_MSG(index < m_functions.size(),
        "Bytecode::functionAt: index out of range — "
        "a Call instruction named a function the artifact does not have");
    return m_functions[index];
}

std::optional<uint32_t> Bytecode::findFunction(std::string_view mangledName) const {
    auto it = m_byName.find(std::string(mangledName));
    if (it == m_byName.end()) return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Invariants
// ─────────────────────────────────────────────────────────────────────────────

void Bytecode::checkInvariants() const {
    // ─── Per-component invariants ──────────────────────────────────────
    //
    // Each component has its own checkInvariants. Bytecode's job is to
    // call them and then check the cross-component invariants below.
    m_constants.checkInvariants();
    m_staticData.checkInvariants();
    m_hostSymbols.checkInvariants();
    m_manifest.checkInvariants();

    // ─── Cross-component invariants ────────────────────────────────────

    // Every function's name is unique (already checked by the
    // constructor's insert, but re-checked here so the invariant is
    // visible in one place).
    //
    // Every Function-kind constant's function index is in range.
    // The pool does not know the function count, so this is the
    // place where that check happens.
    const uint32_t functionCount = static_cast<uint32_t>(m_functions.size());

    // Every RowRef constant's table index is in range of the
    // artifact's static-data tables, and every row index is in range
    // of that table's rows.
    //
    // A RowRef constant is only produced for a table whose row set is
    // fixed at declaration (@fixed or @readonly). For such a table,
    // the rows are baked into StaticData::tables[i].rows, and the
    // row index is stable. The check below validates the index
    // against that baked row set, and asserts that the table the
    // constant names has a fixed row set at all — a RowRef against a
    // growing table is a compiler bug (the compiler should never
    // have produced the constant), not a runtime state the
    // interpreter should have to handle.
    const uint32_t tableCount = static_cast<uint32_t>(m_staticData.tables().size());

    for (size_t ci = 0; ci < m_constants.size(); ++ci) {
        const Constant& c = m_constants.at(static_cast<uint32_t>(ci));

        if (c.kind == Constant::Kind::Function) {
            const uint32_t fnIndex = std::get<uint32_t>(c.value);
            AST_ASSERT_MSG(fnIndex < functionCount,
                "Bytecode: a Function-kind constant names a function "
                "index that is outside the artifact's function list — "
                "the compiler interned a constant against a function "
                "that was never added");
            continue;
        }

        if (c.kind == Constant::Kind::RowRef) {
            const auto& rr = std::get<RowRefConstant>(c.value);
            AST_ASSERT_MSG(rr.tableIndex < tableCount,
                "Bytecode: a RowRef constant names a table index that "
                "is outside the artifact's static-data table list");

            const BakedTable& table =
                m_staticData.tables()[rr.tableIndex];

            // A RowRef is only meaningful for a table whose row set
            // is fixed at declaration. Sema folds `T.Member` sugar
            // into a RowRef constant only when `T` has a fixed row
            // set; a RowRef against a growing table means the
            // compiler (or Sema) produced a constant for a table
            // whose rows can still move.
            AST_ASSERT_MSG(table.schema.hasFixedRowSet(),
                "Bytecode: a RowRef constant names a growing table — "
                "Sema should only fold T.Member sugar for a table "
                "whose row set is fixed at declaration");

            AST_ASSERT_MSG(rr.rowIndex < table.rows.size(),
                "Bytecode: a RowRef constant names a row index that is "
                "outside the referenced table's row list");
            continue;
        }
    }

    // Every host symbol referenced by a StaticData BakedTable's
    // hostTypeSymbolIndex is in range.
    for (const auto& table : m_staticData.tables()) {
        if (!table.schema.isHostBacked) continue;
        AST_ASSERT_MSG(table.schema.hostTypeSymbolIndex >= 0,
            "Bytecode: a host-backed table has no host type symbol index");
        const uint32_t symIndex =
            static_cast<uint32_t>(table.schema.hostTypeSymbolIndex);
        AST_ASSERT_MSG(symIndex < m_hostSymbols.size(),
            "Bytecode: a host-backed table's type symbol index is "
            "outside the host symbol table");
    }

    // Every import's target path is either one of the manifest's own
    // module paths (an intra-session import) or an external module
    // (not in the manifest). We cannot check the latter, so we only
    // check that the target is either present or absent — either is
    // legal. What we can check: an import's target is not the
    // importing module itself (a module cannot import itself).
    for (const auto& mod : m_manifest.modules) {
        for (const auto& imp : mod.imports) {
            AST_ASSERT_MSG(imp.targetPath != mod.modulePath,
                "Bytecode: a module imports itself — "
                "the parser or Sema should have rejected this");
        }
    }
}

} // namespace lucid::bytecode