/// @file bytecode/compile/CompilerContext.cpp
/// @brief Per-function compilation state and byte-emission primitives.

#include "CompilerContext.hpp"
#include "Compiler.hpp"

#include "TypeTranslation.hpp"
#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/memory/StringPool.hpp"

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Opcode emission
// ─────────────────────────────────────────────────────────────────────────────
//
// Every instruction is written by calling emitOpcode followed by the
// operand-emitting calls that match the opcode's OperandShape. The
// caller is responsible for emitting the right operands in the right
// order; this class does not check that the operands match the shape
// (that would require re-deriving the shape from the Opcode enum on
// every emission, which the compiler already knows).
//
// The invariant "the operands match the opcode's shape" is checked
// after the fact: Dispatch.cpp in interp/, and a compiler-side
// validator, both walk the code and confirm that every opcode is
// followed by the right number of operand bytes. That check happens
// once per function, not once per emission.

void CompilerContext::emitOpcode(Opcode op) {
    // A single-byte opcode is written as its stream byte. An extended
    // opcode is written as 0x00 followed by its stream byte.
    if (isSingleByteOp(op)) {
        emitByte(opcodeStreamByte(op));
    } else {
        emitByte(0x00);
        emitByte(opcodeStreamByte(op));
    }
}

void CompilerContext::emitU8(uint8_t v) {
    m_code.push_back(v);
}

void CompilerContext::emitU16(uint16_t v) {
    // Little-endian, matching Serialize.cpp's convention for the
    // artifact's multi-byte integers. (The code stream is a separate
    // format from the artifact file, but they share the same
    // little-endian convention.)
    m_code.push_back(static_cast<uint8_t>(v & 0xFF));
    m_code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void CompilerContext::emitU32(uint32_t v) {
    m_code.push_back(static_cast<uint8_t>(v & 0xFF));
    m_code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    m_code.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    m_code.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void CompilerContext::emitI32(int32_t v) {
    emitU32(static_cast<uint32_t>(v));
}

// ─────────────────────────────────────────────────────────────────────────────
// Patching
// ─────────────────────────────────────────────────────────────────────────────
//
// A forward jump is emitted with a placeholder operand and patched
// once the target is known. The patching writes the *relative*
// offset: the difference between the target's code offset and the
// instruction's own operand position. The interpreter computes the
// target by adding the operand to the position after the operand.
//
// patchU32 writes a little-endian u32 at the given code offset. The
// caller recorded the offset when it emitted the placeholder. The
// patch overwrites exactly four bytes; it does not resize the code.

void CompilerContext::patchU32(uint32_t offset, uint32_t value) {
    AST_ASSERT_MSG(offset + 4 <= m_code.size(),
        "CompilerContext::patchU32: patch offset is past the end of "
        "the code buffer — the caller patched a jump that was never "
        "emitted");
    m_code[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    m_code[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    m_code[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    m_code[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// ─────────────────────────────────────────────────────────────────────────────
// Line table
// ─────────────────────────────────────────────────────────────────────────────
//
// A line entry records "the code at offset N corresponds to source
// location (file, line, column)." The emitters call noteLine before
// emitting the first instruction of a construct; the entry's
// codeOffset is the current code position, so the entry covers every
// instruction emitted until the next noteLine.
//
// The line table is strictly increasing in codeOffset. If the emitter
// calls noteLine twice at the same code offset (which happens when a
// construct's first instruction is also the first instruction of an
// enclosing construct), only the first entry is kept — the second
// would violate the monotonicity invariant FunctionProto checks.

void CompilerContext::noteLine(SourceLocation loc, InternedString file) {
    // Skip if we already have an entry for this exact offset. A
    // duplicate entry would break the binary search in
    // FunctionProto::locationAt.
    const uint32_t currentOffset = here();
    if (!m_lineTable.empty()
        && m_lineTable.back().codeOffset == currentOffset) {
        return;
    }

    // Resolve the file path to an owned string. The InternedString is
    // valid at compile time but the FunctionProto outlives the StringPool,
    // so the line entry owns its file path.
    std::string filePath = m_compiler.pool().lookup(file);

    LineEntry entry;
    entry.codeOffset = currentOffset;
    entry.line       = loc.line();
    entry.column     = loc.column();
    entry.filePath   = std::move(filePath);
    m_lineTable.push_back(std::move(entry));
}

// ─────────────────────────────────────────────────────────────────────────────
// CompilerContext::finalizeProto()
// ─────────────────────────────────────────────────────────────────────────────

FunctionProto CompilerContext::finalizeProto() {
    // ─── Preconditions ─────────────────────────────────────────────────
    AST_ASSERT_MSG(m_currentFn != nullptr,
        "CompilerContext::finalizeProto: no function is being compiled");
    AST_ASSERT_MSG(!m_code.empty(),
        "CompilerContext::finalizeProto: the function's code is empty — "
        "every function has at least an implicit ReturnVoid");

    // ─── Signature ─────────────────────────────────────────────────────
    //
    // The function's signature is built from its declared parameter
    // types and its declared return type. Both are Sema-resolved. The
    // finalize step translates them through the pool.
    //
    // The translation is done here rather than at emission start so
    // that a function whose body emission failed partway does not
    // leave a half-built signature in the artifact — finalizeProto
    // is only called on a successful emission.
    FunctionSignature sig;
    sig.params.reserve(m_currentFn->params.size());
    for (const auto* param : m_currentFn->params) {
        AST_ASSERT_MSG(param->type != nullptr,
            "CompilerContext::finalizeProto: a parameter has no "
            "resolved type — Sema should have resolved it");
        sig.params.push_back(translateType(param->type, m_compiler.pool()));
    }
    AST_ASSERT_MSG(m_currentFn->returnType != nullptr,
        "CompilerContext::finalizeProto: the function has no resolved "
        "return type — Sema should have resolved it (every FN writes "
        "'-> T', including '-> void')");
    sig.returnType = translateType(m_currentFn->returnType,
                                   m_compiler.pool());

    // ─── Resume table ──────────────────────────────────────────────────
    //
    // The slot allocator recorded a (resumeIndex, liveSlots) pair for
    // every suspend point in a @sequence function. Non-sequence
    // functions have none. The recording order is emission order,
    // which is source order; the resume table preserves it.
    std::vector<ResumeEntry> resumeTable;
    for (const auto& [index, slots] : m_slots.suspendPoints()) {
        ResumeEntry entry;
        entry.resumeIndex = index;
        entry.liveSlots   = slots;
        resumeTable.push_back(std::move(entry));
    }

    // ─── Assemble ──────────────────────────────────────────────────────
    //
    // FunctionProto's constructor runs checkInvariants, which
    // validates the name, code, line table monotonicity, resume table
    // consistency, and slot bounds.
    return FunctionProto(m_compiler.pool().lookup(m_currentFn->mangledName),
                         std::move(sig),
                         std::move(m_code),
                         std::move(m_lineTable),
                         m_slots.localSlotCount(),
                         m_slots.maxStackDepth(),
                         m_currentFn->isSequence,
                         std::move(resumeTable));
}

} // namespace lucid::bytecode::compile