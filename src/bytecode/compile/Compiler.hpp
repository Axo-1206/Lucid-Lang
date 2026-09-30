/**
 * @file compile/Compiler.hpp
 *
 * @responsibility The driver: takes a resolved module set and produces
 *                 a Bytecode.
 *
 * ─── Design: the compiler trusts Sema ─────────────────────────────────────
 * Every name is resolved, every type is resolved, every constant is
 * folded, every resource kind is classified. The compiler reads those
 * fields and asserts them as it goes. An assertion failure means Sema
 * broke its contract; it is a compiler bug, not a user error.
 *
 * ─── Design: artifact-index maps ──────────────────────────────────────────
 * The emitters need to translate a declaration's identity (an
 * InternedString mangled name) into an index into the artifact's
 * StaticData:
 *
 *   - m_staticDataOffsets: a top-level binding's index in
 *     StaticData::bindings. Used by LoadStaticData and
 *     StoreStaticData.
 *   - m_tableIndices: a table's index in StaticData::tables. Used by
 *     every table operation (LoadRow, TableAdd, TableFind, ...).
 *
 * Both maps are populated during compile()'s pass A, as each
 * declaration is baked. They are read during pass B, as the emitters
 * resolve identifier references and table operations.
 *
 * The maps are transient compiler state, not artifact state. They are
 * keyed on InternedString (which has a std::hash specialization), so
 * the lookups are O(1) and allocation-free; the pool is alive for the
 * compiler's lifetime, so the keys are valid throughout.
 */

#pragma once

#include "../Bytecode.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode::compile {

class Compiler {
public:
    Compiler(lucid::diag::DiagnosticEngine& diagnostics,
             StringPool& pool);

    Bytecode compile(const std::vector<ModuleAST*>& modules);

    /// The session's string pool.
    StringPool& pool() noexcept { return m_pool; }

    /// The index reserved for a function in pass A. Returns nullopt
    /// if the function is host-bound or was not registered.
    std::optional<uint32_t> functionIndexOf(const FnDeclAST* fn) const;

    /// The static-data offset of a top-level binding, or nullopt if
    /// the binding was not baked. A binding the emitters are trying
    /// to load always has an offset; the nullopt case is a compiler
    /// bug.
    std::optional<uint32_t> staticDataOffsetOf(
        InternedString mangledName) const;

    /// The artifact-wide index of a table, or nullopt if the table
    /// was not baked.
    std::optional<uint32_t> tableIndexOf(
        InternedString mangledName) const;

private:
    lucid::diag::DiagnosticEngine& m_diag;
    StringPool& m_pool;

    // Populated during compile()'s pass A.
    std::unordered_map<const FnDeclAST*, uint32_t> m_functionIndex;
    std::unordered_map<InternedString, uint32_t>   m_staticDataOffsets;
    std::unordered_map<InternedString, uint32_t>   m_tableIndices;

    // Registration helpers, called from compile() during pass A.
    void registerStaticDataOffset(InternedString mangledName, uint32_t offset);
    void registerTableIndex(InternedString mangledName, uint32_t index);

    // Baking helpers, called from compile() during pass A.
    void bakeTable(const TableDeclAST* table,
                   StaticData& staticData,
                   HostSymbolTable& hostSymbols);
    void bakeTopLevelBinding(const VarDeclAST* var,
                             StaticData& staticData);

    // Placeholders for Phase 3. The real type translation and the
    // real FunctionProto construction are in TypeTranslation.cpp and
    // CompilerContext::finalizeProto. These two helpers produce
    // placeholder values for pass A's FunctionProto list; pass B
    // replaces them via finalizeProto.
    TypeDescriptor makeUnknownTypeDescriptor() const;
    FunctionProto makePlaceholderProto(const FnDeclAST* fn) const;
};

} // namespace lucid::bytecode::compile