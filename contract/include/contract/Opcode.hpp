/**
 * @file Opcode.hpp
 *
 * @responsibility The instruction set. A closed enum, the operand
 *                 shapes, and the stack effects. The only file besides
 *                 Bytecode.hpp that interp/ includes from this folder.
 *
 * ─── Design: typed opcodes, not a dispatch-on-type at runtime ─────────────
 * Sema resolved every operand's type at compile time. The compiler
 * emits a typed opcode (Add_I32, Add_F64, ...) rather than a generic
 * Add with a type operand. The interpreter's dispatch is a flat switch
 * with no secondary type dispatch.
 *
 * ─── Design: two numeric ranges, disjoint by construction ─────────────────
 * The byte encoding uses one byte for most opcodes, and a two-byte
 * escape (0x00 followed by a second byte) for the rest. The Opcode
 * enum mirrors this with two numeric ranges:
 *
 *   - Single-byte opcodes: 0x0001 .. 0x00FF
 *       The low byte is the opcode's byte in the code stream. 0x00 is
 *       reserved as the escape prefix and is never a valid opcode, so
 *       the single-byte range starts at 0x0001.
 *
 *   - Extended opcodes:    0x0101 .. 0x01FF
 *       The low byte is the second byte of the two-byte encoding; the
 *       high byte (0x01) marks "this is an extended opcode". 0x0100 is
 *       reserved, so the extended range starts at 0x0101.
 *
 * A value in 0x00xx is a single-byte opcode; a value in 0x01xx is an
 * extended opcode. No separate tag field is needed.
 *
 * ─── Design: stack effects are part of the instruction set ────────────────
 * Every opcode has a fixed, or operand-determined, effect on the value
 * stack. OpcodeInfo records it (pops, pushes). The compiler's
 * CompilerContext uses it to track stack depth automatically; the
 * interpreter uses it to validate bytecode. A fixed-effect opcode has
 * non-negative pops and pushes. A variable-effect opcode (a call, an
 * array construction, a table insert) sets both to -1; the emitter
 * resolves the actual effect and calls noteStackEffect.
 *
 * ─── Design: no opcodes without a design ──────────────────────────────────
 * Every opcode in the enum is emitted by some emitter and has a
 * documented stack effect. There are no placeholder opcodes. When a
 * new subsystem is designed (resource management, say), its opcodes
 * are added then, with a real stack effect and a real emitter.
 */

#pragma once

#include <cstdint>

namespace lucid::contract {

// ─────────────────────────────────────────────────────────────────────────────
// Opcode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The instruction set.
///
/// Values 0x0001..0x00FF are single-byte opcodes; values 0x0101..0x01FF
/// are extended opcodes (a 0x00 prefix byte followed by the low byte in
/// the code stream). 0x0000 and 0x0100 are reserved and never appear.
enum class Opcode : uint16_t {
    // ═══════════════════════════════════════════════════════════════════════
    // Single-byte opcodes: 0x0001 .. 0x00FF
    // ═══════════════════════════════════════════════════════════════════════

    // ─── No operation ───────────────────────────────────────────────────
    Nop              = 0x0001,  ///< no effect

    // ─── Load/store ─────────────────────────────────────────────────────
    LoadLocal        = 0x0002,  ///< u16 slot
    StoreLocal       = 0x0003,  ///< u16 slot
    LoadConst        = 0x0004,  ///< u32 constant index
    LoadStaticData   = 0x0005,  ///< u32 static-data offset
    StoreStaticData  = 0x0006,  ///< u32 static-data offset
    LoadFunction     = 0x0007,  ///< u32 function index
    LoadField        = 0x0008,  ///< u16 column index; row ref on stack
    StoreField       = 0x0009,  ///< u16 column index; row ref + value on stack
    LoadRow          = 0x000A,  ///< u32 table index; row index on stack
    StoreRow         = 0x000B,  ///< u32 table index; row index + value on stack
    LoadIndex        = 0x000C,  ///< array + index on stack
    StoreIndex       = 0x000D,  ///< array + index + value on stack

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
    Eq_RowRef = 0x008D,
    Eq_Function = 0x008E,

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

    // ─── Bitwise — Shr (unsigned, logical) ──────────────────────────────
    Ext_Shr_U8  = 0x0101, Ext_Shr_U16 = 0x0102,
    Ext_Shr_U32 = 0x0103, Ext_Shr_U64 = 0x0104,

    // ─── Stack manipulation ─────────────────────────────────────────────
    Ext_Dup = 0x0110,  ///< duplicate the top of stack
    Ext_Pop = 0x0111,  ///< discard the top of stack

    // ─── Null handling ──────────────────────────────────────────────────
    Ext_IsNil    = 0x0112,  ///< pop value; push bool (is it nil?)
    Ext_Coalesce = 0x0113,  ///< full ?? lowering (see EmitExpr)
    Ext_CheckNil = 0x0114,  ///< pop value; panic with PanicNilDeref if nil

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
    Ext_ArrayContains  = 0x0136,  ///< receiver + element on stack

    // ─── Aggregate — tables ─────────────────────────────────────────────
    Ext_TableAdd       = 0x0137,  ///< u32 table index; cells on stack
    Ext_TableRemove    = 0x0138,  ///< u32 table index; index on stack
    Ext_TableClear     = 0x0139,  ///< u32 table index
    Ext_TableShrink    = 0x013A,  ///< u32 table index
    Ext_TableCount     = 0x013B,  ///< u32 table index
    Ext_TableVersion   = 0x013C,  ///< u32 table index
    Ext_TableFind      = 0x013D,  ///< u32 table index; predicate on stack
    Ext_TableAt        = 0x013E,  ///< u32 table index; index on stack
    Ext_TableByPrimary = 0x013F,  ///< u32 table index, u16 column index

    // ─── Aggregate — column views ───────────────────────────────────────
    Ext_ColumnToArray  = 0x0140,  ///< column view on stack

    // ─── Aggregate — array length ───────────────────────────────────────
    //
    // Ext_ArrayLength returns the number of elements in a dynamic
    // array. A fixed array's length is a compile-time constant and
    // never needs a runtime opcode.
    //
    // The name is Length, not Count: the grammar's array method is
    // LENGTH(), and the opcode matches the method it implements. The
    // table's row-count method keeps the name COUNT(), and its
    // opcode (Ext_TableCount) is unchanged.
    Ext_ArrayLength = 0x0143,   ///< array on stack; pushes its length

    // ─── Aggregate — fixed arrays ───────────────────────────────────────
    //
    // A fixed array is a value: its elements are inline, at fixed
    // offsets. These opcodes read and write one element by its
    // compile-time index. The index is an operand, not a stack
    // value; the array itself is on the stack (for a read) or is
    // followed by the element to store (for a write).
    //
    // Emitted only by the ElementWise copy/drop lowering for a
    // [N, T] where T owns a resource. A fixed array whose elements
    // own nothing is bit-copied as a whole; it never reaches these
    // opcodes.
    Ext_FixedArrayGet = 0x0141,   ///< u32 element index; array on stack
    Ext_FixedArraySet = 0x0142,   ///< u32 element index; array + value on stack

    // ─── Control ────────────────────────────────────────────────────────
    Ext_Jump         = 0x0150,  ///< i32 relative offset
    Ext_JumpIfFalse  = 0x0151,  ///< i32 relative offset
    Ext_JumpIfTrue   = 0x0152,  ///< i32 relative offset
    Ext_SwitchMember = 0x0153,  ///< u32 row-count; comparison table follows

    // ─── Sequences ──────────────────────────────────────────────────────
    Ext_SuspendWait           = 0x0160,  ///< u32 resume index
    Ext_SuspendWaitFrames     = 0x0161,  ///< u32 resume index
    Ext_SuspendWaitUntil      = 0x0162,  ///< u32 resume index
    Ext_SuspendWaitForEvent   = 0x0163,  ///< u32 resume index
    Ext_SuspendWaitForRequest = 0x0164,  ///< u32 resume index
    Ext_StartSequence         = 0x0165,  ///< u32 function index

    // ─── Return ─────────────────────────────────────────────────────────
    Ext_Return     = 0x0170,
    Ext_ReturnVoid = 0x0171,

    // ─── Runtime calls ──────────────────────────────────────────────────
    //
    // A runtime call invokes one of the language's own runtime
    // operations (retain, release, string free, array copy, panic).
    // The operand is a u8 selecting the RuntimeOp. The stack effect
    // depends on the operation; Ext_RtCall's OpcodeInfo entry has
    // pops = -1 and pushes = -1, and the emitter calls
    // noteStackEffect with the operation's resolved effect.
    Ext_RtCall = 0x0172,  ///< u8 RuntimeOp; args on stack

    // ─── Panic ──────────────────────────────────────────────────────────
    Ext_Panic                    = 0x0180,  ///< u32 DiagCode; msg on stack
    Ext_PanicNilDeref            = 0x0181,
    Ext_PanicStaleRef            = 0x0182,
    Ext_PanicDuplicateKey        = 0x0183,
    Ext_PanicGenerationExhausted = 0x0184,
};

// ─────────────────────────────────────────────────────────────────────────────
// Opcode arithmetic
// ─────────────────────────────────────────────────────────────────────────────

/// The numeric marker for extended opcodes.
constexpr uint16_t OPCODE_EXTENDED_MARKER = 0x0100;

/// True if the opcode is a single-byte opcode.
constexpr bool isSingleByteOp(Opcode op) noexcept {
    return static_cast<uint16_t>(op) < OPCODE_EXTENDED_MARKER;
}

/// True if the opcode is an extended opcode.
constexpr bool isExtendedOp(Opcode op) noexcept {
    return static_cast<uint16_t>(op) >= OPCODE_EXTENDED_MARKER;
}

/// The byte that represents the opcode in the code stream.
///
///   - Single-byte opcode: the one byte to emit.
///   - Extended opcode: the second byte to emit, after the 0x00 prefix.
///
/// A caller that writes an extended opcode must emit the 0x00 prefix
/// first; this helper returns only the second byte.
constexpr uint8_t opcodeStreamByte(Opcode op) noexcept {
    return static_cast<uint8_t>(static_cast<uint16_t>(op) & 0x00FF);
}

// ─────────────────────────────────────────────────────────────────────────────
// OperandShape
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

// ─────────────────────────────────────────────────────────────────────────────
// OpcodeInfo
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Static information about an opcode.
///
/// Used by the serializer, the disassembler, the interpreter's operand
/// validator, and the compiler's stack-depth tracking.
struct OpcodeInfo {
    Opcode       opcode;
    OperandShape shape;
    const char*  name;

    /// The number of values this opcode pops off the value stack.
    /// -1 means the effect depends on an operand; the emitter calls
    /// noteStackEffect with the resolved count.
    int8_t pops;

    /// The number of values this opcode pushes onto the value stack.
    /// -1 means the effect depends on an operand.
    int8_t pushes;
};

/// @brief Look up an opcode's info.
///
/// Precondition: op is a valid opcode. Callers that have a raw value
/// from untrusted input should validate with isSingleByteOpcode /
/// isExtendedOpcode first.
const OpcodeInfo& opcodeInfo(Opcode op) noexcept;

/// @brief True if the byte is a valid single-byte opcode.
bool isSingleByteOpcode(uint8_t byte) noexcept;

/// @brief True if the byte is a valid extended opcode.
bool isExtendedOpcode(uint8_t byte) noexcept;

/// @brief Validate the opcode tables against the Opcode enum.
///
/// Fires an assert if any enum value has no table row, or if a table
/// row's `opcode` field does not match the byte at which it is stored.
/// Call once at startup (the interpreter's load path, or a test's
/// setup) to catch enum/table drift early.
void checkOpcodeTable();

} // namespace lucid::contract