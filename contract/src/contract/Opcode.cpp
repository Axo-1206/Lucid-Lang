/// @file contract/Opcode.cpp
/// @brief The opcode info table and the byte-classification helpers.

#include "contract/Opcode.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <array>

namespace lucid::contract {

// ─────────────────────────────────────────────────────────────────────────────
// The opcode tables
// ─────────────────────────────────────────────────────────────────────────────
//
// Two tables, one per numeric range of the Opcode enum:
//
//   - SINGLE_BYTE_INFO: indexed by the opcode's stream byte (0x01..0xFF).
//   - EXTENDED_INFO:    indexed by the opcode's second byte (0x01..0xFF).
//
// Each row is an OpcodeInfo with the opcode's enum value, its operand
// shape, its name, and its stack effect (pops, pushes). An entry whose
// name is nullptr means "this byte is not a valid opcode."
//
// The stack effect is the opcode's *value stack* effect: how many
// values it pops and how many it pushes. A fixed-effect opcode has
// non-negative pops and pushes. A variable-effect opcode (a call, an
// array construction, a table insert, an array sort with a comparator)
// has -1 for the variable dimension(s); the emitter calls
// noteStackEffect with the resolved count.

namespace {

// ─── Single-byte table ───────────────────────────────────────────────────

constexpr std::array<OpcodeInfo, 256> SINGLE_BYTE_INFO = [] {
    std::array<OpcodeInfo, 256> t{};
    for (auto& e : t) {
        e = {Opcode::Nop, OperandShape::None, nullptr, 0, 0};
    }

    // 0x00 is the escape prefix; never a valid opcode.
    t[0x00] = {Opcode::Nop, OperandShape::None, nullptr, 0, 0};

    // ─── No operation ──────────────────────────────────────────────────
    t[0x01] = {Opcode::Nop, OperandShape::None, "Nop", 0, 0};

    // ─── Load/store ────────────────────────────────────────────────────
    t[0x02] = {Opcode::LoadLocal,       OperandShape::U16, "LoadLocal",  0, 1};
    t[0x03] = {Opcode::StoreLocal,      OperandShape::U16, "StoreLocal", 1, 0};
    t[0x04] = {Opcode::LoadConst,       OperandShape::U32, "LoadConst",  0, 1};
    t[0x05] = {Opcode::LoadStaticData,  OperandShape::U32, "LoadStaticData",  0, 1};
    t[0x06] = {Opcode::StoreStaticData, OperandShape::U32, "StoreStaticData", 1, 0};
    t[0x07] = {Opcode::LoadFunction,    OperandShape::U32, "LoadFunction",    0, 1};
    t[0x08] = {Opcode::LoadField,       OperandShape::U16, "LoadField",  1, 1};
    t[0x09] = {Opcode::StoreField,      OperandShape::U16, "StoreField", 2, 0};
    t[0x0A] = {Opcode::LoadRow,         OperandShape::U32, "LoadRow",  1, 1};
    t[0x0B] = {Opcode::StoreRow,        OperandShape::U32, "StoreRow", 2, 0};
    t[0x0C] = {Opcode::LoadIndex,       OperandShape::None, "LoadIndex",  2, 1};
    t[0x0D] = {Opcode::StoreIndex,      OperandShape::None, "StoreIndex", 3, 0};

    // ─── Signed integer arithmetic ─────────────────────────────────────
    t[0x10] = {Opcode::Add_I8,  OperandShape::None, "Add_I8",  2, 1};
    t[0x11] = {Opcode::Add_I16, OperandShape::None, "Add_I16", 2, 1};
    t[0x12] = {Opcode::Add_I32, OperandShape::None, "Add_I32", 2, 1};
    t[0x13] = {Opcode::Add_I64, OperandShape::None, "Add_I64", 2, 1};
    t[0x14] = {Opcode::Sub_I8,  OperandShape::None, "Sub_I8",  2, 1};
    t[0x15] = {Opcode::Sub_I16, OperandShape::None, "Sub_I16", 2, 1};
    t[0x16] = {Opcode::Sub_I32, OperandShape::None, "Sub_I32", 2, 1};
    t[0x17] = {Opcode::Sub_I64, OperandShape::None, "Sub_I64", 2, 1};
    t[0x18] = {Opcode::Mul_I8,  OperandShape::None, "Mul_I8",  2, 1};
    t[0x19] = {Opcode::Mul_I16, OperandShape::None, "Mul_I16", 2, 1};
    t[0x1A] = {Opcode::Mul_I32, OperandShape::None, "Mul_I32", 2, 1};
    t[0x1B] = {Opcode::Mul_I64, OperandShape::None, "Mul_I64", 2, 1};
    t[0x1C] = {Opcode::Div_I8,  OperandShape::None, "Div_I8",  2, 1};
    t[0x1D] = {Opcode::Div_I16, OperandShape::None, "Div_I16", 2, 1};
    t[0x1E] = {Opcode::Div_I32, OperandShape::None, "Div_I32", 2, 1};
    t[0x1F] = {Opcode::Div_I64, OperandShape::None, "Div_I64", 2, 1};
    t[0x20] = {Opcode::Mod_I8,  OperandShape::None, "Mod_I8",  2, 1};
    t[0x21] = {Opcode::Mod_I16, OperandShape::None, "Mod_I16", 2, 1};
    t[0x22] = {Opcode::Mod_I32, OperandShape::None, "Mod_I32", 2, 1};
    t[0x23] = {Opcode::Mod_I64, OperandShape::None, "Mod_I64", 2, 1};
    t[0x24] = {Opcode::Pow_I8,  OperandShape::None, "Pow_I8",  2, 1};
    t[0x25] = {Opcode::Pow_I16, OperandShape::None, "Pow_I16", 2, 1};
    t[0x26] = {Opcode::Pow_I32, OperandShape::None, "Pow_I32", 2, 1};
    t[0x27] = {Opcode::Pow_I64, OperandShape::None, "Pow_I64", 2, 1};

    // ─── Unsigned integer arithmetic ───────────────────────────────────
    t[0x30] = {Opcode::Add_U8,  OperandShape::None, "Add_U8",  2, 1};
    t[0x31] = {Opcode::Add_U16, OperandShape::None, "Add_U16", 2, 1};
    t[0x32] = {Opcode::Add_U32, OperandShape::None, "Add_U32", 2, 1};
    t[0x33] = {Opcode::Add_U64, OperandShape::None, "Add_U64", 2, 1};
    t[0x34] = {Opcode::Sub_U8,  OperandShape::None, "Sub_U8",  2, 1};
    t[0x35] = {Opcode::Sub_U16, OperandShape::None, "Sub_U16", 2, 1};
    t[0x36] = {Opcode::Sub_U32, OperandShape::None, "Sub_U32", 2, 1};
    t[0x37] = {Opcode::Sub_U64, OperandShape::None, "Sub_U64", 2, 1};
    t[0x38] = {Opcode::Mul_U8,  OperandShape::None, "Mul_U8",  2, 1};
    t[0x39] = {Opcode::Mul_U16, OperandShape::None, "Mul_U16", 2, 1};
    t[0x3A] = {Opcode::Mul_U32, OperandShape::None, "Mul_U32", 2, 1};
    t[0x3B] = {Opcode::Mul_U64, OperandShape::None, "Mul_U64", 2, 1};
    t[0x3C] = {Opcode::Div_U8,  OperandShape::None, "Div_U8",  2, 1};
    t[0x3D] = {Opcode::Div_U16, OperandShape::None, "Div_U16", 2, 1};
    t[0x3E] = {Opcode::Div_U32, OperandShape::None, "Div_U32", 2, 1};
    t[0x3F] = {Opcode::Div_U64, OperandShape::None, "Div_U64", 2, 1};
    t[0x40] = {Opcode::Mod_U8,  OperandShape::None, "Mod_U8",  2, 1};
    t[0x41] = {Opcode::Mod_U16, OperandShape::None, "Mod_U16", 2, 1};
    t[0x42] = {Opcode::Mod_U32, OperandShape::None, "Mod_U32", 2, 1};
    t[0x43] = {Opcode::Mod_U64, OperandShape::None, "Mod_U64", 2, 1};
    t[0x44] = {Opcode::Pow_U8,  OperandShape::None, "Pow_U8",  2, 1};
    t[0x45] = {Opcode::Pow_U16, OperandShape::None, "Pow_U16", 2, 1};
    t[0x46] = {Opcode::Pow_U32, OperandShape::None, "Pow_U32", 2, 1};
    t[0x47] = {Opcode::Pow_U64, OperandShape::None, "Pow_U64", 2, 1};

    // ─── Float arithmetic ──────────────────────────────────────────────
    t[0x50] = {Opcode::Add_F32, OperandShape::None, "Add_F32", 2, 1};
    t[0x51] = {Opcode::Add_F64, OperandShape::None, "Add_F64", 2, 1};
    t[0x52] = {Opcode::Sub_F32, OperandShape::None, "Sub_F32", 2, 1};
    t[0x53] = {Opcode::Sub_F64, OperandShape::None, "Sub_F64", 2, 1};
    t[0x54] = {Opcode::Mul_F32, OperandShape::None, "Mul_F32", 2, 1};
    t[0x55] = {Opcode::Mul_F64, OperandShape::None, "Mul_F64", 2, 1};
    t[0x56] = {Opcode::Div_F32, OperandShape::None, "Div_F32", 2, 1};
    t[0x57] = {Opcode::Div_F64, OperandShape::None, "Div_F64", 2, 1};
    t[0x58] = {Opcode::Mod_F32, OperandShape::None, "Mod_F32", 2, 1};
    t[0x59] = {Opcode::Mod_F64, OperandShape::None, "Mod_F64", 2, 1};
    t[0x5A] = {Opcode::Pow_F32, OperandShape::None, "Pow_F32", 2, 1};
    t[0x5B] = {Opcode::Pow_F64, OperandShape::None, "Pow_F64", 2, 1};

    // ─── String concat ─────────────────────────────────────────────────
    t[0x60] = {Opcode::Concat_Str, OperandShape::None, "Concat_Str", 2, 1};

    // ─── Unary ─────────────────────────────────────────────────────────
    t[0x70] = {Opcode::Neg_I8,   OperandShape::None, "Neg_I8",  1, 1};
    t[0x71] = {Opcode::Neg_I16,  OperandShape::None, "Neg_I16", 1, 1};
    t[0x72] = {Opcode::Neg_I32,  OperandShape::None, "Neg_I32", 1, 1};
    t[0x73] = {Opcode::Neg_I64,  OperandShape::None, "Neg_I64", 1, 1};
    t[0x74] = {Opcode::Neg_F32,  OperandShape::None, "Neg_F32", 1, 1};
    t[0x75] = {Opcode::Neg_F64,  OperandShape::None, "Neg_F64", 1, 1};
    t[0x76] = {Opcode::Not_Bool, OperandShape::None, "Not_Bool", 1, 1};
    t[0x77] = {Opcode::BitNot_I8,  OperandShape::None, "BitNot_I8",  1, 1};
    t[0x78] = {Opcode::BitNot_I16, OperandShape::None, "BitNot_I16", 1, 1};
    t[0x79] = {Opcode::BitNot_I32, OperandShape::None, "BitNot_I32", 1, 1};
    t[0x7A] = {Opcode::BitNot_I64, OperandShape::None, "BitNot_I64", 1, 1};
    t[0x7B] = {Opcode::BitNot_U8,  OperandShape::None, "BitNot_U8",  1, 1};
    t[0x7C] = {Opcode::BitNot_U16, OperandShape::None, "BitNot_U16", 1, 1};
    t[0x7D] = {Opcode::BitNot_U32, OperandShape::None, "BitNot_U32", 1, 1};
    t[0x7E] = {Opcode::BitNot_U64, OperandShape::None, "BitNot_U64", 1, 1};

    // ─── Eq ────────────────────────────────────────────────────────────
    t[0x80] = {Opcode::Eq_I8,       OperandShape::None, "Eq_I8",       2, 1};
    t[0x81] = {Opcode::Eq_I16,      OperandShape::None, "Eq_I16",      2, 1};
    t[0x82] = {Opcode::Eq_I32,      OperandShape::None, "Eq_I32",      2, 1};
    t[0x83] = {Opcode::Eq_I64,      OperandShape::None, "Eq_I64",      2, 1};
    t[0x84] = {Opcode::Eq_U8,       OperandShape::None, "Eq_U8",       2, 1};
    t[0x85] = {Opcode::Eq_U16,      OperandShape::None, "Eq_U16",      2, 1};
    t[0x86] = {Opcode::Eq_U32,      OperandShape::None, "Eq_U32",      2, 1};
    t[0x87] = {Opcode::Eq_U64,      OperandShape::None, "Eq_U64",      2, 1};
    t[0x88] = {Opcode::Eq_F32,      OperandShape::None, "Eq_F32",      2, 1};
    t[0x89] = {Opcode::Eq_F64,      OperandShape::None, "Eq_F64",      2, 1};
    t[0x8A] = {Opcode::Eq_Bool,     OperandShape::None, "Eq_Bool",     2, 1};
    t[0x8B] = {Opcode::Eq_Char,     OperandShape::None, "Eq_Char",     2, 1};
    t[0x8C] = {Opcode::Eq_Str,      OperandShape::None, "Eq_Str",      2, 1};
    t[0x8D] = {Opcode::Eq_RowRef,   OperandShape::None, "Eq_RowRef",   2, 1};
    t[0x8E] = {Opcode::Eq_Function, OperandShape::None, "Eq_Function", 2, 1};

    // ─── Ne ────────────────────────────────────────────────────────────
    t[0x90] = {Opcode::Ne_I8,       OperandShape::None, "Ne_I8",       2, 1};
    t[0x91] = {Opcode::Ne_I16,      OperandShape::None, "Ne_I16",      2, 1};
    t[0x92] = {Opcode::Ne_I32,      OperandShape::None, "Ne_I32",      2, 1};
    t[0x93] = {Opcode::Ne_I64,      OperandShape::None, "Ne_I64",      2, 1};
    t[0x94] = {Opcode::Ne_U8,       OperandShape::None, "Ne_U8",       2, 1};
    t[0x95] = {Opcode::Ne_U16,      OperandShape::None, "Ne_U16",      2, 1};
    t[0x96] = {Opcode::Ne_U32,      OperandShape::None, "Ne_U32",      2, 1};
    t[0x97] = {Opcode::Ne_U64,      OperandShape::None, "Ne_U64",      2, 1};
    t[0x98] = {Opcode::Ne_F32,      OperandShape::None, "Ne_F32",      2, 1};
    t[0x99] = {Opcode::Ne_F64,      OperandShape::None, "Ne_F64",      2, 1};
    t[0x9A] = {Opcode::Ne_Bool,     OperandShape::None, "Ne_Bool",     2, 1};
    t[0x9B] = {Opcode::Ne_Char,     OperandShape::None, "Ne_Char",     2, 1};
    t[0x9C] = {Opcode::Ne_Str,      OperandShape::None, "Ne_Str",      2, 1};
    t[0x9D] = {Opcode::Ne_RowRef,   OperandShape::None, "Ne_RowRef",   2, 1};
    t[0x9E] = {Opcode::Ne_Function, OperandShape::None, "Ne_Function", 2, 1};

    // ─── Lt ────────────────────────────────────────────────────────────
    t[0xA0] = {Opcode::Lt_I8,   OperandShape::None, "Lt_I8",   2, 1};
    t[0xA1] = {Opcode::Lt_I16,  OperandShape::None, "Lt_I16",  2, 1};
    t[0xA2] = {Opcode::Lt_I32,  OperandShape::None, "Lt_I32",  2, 1};
    t[0xA3] = {Opcode::Lt_I64,  OperandShape::None, "Lt_I64",  2, 1};
    t[0xA4] = {Opcode::Lt_U8,   OperandShape::None, "Lt_U8",   2, 1};
    t[0xA5] = {Opcode::Lt_U16,  OperandShape::None, "Lt_U16",  2, 1};
    t[0xA6] = {Opcode::Lt_U32,  OperandShape::None, "Lt_U32",  2, 1};
    t[0xA7] = {Opcode::Lt_U64,  OperandShape::None, "Lt_U64",  2, 1};
    t[0xA8] = {Opcode::Lt_F32,  OperandShape::None, "Lt_F32",  2, 1};
    t[0xA9] = {Opcode::Lt_F64,  OperandShape::None, "Lt_F64",  2, 1};
    t[0xAA] = {Opcode::Lt_Char, OperandShape::None, "Lt_Char", 2, 1};
    t[0xAB] = {Opcode::Lt_Str,  OperandShape::None, "Lt_Str",  2, 1};

    // ─── Le ────────────────────────────────────────────────────────────
    t[0xB0] = {Opcode::Le_I8,   OperandShape::None, "Le_I8",   2, 1};
    t[0xB1] = {Opcode::Le_I16,  OperandShape::None, "Le_I16",  2, 1};
    t[0xB2] = {Opcode::Le_I32,  OperandShape::None, "Le_I32",  2, 1};
    t[0xB3] = {Opcode::Le_I64,  OperandShape::None, "Le_I64",  2, 1};
    t[0xB4] = {Opcode::Le_U8,   OperandShape::None, "Le_U8",   2, 1};
    t[0xB5] = {Opcode::Le_U16,  OperandShape::None, "Le_U16",  2, 1};
    t[0xB6] = {Opcode::Le_U32,  OperandShape::None, "Le_U32",  2, 1};
    t[0xB7] = {Opcode::Le_U64,  OperandShape::None, "Le_U64",  2, 1};
    t[0xB8] = {Opcode::Le_F32,  OperandShape::None, "Le_F32",  2, 1};
    t[0xB9] = {Opcode::Le_F64,  OperandShape::None, "Le_F64",  2, 1};
    t[0xBA] = {Opcode::Le_Char, OperandShape::None, "Le_Char", 2, 1};
    t[0xBB] = {Opcode::Le_Str,  OperandShape::None, "Le_Str",  2, 1};

    // ─── Gt ────────────────────────────────────────────────────────────
    t[0xC0] = {Opcode::Gt_I8,   OperandShape::None, "Gt_I8",   2, 1};
    t[0xC1] = {Opcode::Gt_I16,  OperandShape::None, "Gt_I16",  2, 1};
    t[0xC2] = {Opcode::Gt_I32,  OperandShape::None, "Gt_I32",  2, 1};
    t[0xC3] = {Opcode::Gt_I64,  OperandShape::None, "Gt_I64",  2, 1};
    t[0xC4] = {Opcode::Gt_U8,   OperandShape::None, "Gt_U8",   2, 1};
    t[0xC5] = {Opcode::Gt_U16,  OperandShape::None, "Gt_U16",  2, 1};
    t[0xC6] = {Opcode::Gt_U32,  OperandShape::None, "Gt_U32",  2, 1};
    t[0xC7] = {Opcode::Gt_U64,  OperandShape::None, "Gt_U64",  2, 1};
    t[0xC8] = {Opcode::Gt_F32,  OperandShape::None, "Gt_F32",  2, 1};
    t[0xC9] = {Opcode::Gt_F64,  OperandShape::None, "Gt_F64",  2, 1};
    t[0xCA] = {Opcode::Gt_Char, OperandShape::None, "Gt_Char", 2, 1};
    t[0xCB] = {Opcode::Gt_Str,  OperandShape::None, "Gt_Str",  2, 1};

    // ─── Ge ────────────────────────────────────────────────────────────
    t[0xD0] = {Opcode::Ge_I8,   OperandShape::None, "Ge_I8",   2, 1};
    t[0xD1] = {Opcode::Ge_I16,  OperandShape::None, "Ge_I16",  2, 1};
    t[0xD2] = {Opcode::Ge_I32,  OperandShape::None, "Ge_I32",  2, 1};
    t[0xD3] = {Opcode::Ge_I64,  OperandShape::None, "Ge_I64",  2, 1};
    t[0xD4] = {Opcode::Ge_U8,   OperandShape::None, "Ge_U8",   2, 1};
    t[0xD5] = {Opcode::Ge_U16,  OperandShape::None, "Ge_U16",  2, 1};
    t[0xD6] = {Opcode::Ge_U32,  OperandShape::None, "Ge_U32",  2, 1};
    t[0xD7] = {Opcode::Ge_U64,  OperandShape::None, "Ge_U64",  2, 1};
    t[0xD8] = {Opcode::Ge_F32,  OperandShape::None, "Ge_F32",  2, 1};
    t[0xD9] = {Opcode::Ge_F64,  OperandShape::None, "Ge_F64",  2, 1};
    t[0xDA] = {Opcode::Ge_Char, OperandShape::None, "Ge_Char", 2, 1};
    t[0xDB] = {Opcode::Ge_Str,  OperandShape::None, "Ge_Str",  2, 1};

    // ─── BitAnd ────────────────────────────────────────────────────────
    t[0xE0] = {Opcode::BitAnd_I8,  OperandShape::None, "BitAnd_I8",  2, 1};
    t[0xE1] = {Opcode::BitAnd_I16, OperandShape::None, "BitAnd_I16", 2, 1};
    t[0xE2] = {Opcode::BitAnd_I32, OperandShape::None, "BitAnd_I32", 2, 1};
    t[0xE3] = {Opcode::BitAnd_I64, OperandShape::None, "BitAnd_I64", 2, 1};
    t[0xE4] = {Opcode::BitAnd_U8,  OperandShape::None, "BitAnd_U8",  2, 1};
    t[0xE5] = {Opcode::BitAnd_U16, OperandShape::None, "BitAnd_U16", 2, 1};
    t[0xE6] = {Opcode::BitAnd_U32, OperandShape::None, "BitAnd_U32", 2, 1};
    t[0xE7] = {Opcode::BitAnd_U64, OperandShape::None, "BitAnd_U64", 2, 1};

    // ─── BitOr ─────────────────────────────────────────────────────────
    t[0xE8] = {Opcode::BitOr_I8,  OperandShape::None, "BitOr_I8",  2, 1};
    t[0xE9] = {Opcode::BitOr_I16, OperandShape::None, "BitOr_I16", 2, 1};
    t[0xEA] = {Opcode::BitOr_I32, OperandShape::None, "BitOr_I32", 2, 1};
    t[0xEB] = {Opcode::BitOr_I64, OperandShape::None, "BitOr_I64", 2, 1};
    t[0xEC] = {Opcode::BitOr_U8,  OperandShape::None, "BitOr_U8",  2, 1};
    t[0xED] = {Opcode::BitOr_U16, OperandShape::None, "BitOr_U16", 2, 1};
    t[0xEE] = {Opcode::BitOr_U32, OperandShape::None, "BitOr_U32", 2, 1};
    t[0xEF] = {Opcode::BitOr_U64, OperandShape::None, "BitOr_U64", 2, 1};

    // ─── BitXor ────────────────────────────────────────────────────────
    t[0xF0] = {Opcode::BitXor_I8,  OperandShape::None, "BitXor_I8",  2, 1};
    t[0xF1] = {Opcode::BitXor_I16, OperandShape::None, "BitXor_I16", 2, 1};
    t[0xF2] = {Opcode::BitXor_I32, OperandShape::None, "BitXor_I32", 2, 1};
    t[0xF3] = {Opcode::BitXor_I64, OperandShape::None, "BitXor_I64", 2, 1};
    t[0xF4] = {Opcode::BitXor_U8,  OperandShape::None, "BitXor_U8",  2, 1};
    t[0xF5] = {Opcode::BitXor_U16, OperandShape::None, "BitXor_U16", 2, 1};
    t[0xF6] = {Opcode::BitXor_U32, OperandShape::None, "BitXor_U32", 2, 1};
    t[0xF7] = {Opcode::BitXor_U64, OperandShape::None, "BitXor_U64", 2, 1};

    // ─── Shl (signed) ──────────────────────────────────────────────────
    t[0xF8] = {Opcode::Shl_I8,  OperandShape::None, "Shl_I8",  2, 1};
    t[0xF9] = {Opcode::Shl_I16, OperandShape::None, "Shl_I16", 2, 1};
    t[0xFA] = {Opcode::Shl_I32, OperandShape::None, "Shl_I32", 2, 1};
    t[0xFB] = {Opcode::Shl_I64, OperandShape::None, "Shl_I64", 2, 1};

    // ─── Shr (signed) ──────────────────────────────────────────────────
    t[0xFC] = {Opcode::Shr_I8,  OperandShape::None, "Shr_I8",  2, 1};
    t[0xFD] = {Opcode::Shr_I16, OperandShape::None, "Shr_I16", 2, 1};
    t[0xFE] = {Opcode::Shr_I32, OperandShape::None, "Shr_I32", 2, 1};
    t[0xFF] = {Opcode::Shr_I64, OperandShape::None, "Shr_I64", 2, 1};

    return t;
}();

// ─── Extended table ──────────────────────────────────────────────────────

constexpr std::array<OpcodeInfo, 256> EXTENDED_INFO = [] {
    std::array<OpcodeInfo, 256> t{};
    for (auto& e : t) {
        e = {Opcode::Nop, OperandShape::None, nullptr, 0, 0};
    }

    // ─── Shr (unsigned) ────────────────────────────────────────────────
    t[0x01] = {Opcode::Ext_Shr_U8,  OperandShape::None, "Shr_U8",  2, 1};
    t[0x02] = {Opcode::Ext_Shr_U16, OperandShape::None, "Shr_U16", 2, 1};
    t[0x03] = {Opcode::Ext_Shr_U32, OperandShape::None, "Shr_U32", 2, 1};
    t[0x04] = {Opcode::Ext_Shr_U64, OperandShape::None, "Shr_U64", 2, 1};

    // ─── Stack manipulation ────────────────────────────────────────────
    t[0x10] = {Opcode::Ext_Dup, OperandShape::None, "Dup", 1, 2};
    t[0x11] = {Opcode::Ext_Pop, OperandShape::None, "Pop", 1, 0};

    // ─── Null handling ─────────────────────────────────────────────────
    t[0x12] = {Opcode::Ext_IsNil,    OperandShape::None, "IsNil",    1, 1};
    t[0x13] = {Opcode::Ext_Coalesce, OperandShape::None, "Coalesce", 2, 1};
    t[0x14] = {Opcode::Ext_CheckNil, OperandShape::None, "CheckNil", 1, 1};

    // ─── Calls (variable stack effect) ─────────────────────────────────
    t[0x20] = {Opcode::Ext_Call,          OperandShape::U32, "Call",          -1, -1};
    t[0x21] = {Opcode::Ext_CallHost,      OperandShape::U32, "CallHost",      -1, -1};
    t[0x22] = {Opcode::Ext_CallTableMeth, OperandShape::U8,  "CallTableMeth", -1, -1};
    t[0x23] = {Opcode::Ext_CallArrayMeth, OperandShape::U8,  "CallArrayMeth", -1, -1};

    // ─── Array aggregate ───────────────────────────────────────────────
    t[0x30] = {Opcode::Ext_NewArray,      OperandShape::U32, "NewArray",      -1, 1};
    t[0x31] = {Opcode::Ext_NewFixedArray, OperandShape::U32, "NewFixedArray", -1, 1};
    t[0x32] = {Opcode::Ext_ArrayAdd,      OperandShape::None, "ArrayAdd",      2, 0};
    t[0x33] = {Opcode::Ext_ArrayRemove,   OperandShape::None, "ArrayRemove",   2, 0};
    t[0x34] = {Opcode::Ext_ArrayClear,    OperandShape::None, "ArrayClear",    1, 0};
    t[0x35] = {Opcode::Ext_ArraySort,     OperandShape::U8,  "ArraySort",     -1, 0};
    t[0x36] = {Opcode::Ext_ArrayContains, OperandShape::None, "ArrayContains", 2, 1};

    // ─── Table aggregate ───────────────────────────────────────────────
    t[0x37] = {Opcode::Ext_TableAdd,       OperandShape::U32,     "TableAdd",       -1, 1};
    t[0x38] = {Opcode::Ext_TableRemove,    OperandShape::U32,     "TableRemove",     1, 0};
    t[0x39] = {Opcode::Ext_TableClear,     OperandShape::U32,     "TableClear",      0, 0};
    t[0x3A] = {Opcode::Ext_TableShrink,    OperandShape::U32,     "TableShrink",     0, 0};
    t[0x3B] = {Opcode::Ext_TableCount,     OperandShape::U32,     "TableCount",      0, 1};
    t[0x3C] = {Opcode::Ext_TableVersion,   OperandShape::U32,     "TableVersion",    0, 1};
    t[0x3D] = {Opcode::Ext_TableFind,      OperandShape::U32,     "TableFind",       1, 1};
    t[0x3E] = {Opcode::Ext_TableAt,        OperandShape::U32,     "TableAt",         1, 1};
    t[0x3F] = {Opcode::Ext_TableByPrimary, OperandShape::U32_U16, "TableByPrimary",  1, 1};

    // ─── Column view ───────────────────────────────────────────────────
    t[0x40] = {Opcode::Ext_ColumnToArray, OperandShape::None, "ColumnToArray", 1, 1};

    // ─── Control ───────────────────────────────────────────────────────
    t[0x50] = {Opcode::Ext_Jump,         OperandShape::I32,         "Jump",         0, 0};
    t[0x51] = {Opcode::Ext_JumpIfFalse,  OperandShape::I32,         "JumpIfFalse",  1, 0};
    t[0x52] = {Opcode::Ext_JumpIfTrue,   OperandShape::I32,         "JumpIfTrue",   1, 0};
    t[0x53] = {Opcode::Ext_SwitchMember, OperandShape::SwitchTable, "SwitchMember", -1, 0};

    // ─── Sequences ─────────────────────────────────────────────────────
    t[0x60] = {Opcode::Ext_SuspendWait,           OperandShape::U32, "SuspendWait",           1, 0};
    t[0x61] = {Opcode::Ext_SuspendWaitFrames,     OperandShape::U32, "SuspendWaitFrames",     1, 0};
    t[0x62] = {Opcode::Ext_SuspendWaitUntil,      OperandShape::U32, "SuspendWaitUntil",      2, 0};
    t[0x63] = {Opcode::Ext_SuspendWaitForEvent,   OperandShape::U32, "SuspendWaitForEvent",   0, 0};
    t[0x64] = {Opcode::Ext_SuspendWaitForRequest, OperandShape::U32, "SuspendWaitForRequest", 1, 0};
    t[0x65] = {Opcode::Ext_StartSequence,         OperandShape::U32, "StartSequence",         0, 1};

    // ─── Return ────────────────────────────────────────────────────────
    t[0x70] = {Opcode::Ext_Return,     OperandShape::None, "Return",     1, 0};
    t[0x71] = {Opcode::Ext_ReturnVoid, OperandShape::None, "ReturnVoid", 0, 0};

    // ─── Runtime Call ───────────────────────────────────────────────────
    t[0x72] = {Opcode::Ext_RtCall, OperandShape::U8, "RtCall", -1, -1};

    // ─── Panic ─────────────────────────────────────────────────────────
    t[0x80] = {Opcode::Ext_Panic,                    OperandShape::U32, "Panic",                    1, 0};
    t[0x81] = {Opcode::Ext_PanicNilDeref,            OperandShape::None, "PanicNilDeref",           0, 0};
    t[0x82] = {Opcode::Ext_PanicStaleRef,            OperandShape::None, "PanicStaleRef",           0, 0};
    t[0x83] = {Opcode::Ext_PanicDuplicateKey,        OperandShape::None, "PanicDuplicateKey",       0, 0};
    t[0x84] = {Opcode::Ext_PanicGenerationExhausted, OperandShape::None, "PanicGenerationExhausted", 0, 0};

    return t;
}();

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool isSingleByteOpcode(uint8_t byte) noexcept {
    if (byte == 0x00) return false;
    return SINGLE_BYTE_INFO[byte].name != nullptr;
}

bool isExtendedOpcode(uint8_t byte) noexcept {
    if (byte == 0x00) return false;
    return EXTENDED_INFO[byte].name != nullptr;
}

const OpcodeInfo& opcodeInfo(Opcode op) noexcept {
    const uint8_t byte = opcodeStreamByte(op);

    if (isSingleByteOp(op)) {
        AST_ASSERT_MSG(SINGLE_BYTE_INFO[byte].name != nullptr,
            "opcodeInfo: single-byte opcode has no table entry — "
            "the Opcode enum and SINGLE_BYTE_INFO are out of sync");
        return SINGLE_BYTE_INFO[byte];
    } else {
        AST_ASSERT_MSG(EXTENDED_INFO[byte].name != nullptr,
            "opcodeInfo: extended opcode has no table entry — "
            "the Opcode enum and EXTENDED_INFO are out of sync");
        return EXTENDED_INFO[byte];
    }
}

void checkOpcodeTable() {
    // Every non-reserved byte in 0x01..0xFF that has a name must have
    // a matching Opcode enum value at that byte. Same for the extended
    // table.
    for (uint16_t b = 0x01; b <= 0xFF; ++b) {
        const OpcodeInfo& info = SINGLE_BYTE_INFO[b];
        if (info.name == nullptr) continue;
        AST_ASSERT_MSG(static_cast<uint16_t>(info.opcode) == b,
            "checkOpcodeTable: SINGLE_BYTE_INFO row's opcode field does "
            "not match the byte at which it is stored — the table and "
            "the enum have drifted");
    }
    for (uint16_t b = 0x01; b <= 0xFF; ++b) {
        const OpcodeInfo& info = EXTENDED_INFO[b];
        if (info.name == nullptr) continue;
        AST_ASSERT_MSG(
            static_cast<uint16_t>(info.opcode) == (0x0100 + b),
            "checkOpcodeTable: EXTENDED_INFO row's opcode field does "
            "not match the byte at which it is stored — the table and "
            "the enum have drifted");
    }
}

} // namespace lucid::contract