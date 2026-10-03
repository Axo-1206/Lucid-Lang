/**
 * @file compile/EmitDecl.hpp
 *
 * @responsibility Lower a declaration. Two entry points, one per pass:
 *
 *   - emitDeclArtifacts: the artifact-level parts of any declaration.
 *     Called by the driver once per declaration, during pass A, before
 *     any function body is emitted. It produces StaticData entries,
 *     host symbols, and function-index reservations. It emits no code.
 *
 *   - emitDeclPrologue: the per-function prologue of a Lucid-bodied
 *     function. Called by the driver once per function, during pass B,
 *     before the body is emitted. It allocates parameter slots and
 *     records the function's opening line entry.
 *
 * ─── Why two functions ────────────────────────────────────────────────────
 * The two passes need different contexts. Pass A works over the whole
 * module set with only artifact-wide state (ArtifactBuildState). Pass B
 * works over one function at a time with per-function state
 * (CompilerContext). Trying to unify them into one function would force
 * a context that carries state that is meaningless for half the calls.
 *
 * The names mirror emitStmt/emitExpr, which are single-pass dispatchers
 * over their respective AST families. emitDeclArtifacts and
 * emitDeclPrologue together are the two-pass equivalent.
 */

#pragma once

#include "../compile/ArtifactBuildState.hpp"
#include "bytecode/compile/CompilerContext.hpp"
#include "core/ast/DeclAST.hpp"

namespace lucid::bytecode::compile {

// ─── Pass A: artifact-level parts of a declaration ──────────────────────────

/// @brief Emit the artifact-level parts of a declaration.
///
/// Handles:
///   - TableDeclAST:    bakes a BakedTable into StaticData, registers
///                      the table's index in state.tableIndices,
///                      registers the host symbol if host-backed.
///   - VarDeclAST:      bakes a BakedBinding into StaticData, registers
///                      the binding's offset in state.staticDataOffsets.
///   - FnDeclAST:       registers the function's index in
///                      state.functionIndex (Lucid-bodied) or the host
///                      symbol (host-bound). Emits no code.
///   - ImportDeclAST:   records the import in the manifest. (The
///                      manifest itself is owned by the driver; this
///                      function receives the module's manifest entry
///                      via the driver's caller, not through state.)
///
/// Called by the driver for every top-level declaration, in source
/// order, during pass A.
void emitDeclArtifacts(DeclAST* decl, ArtifactBuildState& state);

// ─── Pass B: per-function prologue ──────────────────────────────────────────

/// @brief Emit the prologue of a Lucid-bodied function.
///
/// Allocates one frame slot per parameter, in declaration order, and
/// records the function's opening source location in the line table.
/// Does NOT emit the body; the driver calls emitStmt on the body
/// immediately after this returns.
///
/// Preconditions (asserted):
///   - fn is not host-bound.
///   - fn has a body block.
///   - fn has a resolved return type.
///
/// Called by the driver for every Lucid-bodied function, during pass B,
/// after the CompilerContext is constructed and the current function
/// is set.
void emitDeclPrologue(FnDeclAST* fn, CompilerContext& ctx);

} // namespace lucid::bytecode::compile