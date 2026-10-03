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

#include "bytecode/ConstantPool.hpp"
#include "core/ast/BaseAST.hpp"     // for ConstantValue
#include "core/memory/StringPool.hpp"

namespace lucid::bytecode::compile {

/// @brief Translate a folded ConstantValue into a serializable Constant.
///
/// Precondition (asserted): value.kind is not Unknown, Error, or Void.
/// A value with one of those kinds reaching the compiler is a Sema bug.
///
/// The caller supplies the TypeDescriptor (read from the expression's
/// resolvedType) and, for a Kind::Function value, the FunctionProto
/// index the FnDeclAST* resolves to. For a non-Function value, pass
/// UINT32_MAX as the sentinel — the parameter is ignored.
///
/// The pool is required because String and Char constants are stored as
/// InternedString values whose text is only recoverable via the session's
/// StringPool.
Constant bakeConstant(StringPool& pool,
                      const ConstantValue& value,
                      const TypeDescriptor& type,
                      uint32_t functionIndexForFunctionKind = UINT32_MAX);

} // namespace lucid::bytecode::compile