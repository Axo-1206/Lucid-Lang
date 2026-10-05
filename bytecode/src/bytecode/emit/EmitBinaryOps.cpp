/// @file compile/EmitBinaryOps.cpp
/// @brief The opcode-selection helpers for typed operations.

#include "EmitBinaryOps.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Classification
// ─────────────────────────────────────────────────────────────────────────────

IntFamily intFamilyOf(PrimitiveKind k) {
    if (isSignedIntegerKind(k))   return IntFamily::Signed;
    if (isUnsignedIntegerKind(k)) return IntFamily::Unsigned;
    return IntFamily::None;
}

int widthIndex(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Int8:    return 0;
        case PrimitiveKind::Int16:   return 1;
        case PrimitiveKind::Int32:   return 2;
        case PrimitiveKind::Int64:   return 3;
        case PrimitiveKind::Uint8:   return 0;
        case PrimitiveKind::Uint16:  return 1;
        case PrimitiveKind::Uint32:  return 2;
        case PrimitiveKind::Uint64:  return 3;
        case PrimitiveKind::Float32: return 0;
        case PrimitiveKind::Float64: return 1;
        default: return -1;
    }
}

std::optional<PrimitiveKind> primitiveOf(const TypeDescriptor& t) {
    if (t.isPrimitive()) return t.primitive;
    if (t.isNullable() && t.component && t.component->isPrimitive()) {
        return t.component->primitive;
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opcode selection
// ─────────────────────────────────────────────────────────────────────────────
//
// The opcode enums are laid out in contiguous groups. For each family
// and operation, the base is the opcode of the narrowest width in that
// family. Adding `widthIndex(k)` selects the correct width.

Opcode arithmeticOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    AST_ASSERT_MSG(w >= 0,
        "arithmeticOpcode: operand type is not a numeric primitive — "
        "Sema should have rejected this binary expression");

    const IntFamily fam = intFamilyOf(k);
    if (fam == IntFamily::Signed) {
        // Add_I8 = 0x10, groups of 4 per op, in the order
        // Add, Sub, Mul, Div, Mod, Pow.
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_I8);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x0C + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x10 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x14 + w);
            default: break;
        }
    } else if (fam == IntFamily::Unsigned) {
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_U8);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x0C + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x10 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x14 + w);
            default: break;
        }
    } else if (isFloatKind(k)) {
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_F32);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x02 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x06 + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x0A + w);
            default: break;
        }
    } else if (k == PrimitiveKind::String && op == BinaryOp::Add) {
        return Opcode::Concat_Str;
    }

    AST_ASSERT_MSG(false,
        "arithmeticOpcode: no opcode exists for this (op, type) pair — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

Opcode compoundAssignOpcode(AssignOp op, PrimitiveKind k) {
    switch (op) {
        case AssignOp::AddAssign:
            return arithmeticOpcode(BinaryOp::Add, k);
        case AssignOp::SubAssign:
            return arithmeticOpcode(BinaryOp::Sub, k);
        case AssignOp::MulAssign:
            return arithmeticOpcode(BinaryOp::Mul, k);
        case AssignOp::DivAssign:
            return arithmeticOpcode(BinaryOp::Div, k);
        case AssignOp::ModAssign:
            return arithmeticOpcode(BinaryOp::Mod, k);
        case AssignOp::BitAndAssign:
            return bitwiseOpcode(BinaryOp::BitAnd, k);
        case AssignOp::BitOrAssign:
            return bitwiseOpcode(BinaryOp::BitOr, k);
        case AssignOp::BitXorAssign:
            return bitwiseOpcode(BinaryOp::BitXor, k);
        case AssignOp::ShlAssign:
            return bitwiseOpcode(BinaryOp::Shl, k);
        case AssignOp::ShrAssign:
            return bitwiseOpcode(BinaryOp::Shr, k);
        case AssignOp::Assign:
            break;   // not a compound operator
    }
    AST_ASSERT_MSG(false,
        "compoundAssignOpcode: Assign is not a compound operator — "
        "the caller must handle plain assignment separately");
    return Opcode::Nop;
}

Opcode comparisonOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    // RowRef and Function comparisons are identity, not value.
    // They are dispatched from emitBinaryExpr's Eq/Ne cases before
    // this helper is called.

    const IntFamily fam = intFamilyOf(k);
    if (fam == IntFamily::Signed) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_I8);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_I8);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_I8);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_I8);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_I8);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_I8);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (fam == IntFamily::Unsigned) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_U8);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_U8);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_U8);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_U8);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_U8);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_U8);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (isFloatKind(k)) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_F32);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_F32);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_F32);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_F32);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_F32);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_F32);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (k == PrimitiveKind::Bool) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Bool;
            case BinaryOp::Ne: return Opcode::Ne_Bool;
            default: break;
        }
    } else if (k == PrimitiveKind::Char) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Char;
            case BinaryOp::Ne: return Opcode::Ne_Char;
            case BinaryOp::Lt: return Opcode::Lt_Char;
            case BinaryOp::Le: return Opcode::Le_Char;
            case BinaryOp::Gt: return Opcode::Gt_Char;
            case BinaryOp::Ge: return Opcode::Ge_Char;
            default: break;
        }
    } else if (k == PrimitiveKind::String) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Str;
            case BinaryOp::Ne: return Opcode::Ne_Str;
            case BinaryOp::Lt: return Opcode::Lt_Str;
            case BinaryOp::Le: return Opcode::Le_Str;
            case BinaryOp::Gt: return Opcode::Gt_Str;
            case BinaryOp::Ge: return Opcode::Ge_Str;
            default: break;
        }
    }

    AST_ASSERT_MSG(false,
        "comparisonOpcode: no opcode exists for this (op, type) pair — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

Opcode bitwiseOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    const IntFamily fam = intFamilyOf(k);
    AST_ASSERT_MSG(fam != IntFamily::None,
        "bitwiseOpcode: operand type is not an integer — "
        "Sema should have rejected this binary expression");

    const uint16_t andBase = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitAnd_I8)
        : static_cast<uint16_t>(Opcode::BitAnd_U8);
    const uint16_t orBase  = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitOr_I8)
        : static_cast<uint16_t>(Opcode::BitOr_U8);
    const uint16_t xorBase = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitXor_I8)
        : static_cast<uint16_t>(Opcode::BitXor_U8);

    switch (op) {
        case BinaryOp::BitAnd: return static_cast<Opcode>(andBase + w);
        case BinaryOp::BitOr:  return static_cast<Opcode>(orBase  + w);
        case BinaryOp::BitXor: return static_cast<Opcode>(xorBase + w);
        case BinaryOp::Shl:
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Shl_I8) + w);
            // Unsigned Shl shares the signed Shl range: the shift
            // operand is a count, and left shift is the same operation
            // for signed and unsigned. Only right shift differs.
            return static_cast<Opcode>(
                static_cast<uint16_t>(Opcode::Shl_I8) + w);
        case BinaryOp::Shr:
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Shr_I8) + w);
            return static_cast<Opcode>(
                static_cast<uint16_t>(Opcode::Ext_Shr_U8) + w);
        default: break;
    }
    AST_ASSERT_MSG(false,
        "bitwiseOpcode: no opcode exists for this operation — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

Opcode unaryOpcode(UnaryOp op, PrimitiveKind k) {
    switch (op) {
        case UnaryOp::Neg: {
            const IntFamily fam = intFamilyOf(k);
            const int w = widthIndex(k);
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Neg_I8) + w);
            if (isFloatKind(k))
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Neg_F32) + w);
            // Negation of an unsigned value is legal in the grammar:
            // the result is a signed value of the next-wider type.
            // Sema resolves that; the operand of Neg_U* in the
            // artifact is already the widened type. We re-dispatch on
            // the resolved type of the operand, which Sema has
            // already widened.
            AST_ASSERT_MSG(false,
                "unaryOpcode: Neg applied to a non-numeric operand — "
                "Sema should have widened or rejected");
            break;
        }
        case UnaryOp::Not:
            AST_ASSERT_MSG(k == PrimitiveKind::Bool,
                "unaryOpcode: Not applied to a non-bool operand — "
                "grammar §6.14 requires a bool");
            return Opcode::Not_Bool;
        case UnaryOp::BitNot: {
            const IntFamily fam = intFamilyOf(k);
            const int w = widthIndex(k);
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::BitNot_I8) + w);
            if (fam == IntFamily::Unsigned)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::BitNot_U8) + w);
            AST_ASSERT_MSG(false,
                "unaryOpcode: BitNot applied to a non-integer operand — "
                "Sema should have rejected");
            break;
        }
    }
    AST_ASSERT_MSG(false, "unaryOpcode: unhandled UnaryOp");
    return Opcode::Nop;
}

} // namespace lucid::bytecode