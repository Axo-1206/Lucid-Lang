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
 *
 * The `functions` container is a member (m_functions), not a local,
 * because the lambda-lift stage writes into it between pass A and
 * pass B. The other containers (constants, staticData, hostSymbols)
 * are locals of compile() — nothing outside the driver's own body
 * needs to reach them.
 *
 * ─── Design: the lambda lift is a stage ───────────────────────────────────
 * Lambdas are lowered to top-level functions before pass B runs. The
 * LambdaLift stage owns the synthesized FnDeclAST nodes, registers
 * each with m_functionIndex, and appends a placeholder FunctionProto
 * to m_functions. Pass B emits both the module-level functions and
 * the synthesized ones.
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

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode::compile {

class LambdaLift;   // forward

class Compiler {
public:
    Compiler(lucid::diag::DiagnosticEngine& diagnostics,
             StringPool& pool);

    // A Compiler owns a unique_ptr<LambdaLift> whose LambdaLift type
    // is only forward-declared here. The destructor must be defined
    // where LambdaLift is complete, so it is declared but not
    // defined in this header.
    ~Compiler();

    Compiler(const Compiler&)            = delete;
    Compiler& operator=(const Compiler&) = delete;
    Compiler(Compiler&&)                 = delete;
    Compiler& operator=(Compiler&&)      = delete;

    /// Compile a set of Sema-resolved modules into one Bytecode.
    ///
    /// Preconditions (asserted): every module is non-null and has no
    /// Sema errors. Every lambda in every module was collected by
    /// Sema into that module's `lambdas` span.
    Bytecode compile(const std::vector<ModuleAST*>& modules);

    /// The session's string pool.
    StringPool& pool() noexcept { return m_pool; }

    // ─── Artifact index lookups (used by the emitters) ──────────────────

    /// The FunctionProto index reserved for a function in pass A (or
    /// the lambda lift). Returns nullopt if the function was not
    /// registered.
    std::optional<uint32_t> functionIndexOf(const FnDeclAST* fn) const;

    /// The static-data offset of a top-level binding. Returns nullopt
    /// if the binding was not baked.
    std::optional<uint32_t> staticDataOffsetOf(
        InternedString mangledName) const;

    /// The artifact-wide index of a table. Returns nullopt if the
    /// table was not baked.
    std::optional<uint32_t> tableIndexOf(
        InternedString mangledName) const;

    /// The HostSymbolTable index of a host-bound function. Returns
    /// nullopt if the function is not host-bound or was not
    /// registered. A Lucid-bodied function has a FunctionProto index
    /// (see functionIndexOf), not a host symbol index; the two maps
    /// are disjoint by construction.
    std::optional<uint32_t> hostSymbolIndexOf(
        const FnDeclAST* fn) const;

    // ─── Lambda lifting (used by the lift and by EmitExpr) ─────────────

    /// Synthesize a top-level FnDeclAST for a lambda. Called by
    /// LambdaLift::lift. Assigns the function a FunctionProto index
    /// (the same way pass A registers a module function) and appends
    /// a placeholder proto to m_functions.
    ///
    /// Precondition: `fn` was not previously registered.
    void registerSynthesizedFunction(FnDeclAST* fn, SourceLocation loc);

    /// The FunctionProto index of the function synthesized for a
    /// lambda, or nullopt if the lambda was not lifted. Used by
    /// emitLambdaExpr.
    std::optional<uint32_t> lambdaFunctionIndexFor(
        const LambdaExprAST* lambda) const;

    /// The lift stage's storage. Non-null between the lift call (in
    /// compile()) and the Compiler's destruction.
    const LambdaLift* lambdaLift() const noexcept {
        return m_lambdaLift.get();
    }

private:
    lucid::diag::DiagnosticEngine& m_diag;
    StringPool&                    m_pool;

    // ─── Artifact-wide containers ──────────────────────────────────────
    //
    // m_functions is a member because the lambda lift writes into it
    // between pass A and pass B. The other containers are locals of
    // compile(); nothing outside the driver's own body needs them.

    std::vector<FunctionProto> m_functions;

    // ─── Index maps ────────────────────────────────────────────────────
    //
    // m_functionIndex and m_hostSymbolIndex are disjoint: a function
    // is either Lucid-bodied (in m_functionIndex) or host-bound (in
    // m_hostSymbolIndex), never both. A synthesized lambda function
    // is in m_functionIndex (it has a FunctionProto, like any
    // Lucid-bodied function).

    std::unordered_map<const FnDeclAST*, uint32_t> m_functionIndex;
    std::unordered_map<const FnDeclAST*, uint32_t> m_hostSymbolIndex;
    std::unordered_map<InternedString, uint32_t>   m_staticDataOffsets;
    std::unordered_map<InternedString, uint32_t>   m_tableIndices;

    // ─── The lambda lift ───────────────────────────────────────────────

    std::unique_ptr<LambdaLift> m_lambdaLift;
};

} // namespace lucid::bytecode::compile