/**
 * @file compile/Emitcontract::BinaryOps.hpp
 *
 * @responsibility The contract::Opcode-selection helpers for typed binary,
 *                 unary, and bitwise operations. Pure functions of
 *                 (operator, PrimitiveKind) → contract::Opcode.
 *
 * ─── Why this is its own header ───────────────────────────────────────────
 * These helpers started in EmitExpr.cpp's anonymous namespace, where
 * they were reached only by emitBinaryExpr and emitUnaryExpr. Two
 * developments made them shared:
 *
 *   1. EmitStmt.cpp's compound-assignment lowering (`x += y`) needs
 *      to apply the operator after loading the old value and the
 *      RHS. It calls the same contract::Opcode-selection logic that
 *      emitBinaryExpr uses, but without a synthesized BinaryExprAST.
 *
 *   2. EmitExpr.cpp is over 1,000 lines. Splitting the pure
 *      contract::Opcode-selection logic out shrinks it and isolates the parts
 *      that are "which contract::Opcode?" from the parts that are "walk the
 *      AST and emit."
 *
 * ─── Design: pure functions, no state ─────────────────────────────────────
 * Every helper here is a pure function of its arguments. It reads no
 * context, touches no CompilerContext, emits nothing. It answers
 * "given this operator and this operand type, which contract::Opcode?" — and
 * asserts if the answer is "none" (which would be a Sema bug: Sema
 * is supposed to reject a type-incorrect operator before the
 * compiler runs).
 *
 * Callers write the contract::Opcode themselves via ctx.emitcontract::Opcode(...). The
 * helpers do not emit; they select.
 *
 * ─── Design: integer family / width index ─────────────────────────────────
 * The contract::Opcode enums are laid out in contiguous groups of four (one per
 * width) for signed integers, unsigned integers, and floats. The
 * helpers compute a base + width offset. Two small enums/functions
 * (IntFamily, widthIndex) are exposed because a caller that needs to
 * pick a width-specific contract::Opcode for a non-binary operation (a compound
 * assignment's arithmetic, say) needs the same classification.
 */

#pragma once

#include "core/ast/ExprAST.hpp"
#include "contract/Opcode.hpp"
#include "contract/TypeDescriptor.hpp"

#include "core/PrimitiveKind.hpp"

#include <optional>

namespace lucid::bytecode::compile {

// using contract::contract::Opcode;
// using contract::PrimitiveKind;
// using contract::contract::TypeDescriptor;

/// @brief The signed/unsigned classification of an integer primitive.
enum class IntFamily { None, Signed, Unsigned };

/// @brief Classify a primitive kind as signed integer, unsigned
///        integer, or neither.
IntFamily intFamilyOf(PrimitiveKind k);

/// @brief The width index of a primitive within its family.
///
///   I8  / U8  / F32 -> 0
///   I16 / U16 / F64 -> 1
///   I32 / U32       -> 2
///   I64 / U64       -> 3
///
/// Returns -1 for a primitive that has no width (String, Char, Bool,
/// Void). A caller that gets -1 has a type Sema should have rejected.
int widthIndex(PrimitiveKind k);

/// @brief The primitive kind of a type, if it is a primitive. Returns
///        nullopt for a non-primitive type.
///
/// Descends through one level of Nullable: a `T?` where T is primitive
/// reports T's kind. This matches how Sema treats a nilable primitive
/// for the purpose of operator selection — `int?` and `int` use the
/// same Add_I32.
std::optional<PrimitiveKind> primitiveOf(const contract::TypeDescriptor& t);

/// @brief Select the arithmetic contract::Opcode for (op, operand type).
///
/// Handles Add, Sub, Mul, Div, Mod, Pow, and (for String) Concat_Str.
/// Asserts if no contract::Opcode exists — Sema should have rejected the
/// expression before the compiler ran.
contract::Opcode arithmeticOpcode(BinaryOp op, PrimitiveKind k);

/// @brief Select the opcode for a compound assignment's operator.
///
/// `x op= y` lowers to `x = x op y`. The compound operator maps to
/// one binary operator, and the binary operator plus the operand
/// type selects an opcode. This function does both mappings.
///
/// Every AssignOp except plain `Assign` has a BinaryOp counterpart.
/// `Assign` itself is not a compound operator; the caller handles it
/// separately.
contract::Opcode compoundAssignOpcode(AssignOp op, PrimitiveKind k);

/// @brief Select the comparison contract::Opcode for (op, operand type).
///
/// Handles Eq, Ne, Lt, Le, Gt, Ge. RowRef and Function comparisons are
/// identity, not value, and are dispatched by the caller before this
/// helper is reached (see emitBinaryExpr's Eq/Ne cases).
contract::Opcode comparisonOpcode(BinaryOp op, PrimitiveKind k);

/// @brief Select the bitwise contract::Opcode for (op, operand type).
///
/// Handles BitAnd, BitOr, BitXor, Shl, Shr. Shr dispatches on the
/// signed/unsigned family: signed uses Shr_I*, unsigned uses
/// Ext_Shr_U*. Shl shares the signed Shl range for both families,
/// because left shift is the same operation for signed and unsigned.
contract::Opcode bitwiseOpcode(BinaryOp op, PrimitiveKind k);

/// @brief Select the unary contract::Opcode for (op, operand type).
///
/// Handles Neg, Not, BitNot.
contract::Opcode unaryOpcode(UnaryOp op, PrimitiveKind k);

} // namespace lucid::bytecode::compile