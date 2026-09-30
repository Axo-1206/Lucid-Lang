/// @file compile/CompilerContext.hpp
///
/// @responsibility The per-function compilation state: the FunctionProto
///                 under construction, the SlotAllocator, the code
///                 buffer, the loop/label stacks, and the line table.

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

#include <string>
#include <vector>

namespace lucid::bytecode::compile {

class Compiler;   // forward

struct LoopContext {
    InternedString label;
    uint32_t continueTarget = 0;
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

    // Artifact-wide containers
    ConstantPool&    pool()        noexcept { return m_pool; }
    HostSymbolTable& hostSymbols() noexcept { return m_hostSymbols; }
    StaticData&      staticData()  noexcept { return m_staticData; }

    // The compiler (for function-index lookups, string pool access)
    Compiler&        compiler()    noexcept { return m_compiler; }

    // The current module (for the manifest, for line entries)
    ModuleAST*       module()      const noexcept { return m_module; }

    // Function-local state
    SlotAllocator&   slots()       noexcept { return m_slots; }
    std::vector<uint8_t>& code()   noexcept { return m_code; }

    // The function currently being compiled
    FnDeclAST*       currentFn()   const noexcept { return m_currentFn; }
    void             setCurrentFn(FnDeclAST* fn) noexcept { m_currentFn = fn; }

    // Loop stack
    std::vector<LoopContext>& loops() noexcept { return m_loops; }

    // Emission primitives
    void emitByte(uint8_t b)      { m_code.push_back(b); }
    void emitOpcode(Opcode op);
    void emitU8(uint8_t v);
    void emitU16(uint16_t v);
    void emitU32(uint32_t v);
    void emitI32(int32_t v);

    void patchU32(uint32_t offset, uint32_t value);
    uint32_t here() const noexcept {
        return static_cast<uint32_t>(m_code.size());
    }

    void noteLine(SourceLocation loc, InternedString file);

    /// Finalize the FunctionProto under construction. Called at the
    /// end of the function's emission; returns the proto with the
    /// code, line table, slots, and resume table assembled.
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
};

} // namespace lucid::bytecode::compile