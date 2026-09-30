/**
 * @file Opcode.hpp
 *
 * @responsibility The instruction set. A closed enum plus operand-
 *                 encoding rules. The only file besides Bytecode.hpp
 *                 that interp/ includes from this folder.
 *
 * ─── Design: typed opcodes, not a dispatch-on-type at runtime ─────────────
 * Sema resolved every operand's type at compile time. The compiler emits
 * a typed opcode (Add_Int32, Add_Float64, ...) rather than a generic Add
 * with a type operand. The interpreter's dispatch is therefore a flat
 * switch with no secondary type dispatch. This is the standard shape for
 * a bytecode with a resolved type system and it is what makes the
 * interpreter's inner loop small.
 *
 * ─── Design: opcodes are grouped by band ──────────────────────────────────
 * The enum is ordered so that related opcodes are contiguous. The bands
 * are: load/store, arithmetic, comparison, logical, bitwise, null,
 * calls, aggregate, control, sequence, return, panic, resource, debug.
 *
 * ─── Design: operand encoding lives here ──────────────────────────────────
 * Each opcode has a fixed operand shape. OpcodeInfo records it so the
 * serializer, the disassembler, and the interp's operand validator can
 * walk code without a per-opcode switch. The actual encoder/decoder
 * (variable-length u32, etc.) lives in Serialize.cpp and Dispatch.cpp.
 */

#pragma once

#include <cstdint>

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Opcode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The instruction set.
///
/// Every opcode is one byte. Operands follow the opcode in the code
/// stream, encoded by the shape recorded in OpcodeInfo.
enum class Opcode : uint8_t {
    // ─── Load/store ─────────────────────────────────────────────────────
    Nop              = 0x00,  ///< No operation; carries a SourceLocation.

    LoadLocal        = 0x01,  ///< u16 slot
    StoreLocal       = 0x02,  ///< u16 slot
    LoadConst        = 0x03,  ///< u32 constant index
    LoadStaticData   = 0x04,  ///< u32 static-data offset
    StoreStaticData  = 0x05,  ///< u32 static-data offset
    LoadFunction     = 0x06,  ///< u32 function index (a code address)
    LoadField        = 0x07,  ///< u16 column index (row ref on stack)
    StoreField       = 0x08,  ///< u16 column index (row ref + value on stack)
    LoadRow          = 0x09,  ///< table operand (index on stack)
    StoreRow         = 0x0A,  ///< table operand (index + value on stack)
    LoadIndex        = 0x0B,  ///< array (index on stack)
    StoreIndex       = 0x0C,  ///< array (index + value on stack)

    // ─── Arithmetic — signed integers ───────────────────────────────────
    Add_I8   = 0x10, Add_I16 = 0x11, Add_I32 = 0x12, Add_I64 = 0x13,
    Sub_I8   = 0x14, Sub_I16 = 0x15, Sub_I32 = 0x16, Sub_I64 = 0x17,
    Mul_I8   = 0x18, Mul_I16 = 0x19, Mul_I32 = 0x1A, Mul_I64 = 0x1B,
    Div_I8   = 0x1C, Div_I16 = 0x1D, Div_I32 = 0x1E, Div_I64 = 0x1F,
    Mod_I8   = 0x20, Mod_I16 = 0x21, Mod_I32 = 0x22, Mod_I64 = 0x23,
    Pow_I8   = 0x24, Pow_I16 = 0x25, Pow_I32 = 0x26, Pow_I64 = 0x27,

    // ─── Arithmetic — unsigned integers ─────────────────────────────────
    Add_U8   = 0x30, Add_U16 = 0x31, Add_U32 = 0x32, Add_U64 = 0x33,
    Sub_U8   = 0x34, Sub_U16 = 0x35, Sub_U32 = 0x36, Sub_U64 = 0x37,
    Mul_U8   = 0x38, Mul_U16 = 0x39, Mul_U32 = 0x3A, Mul_U64 = 0x3B,
    Div_U8   = 0x3C, Div_U16 = 0x3D, Div_U32 = 0x3E, Div_U64 = 0x3F,
    Mod_U8   = 0x40, Mod_U16 = 0x41, Mod_U32 = 0x42, Mod_U64 = 0x43,
    Pow_U8   = 0x44, Pow_U16 = 0x45, Pow_U32 = 0x46, Pow_U64 = 0x47,

    // ─── Arithmetic — floats ────────────────────────────────────────────
    Add_F32 = 0x50, Add_F64 = 0x51,
    Sub_F32 = 0x52, Sub_F64 = 0x53,
    Mul_F32 = 0x54, Mul_F64 = 0x55,
    Div_F32 = 0x56, Div_F64 = 0x57,
    Mod_F32 = 0x58, Mod_F64 = 0x59,
    Pow_F32 = 0x5A, Pow_F64 = 0x5B,

    // ─── Arithmetic — string ────────────────────────────────────────────
    Concat_Str = 0x60,        ///< string + string

    // ─── Negation / not / bitwise-not ───────────────────────────────────
    Neg_I8  = 0x70, Neg_I16 = 0x71, Neg_I32 = 0x72, Neg_I64 = 0x73,
    Neg_F32 = 0x74, Neg_F64 = 0x75,
    Not_Bool = 0x76,
    BitNot_I8 = 0x77, BitNot_I16 = 0x78,
    BitNot_I32 = 0x79, BitNot_I64 = 0x7A,
    BitNot_U8 = 0x7B, BitNot_U16 = 0x7C,
    BitNot_U32 = 0x7D, BitNot_U64 = 0x7E,

    // ─── Comparison ─────────────────────────────────────────────────────
    // Each comparison produces a bool. Typed by operand kind.
    Eq_I8   = 0x80, Eq_I16 = 0x81, Eq_I32 = 0x82, Eq_I64 = 0x83,
    Eq_U8   = 0x84, Eq_U16 = 0x85, Eq_U32 = 0x86, Eq_U64 = 0x87,
    Eq_F32  = 0x88, Eq_F64 = 0x89,
    Eq_Bool = 0x8A, Eq_Char = 0x8B, Eq_Str = 0x8C,
    Eq_RowRef = 0x8D,         ///< identity comparison on a &T
    Eq_Function = 0x8E,       ///< identity comparison on a function value

    Ne_I8   = 0x90, Ne_I16 = 0x91, Ne_I32 = 0x92, Ne_I64 = 0x93,
    Ne_U8   = 0x94, Ne_U16 = 0x95, Ne_U32 = 0x96, Ne_U64 = 0x97,
    Ne_F32  = 0x98, Ne_F64 = 0x99,
    Ne_Bool = 0x9A, Ne_Char = 0x9B, Ne_Str = 0x9C,
    Ne_RowRef = 0x9D, Ne_Function = 0x9E,

    Lt_I8   = 0xA0, Lt_I16 = 0xA1, Lt_I32 = 0xA2, Lt_I64 = 0xA3,
    Lt_U8   = 0xA4, Lt_U16 = 0xA5, Lt_U32 = 0xA6, Lt_U64 = 0xA7,
    Lt_F32  = 0xA8, Lt_F64 = 0xA9, Lt_Char = 0xAA, Lt_Str = 0xAB,

    Le_I8   = 0xB0, Le_I16 = 0xB1, Le_I32 = 0xB2, Le_I64 = 0xB3,
    Le_U8   = 0xB4, Le_U16 = 0xB5, Le_U32 = 0xB6, Le_U64 = 0xB7,
    Le_F32  = 0xB8, Le_F64 = 0xB9, Le_Char = 0xBA, Le_Str = 0xBB,

    Gt_I8   = 0xC0, Gt_I16 = 0xC1, Gt_I32 = 0xC2, Gt_I64 = 0xC3,
    Gt_U8   = 0xC4, Gt_U16 = 0xC5, Gt_U32 = 0xC6, Gt_U64 = 0xC7,
    Gt_F32  = 0xC8, Gt_F64 = 0xC9, Gt_Char = 0xCA, Gt_Str = 0xCB,

    Ge_I8   = 0xD0, Ge_I16 = 0xD1, Ge_I32 = 0xD2, Ge_I64 = 0xD3,
    Ge_U8   = 0xD4, Ge_U16 = 0xD5, Ge_U32 = 0xD6, Ge_U64 = 0xD7,
    Ge_F32  = 0xD8, Ge_F64 = 0xD9, Ge_Char = 0xDA, Ge_Str = 0xDB,

    // ─── Bitwise ────────────────────────────────────────────────────────
    BitAnd_I8 = 0xE0, BitAnd_I16 = 0xE1,
    BitAnd_I32 = 0xE2, BitAnd_I64 = 0xE3,
    BitAnd_U8 = 0xE4, BitAnd_U16 = 0xE5,
    BitAnd_U32 = 0xE6, BitAnd_U64 = 0xE7,

    BitOr_I8 = 0xE8, BitOr_I16 = 0xE9,
    BitOr_I32 = 0xEA, BitOr_I64 = 0xEB,
    BitOr_U8 = 0xEC, BitOr_U16 = 0xED,
    BitOr_U32 = 0xEE, BitOr_U64 = 0xEF,

    BitXor_I8 = 0xF0, BitXor_I16 = 0xF1,
    BitXor_I32 = 0xF2, BitXor_I64 = 0xF3,
    BitXor_U8 = 0xF4, BitXor_U16 = 0xF5,
    BitXor_U32 = 0xF6, BitXor_U64 = 0xF7,

    Shl_I8 = 0xF8, Shl_I16 = 0xF9, Shl_I32 = 0xFA, Shl_I64 = 0xFB,
    Shr_I8 = 0xFC, Shr_I16 = 0xFD, Shr_I32 = 0xFE, Shr_I64 = 0xFF,
    // ── NOTE: byte value 0xFF is the last single-byte opcode. ──
    //
    // Everything below uses a two-byte encoding: 0x00 as a prefix byte
    // followed by a second byte selecting the extended opcode. This is
    // the standard escape convention. The interpreter's dispatch
    // switches on the first byte; a 0x00 first byte switches on the
    // second.

    // Extended opcodes (first byte 0x00).
    // Value is the second byte.
    Ext_Shr_U8   = 0x01, Ext_Shr_U16 = 0x02,
    Ext_Shr_U32  = 0x03, Ext_Shr_U64 = 0x04,

    // ─── Null handling ──────────────────────────────────────────────────
    Ext_IsNil      = 0x10,   ///< pop value; push bool (is it nil?)
    Ext_Coalesce   = 0x11,   ///< full ?? lowering; used when LHS is not
                             ///< a simple load. See EmitExpr.
    Ext_CheckNil   = 0x12,   ///< pop value; panic with PanicNilDeref if nil

    // ─── Calls ──────────────────────────────────────────────────────────
    Ext_Call          = 0x20,  ///< u32 function index; args on stack
    Ext_CallHost      = 0x21,  ///< u32 host symbol index; args on stack
    Ext_CallTableMeth = 0x22,  ///< u8 method tag; receiver + args on stack
    Ext_CallArrayMeth = 0x23,  ///< u8 method tag; receiver + args on stack

    // ─── Aggregate ──────────────────────────────────────────────────────
    Ext_NewArray       = 0x30,  ///< u32 element count
    Ext_NewFixedArray  = 0x31,  ///< u32 element count
    Ext_ArrayAdd       = 0x32,  ///< receiver + value on stack
    Ext_ArrayRemove    = 0x33,  ///< receiver + index on stack
    Ext_ArrayClear     = 0x34,
    Ext_ArraySort      = 0x35,  ///< u8 flag: 0 = natural, 1 = comparator
    Ext_ArrayContains  = 0x36,
    Ext_TableAdd       = 0x37,  ///< u32 table index; cells on stack
    Ext_TableRemove    = 0x38,  ///< u32 table index; index on stack
    Ext_TableClear     = 0x39,
    Ext_TableShrink    = 0x3A,
    Ext_TableCount     = 0x3B,
    Ext_TableVersion   = 0x3C,
    Ext_TableFind      = 0x3D,
    Ext_TableAt        = 0x3E,
    Ext_TableByPrimary = 0x3F,  ///< u32 table index, u16 column index
    Ext_ColumnToArray  = 0x40,

    // ─── Control ────────────────────────────────────────────────────────
    Ext_Jump         = 0x50,  ///< i32 relative offset
    Ext_JumpIfFalse  = 0x51,  ///< i32 relative offset
    Ext_JumpIfTrue   = 0x52,  ///< i32 relative offset
    Ext_SwitchMember = 0x53,  ///< u32 row-count; comparison table follows

    // ─── Sequences ──────────────────────────────────────────────────────
    Ext_SuspendWait          = 0x60,  ///< u32 resume index
    Ext_SuspendWaitFrames    = 0x61,
    Ext_SuspendWaitUntil     = 0x62,
    Ext_SuspendWaitForEvent  = 0x63,
    Ext_SuspendWaitForRequest = 0x64,
    Ext_StartSequence        = 0x65,  ///< u32 function index

    // ─── Return ─────────────────────────────────────────────────────────
    Ext_Return     = 0x70,
    Ext_ReturnVoid = 0x71,

    // ─── Panic ──────────────────────────────────────────────────────────
    Ext_Panic                  = 0x80,  ///< u32 DiagCode; msg on stack
    Ext_PanicNilDeref          = 0x81,
    Ext_PanicStaleRef          = 0x82,
    Ext_PanicDuplicateKey      = 0x83,
    Ext_PanicGenerationExhausted = 0x84,

    // ─── Resource management ────────────────────────────────────────────
    Ext_Retain  = 0x90,  ///< ResourceKind-driven; retain a value
    Ext_Release = 0x91,  ///< ResourceKind-driven; release a value
    Ext_Copy    = 0x92,  ///< deep copy per ResourceKind
};

// ─────────────────────────────────────────────────────────────────────────────
// OpcodeInfo
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The operand shape of one opcode.
enum class OperandShape : uint8_t {
    None,            ///< no operands
    U8,              ///< one u8
    U16,             ///< one u16
    U32,             ///< one u32
    I32,             ///< one i32 (relative jump)
    U8_U16,          ///< two operands
    U32_U16,         ///< two operands
    SwitchTable,     ///< u32 count, then count * (u32 case index + i32 offset)
};

/// @brief Static information about an opcode, used by the serializer,
///        the disassembler, and the interpreter's operand validator.
struct OpcodeInfo {
    Opcode       opcode;
    OperandShape shape;
    const char*  name;
};

/// @brief Look up an opcode's info. Precondition: op is a valid opcode.
const OpcodeInfo& opcodeInfo(Opcode op) noexcept;

/// @brief True if the byte is a valid single-byte opcode.
bool isSingleByteOpcode(uint8_t byte) noexcept;

/// @brief True if the byte is a valid extended opcode (second byte
///        after a 0x00 prefix).
bool isExtendedOpcode(uint8_t byte) noexcept;

} // namespace lucid::bytecode