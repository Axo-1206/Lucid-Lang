/**
 * @file compile/CompilerContext.hpp
 *
 * @responsibility The per-function compilation state: the code buffer,
 *                 the slot allocator, the loop stack, the line table,
 *                 and the value-stack depth tracker.
 *
 * ─── Design: the context is per-function ──────────────────────────────────
 * The driver constructs one CompilerContext per Lucid-bodied function.
 * The context is not reused; a fresh one is constructed for each
 * function, and its state is discarded when finalizeProto returns.
 *
 * ─── Design: stack depth is tracked automatically ─────────────────────────
 * Every emitOpcode call updates m_currentDepth from the opcode's
 * OpcodeInfo entry (pops, pushes). A variable-effect opcode (a call, an
 * array construction, a table insert) has pops == -1 in the table; the
 * emitter resolves the actual effect and calls noteStackEffect with
 * the (pops, pushes) it knows.
 *
 * m_maxDepth is the high-water mark of m_currentDepth. finalizeProto
 * uses it as the FunctionProto's maxStackDepth.
 *
 * ─── Design: finalizeProto leaves nothing behind ──────────────────────────
 * finalizeProto asserts that m_currentDepth is 0 at the end. A
 * nonzero depth means an expression was left unconsumed — a compiler
 * bug. The assert catches a whole class of emitter mistakes.
 *
 * ─── Design: the context is not thread-safe ───────────────────────────────
 * Like every other compiler structure, the context is confined to the
 * thread that runs the compilation. A future parallel codegen would
 * give each thread its own context and merge the results.
 */

#pragma once

#include "SlotAllocator.hpp"
#include "../ConstantPool.hpp"
#include "../HostSymbolTable.hpp"
#include "../StaticData.hpp"
#include "../FunctionProto.hpp"
#include "../Opcode.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/SourceLocation.hpp"
#include "core/memory/InternedString.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::bytecode::compile {

class Compiler;   // forward

/// @brief One enclosing loop's break/continue targets.
struct LoopContext {
    /// The loop's label, if it has one. Invalid when the loop is
    /// unlabeled.
    InternedString label;

    /// The code offset that `continue` jumps to.
    uint32_t continueTarget = 0;

    /// The code offsets of every `break` jump in the loop body. Patched
    /// to the loop's end offset once the loop finishes.
    std::vector<uint32_t> breakJumps;
};

class CompilerContext {
public:
    CompilerContext(ConstantPool&     pool,
                    HostSymbolTable&  hostSymbols,
                    StaticData&       staticData,
                    Compiler&         compiler,
                    ModuleAST*        module)
        : m_pool(pool)
        , m_hostSymbols(hostSymbols)
        , m_staticData(staticData)
        , m_compiler(compiler)
        , m_module(module) {}

    // ─── Artifact-wide containers ───────────────────────────────────────
    ConstantPool&    pool()        noexcept { return m_pool; }
    HostSymbolTable& hostSymbols() noexcept { return m_hostSymbols; }
    StaticData&      staticData()  noexcept { return m_staticData; }

    // ─── The compiler ──────────────────────────────────────────────────
    // Reached for function-index lookups, static-data offsets, table
    // indices, and the session's StringPool.
    Compiler&        compiler()    noexcept { return m_compiler; }

    // ─── The current module ────────────────────────────────────────────
    // Used to build line entries (the module's file path).
    ModuleAST*       module()      const noexcept { return m_module; }

    // ─── Function-local state ──────────────────────────────────────────
    SlotAllocator&   slots()       noexcept { return m_slots; }
    std::vector<uint8_t>& code()   noexcept { return m_code; }

    // ─── The function currently being compiled ─────────────────────────
    FnDeclAST*       currentFn()   const noexcept { return m_currentFn; }
    void             setCurrentFn(FnDeclAST* fn) noexcept { m_currentFn = fn; }

    // ─── The loop stack ────────────────────────────────────────────────
    std::vector<LoopContext>& loops() noexcept { return m_loops; }

    // ─── Emission primitives ───────────────────────────────────────────
    //
    // emitOpcode updates the value-stack depth automatically, using the
    // opcode's OpcodeInfo entry. The operand-emitting calls (emitU8,
    // emitU16, ...) write bytes to the code buffer without touching
    // the depth; they are called by the emitter after emitOpcode.
    void emitByte(uint8_t b)      { m_code.push_back(b); }
    void emitOpcode(Opcode op);
    void emitU8(uint8_t v);
    void emitU16(uint16_t v);
    void emitU32(uint32_t v);
    void emitI32(int32_t v);

    /// Overwrite a u32 at a recorded code offset. Used to patch forward
    /// jumps and the switch table.
    void patchU32(uint32_t offset, uint32_t value);

    /// The current code offset — the position of the next byte to be
    /// emitted.
    uint32_t here() const noexcept {
        return static_cast<uint32_t>(m_code.size());
    }

    // ─── Stack-depth feedback ──────────────────────────────────────────
    //
    // For a variable-effect opcode (its OpcodeInfo has pops == -1 or
    // pushes == -1), emitOpcode cannot update the depth on its own.
    // The emitter resolves the actual effect from the operand it just
    // wrote (or from the receiver's type) and calls this method.
    //
    // For a fixed-effect opcode, emitOpcode handles the depth; the
    // emitter does NOT call this method.
    void noteStackEffect(int8_t pops, int8_t pushes);

    /// The high-water mark of the value stack during this function's
    /// emission. finalizeProto uses it as the FunctionProto's
    /// maxStackDepth.
    int32_t maxStackDepth() const noexcept { return m_maxDepth; }

    // ─── Line table ────────────────────────────────────────────────────
    void noteLine(SourceLocation loc, InternedString file);

    // ─── Finalization ──────────────────────────────────────────────────
    //
    // Assemble the FunctionProto from the accumulated code, line table,
    // slot counts, resume table, and stack depth. Called exactly once,
    // at the end of the function's emission.
    //
    // Preconditions (asserted):
    //   - m_currentFn is set.
    //   - m_code is non-empty (every function has an implicit
    //     ReturnVoid).
    //   - m_currentDepth is 0 (every expression has been consumed).
    FunctionProto finalizeProto();

private:
    ConstantPool&    m_pool;
    HostSymbolTable& m_hostSymbols;
    StaticData&      m_staticData;
    Compiler&        m_compiler;
    ModuleAST*       m_module;

    SlotAllocator            m_slots;
    std::vector<uint8_t>     m_code;
    FnDeclAST*               m_currentFn = nullptr;
    std::vector<LoopContext> m_loops;
    std::vector<LineEntry>   m_lineTable;

    /// The value-stack depth *before* the next instruction to be
    /// emitted. Updated by emitOpcode and noteStackEffect.
    int32_t m_currentDepth = 0;

    /// The high-water mark of m_currentDepth. Read by finalizeProto.
    int32_t m_maxDepth = 0;
};

} // namespace lucid::bytecode::compile