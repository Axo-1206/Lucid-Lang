/**
 * @file Opcode.hpp
 *
 * @responsibility The instruction set. A closed enum plus operand-
 *                 encoding rules. The only file besides Bytecode.hpp
 *                 that interp/ includes from this folder.
 *
 * ─── Design: typed opcodes, not a dispatch-on-type at runtime ─────────────
 * Sema resolved every operand's type at compile time. The compiler emits
 * a typed opcode (Add_I32, Add_F64, ...) rather than a generic Add with
 * a type operand. The interpreter's dispatch is therefore a flat switch
 * with no secondary type dispatch. This is the standard shape for a
 * bytecode with a resolved type system and it is what makes the
 * interpreter's inner loop small.
 *
 * ─── Design: opcodes are grouped by band ──────────────────────────────────
 * The enum is ordered so that related opcodes are contiguous. The bands
 * are: load/store, arithmetic, comparison, logical, bitwise, null,
 * calls, aggregate, control, sequence, return, panic, resource.
 *
 * ─── Design: two numeric spaces, disjoint by construction ─────────────────
 * The byte encoding uses one byte for most opcodes, and a two-byte
 * escape (0x00 followed by a second byte) for the rest. The Opcode
 * enum mirrors this with two numeric ranges:
 *
 *   - Single-byte opcodes: 0x0001 .. 0x00FF
 *       The low byte is the opcode's byte in the code stream. 0x00 is
 *       reserved as the escape prefix and is never a valid opcode, so
 *       the single-byte range starts at 0x0001.
 *
 *   - Extended opcodes:    0x0100 .. 0x01FF
 *       The low byte is the second byte of the two-byte encoding; the
 *       high byte (0x01) marks "this is an extended opcode". 0x0100 is
 *       reserved (it would mean "escape followed by 0x00", which is not
 *       a valid encoding), so the extended range starts at 0x0101.
 *
 * This makes the enum values themselves unambiguous: a value in
 * 0x00xx is a single-byte opcode, and a value in 0x01xx is an extended
 * opcode. No separate tag field is needed, and opcodeInfo can dispatch
 * with one comparison on the high byte.
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
/// Values 0x0001..0x00FF are single-byte opcodes; values 0x0101..0x01FF
/// are extended opcodes (a 0x00 prefix byte followed by the low byte in
/// the code stream). 0x0000 and 0x0100 are reserved and never appear.
///
/// The underlying type is uint16_t so the two ranges are representable.
/// The enum is not uint8_t because a single-byte opcode and an extended
/// opcode would then share a numeric value, which is an alias in the
/// enum — the exact bug this revision fixes.
enum class Opcode : uint16_t {
    // ═══════════════════════════════════════════════════════════════════════
    // Single-byte opcodes: 0x0001 .. 0x00FF
    // ═══════════════════════════════════════════════════════════════════════
    //
    // In the code stream, a single-byte opcode is written as one byte:
    // the low byte of its enum value. 0x00 is reserved as the escape
    // prefix and never appears here.

    // ─── No operation ───────────────────────────────────────────────────
    Nop              = 0x0001,  ///< no effect; carries no operand

    // ─── Load/store ─────────────────────────────────────────────────────
    LoadLocal        = 0x0002,  ///< u16 slot
    StoreLocal       = 0x0003,  ///< u16 slot
    LoadConst        = 0x0004,  ///< u32 constant index
    LoadStaticData   = 0x0005,  ///< u32 static-data offset
    StoreStaticData  = 0x0006,  ///< u32 static-data offset
    LoadFunction     = 0x0007,  ///< u32 function index (a code address)
    LoadField        = 0x0008,  ///< u16 column index (row ref on stack)
    StoreField       = 0x0009,  ///< u16 column index (row ref + value on stack)
    LoadRow          = 0x000A,  ///< u8 table index (index on stack)
    StoreRow         = 0x000B,  ///< u8 table index (index + value on stack)
    LoadIndex        = 0x000C,  ///< array (index on stack)
    StoreIndex       = 0x000D,  ///< array (index + value on stack)

    // ─── Arithmetic — signed integers ───────────────────────────────────
    Add_I8   = 0x0010, Add_I16 = 0x0011, Add_I32 = 0x0012, Add_I64 = 0x0013,
    Sub_I8   = 0x0014, Sub_I16 = 0x0015, Sub_I32 = 0x0016, Sub_I64 = 0x0017,
    Mul_I8   = 0x0018, Mul_I16 = 0x0019, Mul_I32 = 0x001A, Mul_I64 = 0x001B,
    Div_I8   = 0x001C, Div_I16 = 0x001D, Div_I32 = 0x001E, Div_I64 = 0x001F,
    Mod_I8   = 0x0020, Mod_I16 = 0x0021, Mod_I32 = 0x0022, Mod_I64 = 0x0023,
    Pow_I8   = 0x0024, Pow_I16 = 0x0025, Pow_I32 = 0x0026, Pow_I64 = 0x0027,

    // ─── Arithmetic — unsigned integers ─────────────────────────────────
    Add_U8   = 0x0030, Add_U16 = 0x0031, Add_U32 = 0x0032, Add_U64 = 0x0033,
    Sub_U8   = 0x0034, Sub_U16 = 0x0035, Sub_U32 = 0x0036, Sub_U64 = 0x0037,
    Mul_U8   = 0x0038, Mul_U16 = 0x0039, Mul_U32 = 0x003A, Mul_U64 = 0x003B,
    Div_U8   = 0x003C, Div_U16 = 0x003D, Div_U32 = 0x003E, Div_U64 = 0x003F,
    Mod_U8   = 0x0040, Mod_U16 = 0x0041, Mod_U32 = 0x0042, Mod_U64 = 0x0043,
    Pow_U8   = 0x0044, Pow_U16 = 0x0045, Pow_U32 = 0x0046, Pow_U64 = 0x0047,

    // ─── Arithmetic — floats ────────────────────────────────────────────
    Add_F32 = 0x0050, Add_F64 = 0x0051,
    Sub_F32 = 0x0052, Sub_F64 = 0x0053,
    Mul_F32 = 0x0054, Mul_F64 = 0x0055,
    Div_F32 = 0x0056, Div_F64 = 0x0057,
    Mod_F32 = 0x0058, Mod_F64 = 0x0059,
    Pow_F32 = 0x005A, Pow_F64 = 0x005B,

    // ─── Arithmetic — string ────────────────────────────────────────────
    Concat_Str = 0x0060,        ///< string + string

    // ─── Negation / not / bitwise-not ───────────────────────────────────
    Neg_I8  = 0x0070, Neg_I16 = 0x0071, Neg_I32 = 0x0072, Neg_I64 = 0x0073,
    Neg_F32 = 0x0074, Neg_F64 = 0x0075,
    Not_Bool = 0x0076,
    BitNot_I8 = 0x0077, BitNot_I16 = 0x0078,
    BitNot_I32 = 0x0079, BitNot_I64 = 0x007A,
    BitNot_U8 = 0x007B, BitNot_U16 = 0x007C,
    BitNot_U32 = 0x007D, BitNot_U64 = 0x007E,

    // ─── Comparison — Eq ────────────────────────────────────────────────
    Eq_I8   = 0x0080, Eq_I16 = 0x0081, Eq_I32 = 0x0082, Eq_I64 = 0x0083,
    Eq_U8   = 0x0084, Eq_U16 = 0x0085, Eq_U32 = 0x0086, Eq_U64 = 0x0087,
    Eq_F32  = 0x0088, Eq_F64 = 0x0089,
    Eq_Bool = 0x008A, Eq_Char = 0x008B, Eq_Str = 0x008C,
    Eq_RowRef = 0x008D,       ///< identity comparison on a &T
    Eq_Function = 0x008E,     ///< identity comparison on a function value

    // ─── Comparison — Ne ────────────────────────────────────────────────
    Ne_I8   = 0x0090, Ne_I16 = 0x0091, Ne_I32 = 0x0092, Ne_I64 = 0x0093,
    Ne_U8   = 0x0094, Ne_U16 = 0x0095, Ne_U32 = 0x0096, Ne_U64 = 0x0097,
    Ne_F32  = 0x0098, Ne_F64 = 0x0099,
    Ne_Bool = 0x009A, Ne_Char = 0x009B, Ne_Str = 0x009C,
    Ne_RowRef = 0x009D, Ne_Function = 0x009E,

    // ─── Comparison — Lt ────────────────────────────────────────────────
    Lt_I8   = 0x00A0, Lt_I16 = 0x00A1, Lt_I32 = 0x00A2, Lt_I64 = 0x00A3,
    Lt_U8   = 0x00A4, Lt_U16 = 0x00A5, Lt_U32 = 0x00A6, Lt_U64 = 0x00A7,
    Lt_F32  = 0x00A8, Lt_F64 = 0x00A9, Lt_Char = 0x00AA, Lt_Str = 0x00AB,

    // ─── Comparison — Le ────────────────────────────────────────────────
    Le_I8   = 0x00B0, Le_I16 = 0x00B1, Le_I32 = 0x00B2, Le_I64 = 0x00B3,
    Le_U8   = 0x00B4, Le_U16 = 0x00B5, Le_U32 = 0x00B6, Le_U64 = 0x00B7,
    Le_F32  = 0x00B8, Le_F64 = 0x00B9, Le_Char = 0x00BA, Le_Str = 0x00BB,

    // ─── Comparison — Gt ────────────────────────────────────────────────
    Gt_I8   = 0x00C0, Gt_I16 = 0x00C1, Gt_I32 = 0x00C2, Gt_I64 = 0x00C3,
    Gt_U8   = 0x00C4, Gt_U16 = 0x00C5, Gt_U32 = 0x00C6, Gt_U64 = 0x00C7,
    Gt_F32  = 0x00C8, Gt_F64 = 0x00C9, Gt_Char = 0x00CA, Gt_Str = 0x00CB,

    // ─── Comparison — Ge ────────────────────────────────────────────────
    Ge_I8   = 0x00D0, Ge_I16 = 0x00D1, Ge_I32 = 0x00D2, Ge_I64 = 0x00D3,
    Ge_U8   = 0x00D4, Ge_U16 = 0x00D5, Ge_U32 = 0x00D6, Ge_U64 = 0x00D7,
    Ge_F32  = 0x00D8, Ge_F64 = 0x00D9, Ge_Char = 0x00DA, Ge_Str = 0x00DB,

    // ─── Bitwise — And ──────────────────────────────────────────────────
    BitAnd_I8  = 0x00E0, BitAnd_I16 = 0x00E1,
    BitAnd_I32 = 0x00E2, BitAnd_I64 = 0x00E3,
    BitAnd_U8  = 0x00E4, BitAnd_U16 = 0x00E5,
    BitAnd_U32 = 0x00E6, BitAnd_U64 = 0x00E7,

    // ─── Bitwise — Or ───────────────────────────────────────────────────
    BitOr_I8  = 0x00E8, BitOr_I16 = 0x00E9,
    BitOr_I32 = 0x00EA, BitOr_I64 = 0x00EB,
    BitOr_U8  = 0x00EC, BitOr_U16 = 0x00ED,
    BitOr_U32 = 0x00EE, BitOr_U64 = 0x00EF,

    // ─── Bitwise — Xor ──────────────────────────────────────────────────
    BitXor_I8  = 0x00F0, BitXor_I16 = 0x00F1,
    BitXor_I32 = 0x00F2, BitXor_I64 = 0x00F3,
    BitXor_U8  = 0x00F4, BitXor_U16 = 0x00F5,
    BitXor_U32 = 0x00F6, BitXor_U64 = 0x00F7,

    // ─── Bitwise — Shl (signed) ─────────────────────────────────────────
    Shl_I8 = 0x00F8, Shl_I16 = 0x00F9, Shl_I32 = 0x00FA, Shl_I64 = 0x00FB,

    // ─── Bitwise — Shr (signed, arithmetic) ─────────────────────────────
    Shr_I8 = 0x00FC, Shr_I16 = 0x00FD, Shr_I32 = 0x00FE, Shr_I64 = 0x00FF,

    // ═══════════════════════════════════════════════════════════════════════
    // Extended opcodes: 0x0101 .. 0x01FF
    // ═══════════════════════════════════════════════════════════════════════
    //
    // In the code stream, an extended opcode is written as two bytes:
    // 0x00 (the escape prefix) followed by the low byte of its enum
    // value. The high byte of the enum value (0x01) is the marker that
    // distinguishes extended opcodes from single-byte opcodes; it is
    // never written to the stream.
    //
    // 0x0100 is reserved (it would mean "escape followed by 0x00",
    // which is not a valid encoding). The range starts at 0x0101.

    // ─── Bitwise — Shr (unsigned, logical) ──────────────────────────────
    Ext_Shr_U8  = 0x0101, Ext_Shr_U16 = 0x0102,
    Ext_Shr_U32 = 0x0103, Ext_Shr_U64 = 0x0104,

    // ─── Null handling ──────────────────────────────────────────────────
    Ext_IsNil    = 0x0110,  ///< pop value; push bool (is it nil?)
    Ext_Coalesce = 0x0111,  ///< full ?? lowering; used when LHS is not
                            ///< a simple load. See EmitExpr.
    Ext_CheckNil = 0x0112,  ///< pop value; panic with PanicNilDeref if nil

    // ─── Calls ──────────────────────────────────────────────────────────
    Ext_Call          = 0x0120,  ///< u32 function index; args on stack
    Ext_CallHost      = 0x0121,  ///< u32 host symbol index; args on stack
    Ext_CallTableMeth = 0x0122,  ///< u8 method tag; receiver + args on stack
    Ext_CallArrayMeth = 0x0123,  ///< u8 method tag; receiver + args on stack

    // ─── Aggregate — arrays ─────────────────────────────────────────────
    Ext_NewArray       = 0x0130,  ///< u32 element count
    Ext_NewFixedArray  = 0x0131,  ///< u32 element count
    Ext_ArrayAdd       = 0x0132,  ///< receiver + value on stack
    Ext_ArrayRemove    = 0x0133,  ///< receiver + index on stack
    Ext_ArrayClear     = 0x0134,
    Ext_ArraySort      = 0x0135,  ///< u8 flag: 0 = natural, 1 = comparator
    Ext_ArrayContains  = 0x0136,

    // ─── Aggregate — tables ─────────────────────────────────────────────
    Ext_TableAdd       = 0x0137,  ///< u32 table index; cells on stack
    Ext_TableRemove    = 0x0138,  ///< u32 table index; index on stack
    Ext_TableClear     = 0x0139,
    Ext_TableShrink    = 0x013A,
    Ext_TableCount     = 0x013B,
    Ext_TableVersion   = 0x013C,
    Ext_TableFind      = 0x013D,
    Ext_TableAt        = 0x013E,
    Ext_TableByPrimary = 0x013F,  ///< u32 table index, u16 column index

    // ─── Aggregate — column views ───────────────────────────────────────
    Ext_ColumnToArray  = 0x0140,

    // ─── Control ────────────────────────────────────────────────────────
    Ext_Jump         = 0x0150,  ///< i32 relative offset
    Ext_JumpIfFalse  = 0x0151,  ///< i32 relative offset
    Ext_JumpIfTrue   = 0x0152,  ///< i32 relative offset
    Ext_SwitchMember = 0x0153,  ///< u32 row-count; comparison table follows

    // ─── Sequences ──────────────────────────────────────────────────────
    Ext_SuspendWait           = 0x0160,  ///< u32 resume index
    Ext_SuspendWaitFrames     = 0x0161,
    Ext_SuspendWaitUntil      = 0x0162,
    Ext_SuspendWaitForEvent   = 0x0163,
    Ext_SuspendWaitForRequest = 0x0164,
    Ext_StartSequence         = 0x0165,  ///< u32 function index

    // ─── Return ─────────────────────────────────────────────────────────
    Ext_Return     = 0x0170,
    Ext_ReturnVoid = 0x0171,

    // ─── Panic ──────────────────────────────────────────────────────────
    Ext_Panic                    = 0x0180,  ///< u32 DiagCode; msg on stack
    Ext_PanicNilDeref            = 0x0181,
    Ext_PanicStaleRef            = 0x0182,
    Ext_PanicDuplicateKey        = 0x0183,
    Ext_PanicGenerationExhausted = 0x0184,

    // ─── Resource management ────────────────────────────────────────────
    Ext_Retain  = 0x0190,  ///< ResourceKind-driven; retain a value
    Ext_Release = 0x0191,  ///< ResourceKind-driven; release a value
    Ext_Copy    = 0x0192,  ///< deep copy per ResourceKind
};

// ─────────────────────────────────────────────────────────────────────────────
// Opcode arithmetic
// ─────────────────────────────────────────────────────────────────────────────
//
// The two numeric ranges are the contract. These helpers are the only
// sanctioned way to ask "which range is this opcode in?" — a caller
// that compares the raw value against a magic constant is a caller
// that will break when the ranges change.

/// The numeric marker for extended opcodes. A raw opcode value >= this
/// constant is an extended opcode; a value < this constant is a
/// single-byte opcode.
constexpr uint16_t OPCODE_EXTENDED_MARKER = 0x0100;

/// True if the opcode is a single-byte opcode (its stream encoding is
/// one byte).
constexpr bool isSingleByteOp(Opcode op) noexcept {
    return static_cast<uint16_t>(op) < OPCODE_EXTENDED_MARKER;
}

/// True if the opcode is an extended opcode (its stream encoding is
/// 0x00 followed by one byte).
constexpr bool isExtendedOp(Opcode op) noexcept {
    return static_cast<uint16_t>(op) >= OPCODE_EXTENDED_MARKER;
}

/// The byte that represents the opcode in the code stream.
///
///   - Single-byte opcode: the one byte to emit.
///   - Extended opcode: the second byte to emit, after the 0x00 prefix.
///
/// The caller that writes an extended opcode is responsible for
/// emitting the 0x00 prefix first; this helper returns only the second
/// byte, because the prefix is a property of the encoding, not of the
/// opcode.
constexpr uint8_t opcodeStreamByte(Opcode op) noexcept {
    return static_cast<uint8_t>(static_cast<uint16_t>(op) & 0x00FF);
}

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
    U8_U16,          ///< two operands: u8, then u16
    U32_U16,         ///< two operands: u32, then u16
    SwitchTable,     ///< u32 count, then count * (u32 case index + i32 offset)
};

/// @brief Static information about an opcode, used by the serializer,
///        the disassembler, and the interpreter's operand validator.
struct OpcodeInfo {
    Opcode       opcode;
    OperandShape shape;
    const char*  name;
};

/// @brief Look up an opcode's info.
///
/// Precondition: op is a valid opcode (its value is not 0x0000, not
/// 0x0100, and appears in either the single-byte or the extended
/// table). Callers that have a raw value from untrusted input should
/// validate with isSingleByteOpcode / isExtendedOpcode first.
///
/// The returned reference points into a static table and is valid for
/// the program's lifetime.
const OpcodeInfo& opcodeInfo(Opcode op) noexcept;

/// @brief True if the byte is a valid single-byte opcode.
///
/// A single-byte opcode is a byte value in 0x01..0xFF. The byte 0x00 is
/// the escape prefix and is not a valid opcode.
bool isSingleByteOpcode(uint8_t byte) noexcept;

/// @brief True if the byte is a valid extended opcode.
///
/// An extended opcode's byte is the second byte after a 0x00 prefix.
/// Its valid range is 0x01..0xFF (0x00 would mean "escape followed by
/// 0x00", which is not a valid encoding).
bool isExtendedOpcode(uint8_t byte) noexcept;

} // namespace lucid::bytecode