/**
 * @file compile/BakeConstant.hpp
 *
 * @responsibility Translate a ConstantValue from Sema's representation
 *                 (core/ast/BaseAST.hpp) into the artifact's
 *                 serializable Constant (bytecode/ConstantPool.hpp).
 *                 This is a translation step, not a pass: it is called
 *                 at the point of use by EmitExpr (for a pool entry)
 *                 and EmitDecl (for a StaticData entry).
 *
 * ─── Naming note ──────────────────────────────────────────────────────────
 * The FileStructure calls this ConstantFolding.cpp. It was renamed
 * because the compiler does not fold — Sema does. Keeping the name
 * "folding" would invite a future contributor to add a second folder.
 *
 * ─── What it does ─────────────────────────────────────────────────────────
 *   - reads a ConstantValue that Sema has already folded
 *   - produces a Constant with the same payload, in the artifact's
 *     serializable form
 *   - resolves a Kind::Function constant's FnDeclAST* to a FunctionProto
 *     index (the caller supplies the index)
 *
 * ─── What it does NOT do ──────────────────────────────────────────────────
 *   - evaluate anything
 *   - fold anything
 *   - traverse a subtree
 *   - decide where the result goes (pool vs StaticData — the caller
 *     decides, because the caller knows whether this value is a code
 *     reference or a declaration's initial value)
 */

#pragma once

#include "../ConstantPool.hpp"
#include "core/ast/BaseAST.hpp"     // for ConstantValue

namespace lucid::bytecode::compile {

/// @brief Translate a folded ConstantValue into a serializable Constant.
///
/// Precondition (asserted): value.isEvaluated() is true.
///
/// The caller supplies the TypeDescriptor (read from the expression's
/// resolvedType) and, for Kind::Function, the FunctionProto index the
/// FnDeclAST* resolves to.
Constant bakeConstant(const ConstantValue& value,
                      const TypeDescriptor& type,
                      uint32_t functionIndexForFunctionKind);

} // namespace lucid::bytecode::compile