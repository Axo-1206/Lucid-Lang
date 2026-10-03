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
 * ─── Design: the driver is thin ───────────────────────────────────────────
 * The driver's job is to walk the module set in the right order and
 * call the emitters. Per-declaration lowering lives in EmitDecl;
 * per-statement and per-expression lowering live in EmitStmt and
 * EmitExpr. The driver does not know how a declaration is baked or
 * how a statement is emitted — it just calls the right function at
 * the right time.
 *
 * ─── Design: three index maps ─────────────────────────────────────────────
 * Pass A populates three maps that pass B reads:
 *
 *   - m_functionIndex:    FnDeclAST*  → FunctionProto index
 *   - m_staticDataOffsets: mangled name → StaticData::bindings index
 *   - m_tableIndices:     mangled name → StaticData::tables index
 *
 * The emitters reach them through CompilerContext::compiler().
 *
 * ─── Design: artifact-wide containers live here ───────────────────────────
 * The driver owns the artifact containers (ConstantPool, StaticData,
 * HostSymbolTable, functions list) while it runs. At the end, it
 * moves them into a Bytecode. Before that, they're referenced by
 * ArtifactBuildState (pass A) and CompilerContext (pass B).
 */

#pragma once

#include "bytecode/Bytecode.hpp"
#include "bytecode/ConstantPool.hpp"
#include "bytecode/HostSymbolTable.hpp"
#include "bytecode/StaticData.hpp"
#include "bytecode/FunctionProto.hpp"

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

    /// Compile a set of Sema-resolved modules into one Bytecode.
    ///
    /// Preconditions (asserted): every module is non-null and has no
    /// Sema errors.
    Bytecode compile(const std::vector<ModuleAST*>& modules);

    /// The session's string pool.
    StringPool& pool() noexcept { return m_pool; }

    // ─── Artifact index lookups (used by the emitters) ──────────────────

    /// The FunctionProto index reserved for a function in pass A.
    /// Returns nullopt if the function was not registered.
    std::optional<uint32_t> functionIndexOf(const FnDeclAST* fn) const;

    /// The static-data offset of a top-level binding. Returns nullopt
    /// if the binding was not baked.
    std::optional<uint32_t> staticDataOffsetOf(
        InternedString mangledName) const;

    /// The artifact-wide index of a table. Returns nullopt if the
    /// table was not baked.
    std::optional<uint32_t> tableIndexOf(
        InternedString mangledName) const;

private:
    lucid::diag::DiagnosticEngine& m_diag;
    StringPool& m_pool;

    // Populated during compile()'s pass A; read during pass B.
    std::unordered_map<const FnDeclAST*, uint32_t> m_functionIndex;
    std::unordered_map<InternedString, uint32_t>   m_staticDataOffsets;
    std::unordered_map<InternedString, uint32_t>   m_tableIndices;
};

} // namespace lucid::bytecode::compile