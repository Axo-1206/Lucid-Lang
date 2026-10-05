/// @file compile/CompilerContext.cpp
/// @brief Per-function compilation state and byte-emission primitives.

#include "CompilerContext.hpp"
#include "bytecode/compile/Compiler.hpp"
#include "TypeTranslation.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/memory/StringPool.hpp"

#include <algorithm>

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Opcode emission
// ─────────────────────────────────────────────────────────────────────────────
//
// Every instruction is written by calling emitOpcode followed by the
// operand-emitting calls that match the opcode's OperandShape. The
// caller is responsible for emitting the right operands in the right
// order; this class does not check that the operands match the shape.
//
// The invariant "operands match the opcode's shape" is checked by the
// interpreter's dispatch, and by a compiler-side validator. That check
// happens once per function, not once per emission.
//
// emitOpcode also updates the value-stack depth tracker. For a
// fixed-effect opcode, the update is automatic from OpcodeInfo. For a
// variable-effect opcode (pops == -1 or pushes == -1 in the table),
// emitOpcode skips the update; the emitter resolves the effect from
// the operand it just wrote and calls noteStackEffect.

void CompilerContext::emitOpcode(Opcode op) {
    // Write the opcode's stream encoding.
    if (isSingleByteOp(op)) {
        emitByte(opcodeStreamByte(op));
    } else {
        emitByte(0x00);
        emitByte(opcodeStreamByte(op));
    }

    // Update the value-stack depth and the ownership stack from the
    // opcode's table entry.
    const OpcodeInfo& info = opcodeInfo(op);
    if (info.pops >= 0 && info.pushes >= 0) {
        m_currentDepth -= info.pops;
        m_currentDepth += info.pushes;
        AST_ASSERT_MSG(m_currentDepth >= 0,
            "CompilerContext::emitOpcode: the value stack went "
            "negative — an earlier opcode's pop count was over-counted "
            "or its push count was under-counted");

        // Mirror the depth change on the ownership stack. Consumed
        // values lose their entry; produced values gain a default
        // BitCopy entry. The emitter overrides a produced entry to
        // Owned or Moved when it needs to (see markTopAsOwned and
        // markTopAsMoved).
        //
        // Ext_Dup is special-cased: it duplicates the top value but
        // does not consume it. Its table entry records pops=1,
        // pushes=2 for a naive depth update (net +1), but for
        // ownership the original entry must survive. So we push one
        // BitCopy entry for the copy and leave the original entry
        // untouched.
        if (op == Opcode::Ext_Dup) {
            m_owned.pushBitCopy();
        } else {
            for (int8_t i = 0; i < info.pops; ++i) {
                m_owned.pop();
            }
            for (int8_t i = 0; i < info.pushes; ++i) {
                m_owned.pushBitCopy();
            }
        }

        if (m_currentDepth > m_maxDepth) {
            m_maxDepth = m_currentDepth;
        }
    }
    // If pops or pushes is -1, the emitter will call noteStackEffect
    // once it has resolved the actual effect from the operand.
}

void CompilerContext::emitU8(uint8_t v) {
    m_code.push_back(v);
}

void CompilerContext::emitU16(uint16_t v) {
    // Little-endian, matching Serialize.cpp's convention for the
    // artifact's multi-byte integers.
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
// A forward jump is emitted with a placeholder operand and patched once
// the target is known. The patching writes the *relative* offset: the
// difference between the target's code offset and the position after
// the operand. The interpreter computes the target by adding the
// operand to the position after the operand.

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
// Stack-depth feedback
// ─────────────────────────────────────────────────────────────────────────────
//
// Called by the emitter for variable-effect opcodes, after the emitter
// has resolved the actual (pops, pushes). The two current call sites
// are EmitExpr's array-literal construction (NewArray / NewFixedArray)
// and EmitExpr's call site (Ext_Call). EmitCopy/EmitDrop also call this
// for Ext_RtCall.

void CompilerContext::noteStackEffect(int8_t pops, int8_t pushes) {
    AST_ASSERT_MSG(pops >= 0,
        "CompilerContext::noteStackEffect: pops must be non-negative "
        "— -1 is a table sentinel, not an argument");
    AST_ASSERT_MSG(pushes >= 0,
        "CompilerContext::noteStackEffect: pushes must be "
        "non-negative — -1 is a table sentinel, not an argument");

    m_currentDepth -= pops;
    m_currentDepth += pushes;

    AST_ASSERT_MSG(m_currentDepth >= 0,
        "CompilerContext::noteStackEffect: the value stack went "
        "negative — the emitter under-counted an opcode's pops or "
        "over-counted an earlier opcode's pushes");

    // Mirror the depth change on the ownership stack. A
    // variable-effect opcode is called via noteStackEffect rather
    // than emitOpcode; the bookkeeping is the same as emitOpcode's
    // general path. No special-case for Ext_Dup here: Ext_Dup is a
    // fixed-effect opcode and never reaches this method.
    for (int8_t i = 0; i < pops; ++i) {
        m_owned.pop();
    }
    for (int8_t i = 0; i < pushes; ++i) {
        m_owned.pushBitCopy();
    }

    if (m_currentDepth > m_maxDepth) {
        m_maxDepth = m_currentDepth;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Line table
// ─────────────────────────────────────────────────────────────────────────────
//
// A line entry records "the code at offset N corresponds to source
// location (file, line, column)." The emitters call noteLine before
// emitting the first instruction of a construct; the entry covers
// every instruction emitted until the next noteLine.
//
// The line table is strictly increasing in codeOffset. If the emitter
// calls noteLine twice at the same code offset (which happens when a
// construct's first instruction is also the first instruction of an
// enclosing construct), only the first entry is kept.

void CompilerContext::noteLine(SourceLocation loc, InternedString file) {
    const uint32_t currentOffset = here();
    if (!m_lineTable.empty()
        && m_lineTable.back().codeOffset == currentOffset) {
        return;
    }

    // Resolve the file path to an owned string. The InternedString is
    // valid at compile time but the FunctionProto outlives the
    // StringPool, so the line entry owns its file path.
    std::string filePath = m_compiler.pool().lookup(file);

    LineEntry entry;
    entry.codeOffset = currentOffset;
    entry.line       = loc.line();
    entry.column     = loc.column();
    entry.filePath   = std::move(filePath);
    m_lineTable.push_back(std::move(entry));
}

// ─────────────────────────────────────────────────────────────────────────────
// finalizeProto
// ─────────────────────────────────────────────────────────────────────────────

FunctionProto CompilerContext::finalizeProto() {
    // ─── Preconditions ─────────────────────────────────────────────────
    AST_ASSERT_MSG(m_currentFn != nullptr,
        "CompilerContext::finalizeProto: no function is being compiled");
    AST_ASSERT_MSG(!m_code.empty(),
        "CompilerContext::finalizeProto: the function's code is empty — "
        "every function has at least an implicit ReturnVoid");
    AST_ASSERT_MSG(m_currentDepth == 0,
        "CompilerContext::finalizeProto: the value stack is non-empty "
        "at the end of the function — an expression was left unconsumed "
        "by the emitter");

    // The ownership stack must match the value stack. Both being empty
    // is the strongest form of the invariant; a mismatch here means
    // the emitter left a value unaccounted for.
    m_owned.assertEmpty();

    // ─── Signature ─────────────────────────────────────────────────────
    //
    // The function's signature is built from its declared parameter
    // types and its declared return type. Both are Sema-resolved.
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
    // functions have none.
    std::vector<ResumeEntry> resumeTable;
    for (const auto& [index, slots] : m_slots.suspendPoints()) {
        ResumeEntry entry;
        entry.resumeIndex = index;
        entry.liveSlots   = slots;
        resumeTable.push_back(std::move(entry));
    }

    // ─── Stack depth ───────────────────────────────────────────────────
    //
    // The FunctionProto's maxStackDepth is the high-water mark of the
    // value stack during the function's emission. Clamp to at least 1
    // so the interpreter can always allocate at least one slot.
    const uint32_t maxStackDepth =
        static_cast<uint32_t>(std::max<int32_t>(m_maxDepth, 1));

    // ─── Assemble ──────────────────────────────────────────────────────
    return FunctionProto(m_compiler.pool().lookup(m_currentFn->mangledName),
                         std::move(sig),
                         std::move(m_code),
                         std::move(m_lineTable),
                         m_slots.localSlotCount(),
                         maxStackDepth,
                         m_currentFn->isSequence,
                         std::move(resumeTable));
}

} // namespace lucid::bytecode::compile