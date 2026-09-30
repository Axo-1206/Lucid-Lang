/**
 * @file compile/CompilerContext.hpp
 *
 * @responsibility The per-function compilation state: the FunctionProto
 *                 under construction, the SlotAllocator, the constant
 *                 pool being filled, the host symbol table, and the
 *                 loop/label stacks.
 *
 * ─── Design: one context per function ─────────────────────────────────────
 * The driver constructs a fresh CompilerContext for each function it
 * lowers. The context holds references to the artifact-wide containers
 * (pool, host symbols, static data), and owns the function-local state
 * (slot allocator, code buffer, label stack).
 */

#pragma once

#include "SlotAllocator.hpp"
#include "../ConstantPool.hpp"
#include "../HostSymbolTable.hpp"
#include "../StaticData.hpp"
#include "../FunctionProto.hpp"

#include "core/ast/DeclAST.hpp"

#include <string>
#include <vector>

namespace lucid::bytecode::compile {

/// @brief One loop's break/continue targets, for `break label` /
///        `continue label`.
struct LoopContext {
    InternedString label;              ///< invalid if the loop is unlabeled
    uint32_t       continueTarget = 0; ///< code offset
    std::vector<uint32_t> breakJumps;  ///< code offsets to patch
};

class CompilerContext {
public:
    CompilerContext(ConstantPool&     pool,
                    HostSymbolTable&  hostSymbols,
                    StaticData&       staticData)
        : m_pool(pool)
        , m_hostSymbols(hostSymbols)
        , m_staticData(staticData) {}

    // ─── Artifact-wide containers ───────────────────────────────────────
    ConstantPool&    pool()         noexcept { return m_pool; }
    HostSymbolTable& hostSymbols()  noexcept { return m_hostSymbols; }
    StaticData&      staticData()   noexcept { return m_staticData; }

    // ─── Function-local state ───────────────────────────────────────────
    SlotAllocator&   slots()        noexcept { return m_slots; }
    std::vector<uint8_t>& code()    noexcept { return m_code; }
    FunctionProto&   proto()        noexcept { return m_proto; }

    // ─── The function currently being compiled ──────────────────────────
    FnDeclAST*       currentFn()    const noexcept { return m_currentFn; }
    void             setCurrentFn(FnDeclAST* fn) noexcept { m_currentFn = fn; }

    // ─── Loop stack ─────────────────────────────────────────────────────
    std::vector<LoopContext>& loops() noexcept { return m_loops; }

    // ─── Emit primitives ────────────────────────────────────────────────
    void emitByte(uint8_t b)                     { m_code.push_back(b); }
    void emitOpcode(Opcode op);
    void emitU8(uint8_t v);
    void emitU16(uint16_t v);
    void emitU32(uint32_t v);
    void emitI32(int32_t v);

    /// @brief Patch a u32 at a recorded offset (used for jump targets).
    void patchU32(uint32_t offset, uint32_t value);

    /// @brief The current code offset. Used to record jump targets.
    uint32_t here() const noexcept { return static_cast<uint32_t>(m_code.size()); }

    // ─── Line table ─────────────────────────────────────────────────────
    void noteLine(SourceLocation loc, InternedString file);

private:
    ConstantPool&    m_pool;
    HostSymbolTable& m_hostSymbols;
    StaticData&      m_staticData;

    SlotAllocator         m_slots;
    std::vector<uint8_t>  m_code;
    FunctionProto         m_proto;
    FnDeclAST*            m_currentFn = nullptr;
    std::vector<LoopContext> m_loops;
    std::vector<LineEntry>   m_lineTable;
};

} // namespace lucid::bytecode::compile