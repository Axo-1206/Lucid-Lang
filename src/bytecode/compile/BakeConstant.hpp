/**
 * @file compile/BakeConstant.hpp
 *
 * @responsibility Translate a ConstantValue from Sema's representation
 *                 into the artifact's serializable Constant, and route
 *                 it to the right container (ConstantPool vs.
 *                 StaticData).
 *
 * ─── Naming note ──────────────────────────────────────────────────────────
 * The FileStructure calls this ConstantFolding.cpp. It was renamed
 * because the compiler does not fold — Sema does. Keeping the name
 * "folding" would invite a future contributor to add a second folder.
 */

#pragma once

#include "../ConstantPool.hpp"
#include "core/ast/BaseAST.hpp"

namespace lucid::bytecode::compile {

/// @brief Translate a folded ConstantValue into a serializable Constant.
///
/// Precondition (asserted): value.isEvaluated() is true.
/// A Kind::Function's FnDeclAST* is resolved to a FunctionProto index by
/// the caller; this function only handles the value-translation part
/// and requires the caller to supply the index for that case.
Constant bakeConstant(const ConstantValue& value,
                      const TypeDescriptor& type,
                      uint32_t functionIndexForFunctionKind);

} // namespace lucid::bytecode::compile