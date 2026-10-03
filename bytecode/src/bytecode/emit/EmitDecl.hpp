// ─── compile/EmitDecl.hpp ──────────────────────────────────────────────────
/**
 * @file compile/EmitDecl.hpp
 *
 * @responsibility Lower a declaration's non-body parts: function
 *                 headers, host-bound symbol registration, fixed-table
 *                 rows (into StaticData), top-level bindings (into
 *                 StaticData).
 */

#pragma once

#include "core/ast/DeclAST.hpp"
#include "bytecode/Bytecode.hpp"
#include "../compile/CompilerContext.hpp"

namespace lucid::bytecode::compile {

/// @brief Emit the non-body parts of a declaration into the artifact
///        under construction.
void emitDecl(DeclAST* decl, CompilerContext& ctx);

} // namespace lucid::bytecode::compile