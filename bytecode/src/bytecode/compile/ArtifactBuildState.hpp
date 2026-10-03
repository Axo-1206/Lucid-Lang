/**
 * @file compile/ArtifactBuildState.hpp
 *
 * @responsibility The artifact-wide state that pass A populates and
 *                 pass B reads. It bundles the artifact containers
 *                 under construction (ConstantPool, StaticData,
 *                 HostSymbolTable, FunctionProto list) with the
 *                 compiler's index maps (function index, static-data
 *                 offset, table index) and the session's StringPool.
 *
 * ─── Design: a struct, not a class ────────────────────────────────────────
 * ArtifactBuildState has no methods and no invariants of its own. It's
 * a bag of references the driver, the emitters, and the compiler's
 * helpers share. Any function that needs to touch the artifact under
 * construction takes it as a parameter.
 *
 * ─── Design: no per-function state ────────────────────────────────────────
 * ArtifactBuildState does NOT hold the code buffer, the slot allocator,
 * the loop stack, or the line table. Those are per-function and live in
 * CompilerContext. Splitting them here makes it obvious that a table or
 * a top-level binding has no per-function state to speak of — only
 * artifact-wide state.
 */

#pragma once

#include "bytecode/ConstantPool.hpp"
#include "bytecode/StaticData.hpp"
#include "bytecode/HostSymbolTable.hpp"
#include "bytecode/FunctionProto.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode::compile {

struct ArtifactBuildState {
    // ─── Artifact containers under construction ────────────────────────
    ConstantPool&               constants;
    StaticData&                 staticData;
    HostSymbolTable&            hostSymbols;
    std::vector<FunctionProto>& functions;

    // ─── The session's string pool ─────────────────────────────────────
    StringPool&                 pool;

    // ─── The compiler's index maps ─────────────────────────────────────
    // Populated by pass A; read by pass B.

    /// (FnDeclAST → FunctionProto index). Populated when a function is
    /// registered; the reserved index is filled in by pass B.
    std::unordered_map<const FnDeclAST*, uint32_t>& functionIndex;

    /// (mangled name → offset in StaticData::bindings).
    std::unordered_map<InternedString, uint32_t>&   staticDataOffsets;

    /// (mangled name → index in StaticData::tables).
    std::unordered_map<InternedString, uint32_t>&   tableIndices;
};

} // namespace lucid::bytecode::compile