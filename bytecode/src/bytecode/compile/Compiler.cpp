/// @file bytecode/compile/Compiler.cpp
/// @brief The driver: walk a resolved module set, produce a Bytecode.

#include "bytecode/compile/Compiler.hpp"
#include "ArtifactBuildState.hpp"
#include "CompilerContext.hpp"
#include "bytecode/emit/EmitDecl.hpp"
#include "bytecode/emit/EmitStmt.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Compiler::Compiler(lucid::diag::DiagnosticEngine& diagnostics,
                   StringPool& pool)
    : m_diag(diagnostics)
    , m_pool(pool) {}

// ─────────────────────────────────────────────────────────────────────────────
// compile — the entry point
// ─────────────────────────────────────────────────────────────────────────────
//
// Two passes over the module set:
//
//   Pass A — collect and bake. For each module, in source order:
//     1. Record its imports in the Manifest.
//     2. Call emitDeclArtifacts for every top-level declaration.
//        This bakes tables and top-level bindings into StaticData,
//        registers host symbols, and reserves function indices.
//     3. Record each declaration's mangled name in the Manifest's
//        per-module lists.
//
//   Pass B — emit bodies. For each Lucid-bodied function:
//     1. Construct a CompilerContext.
//     2. Call emitDeclPrologue (parameter slots, opening line).
//     3. Call emitStmt on the body.
//     4. Emit an implicit ReturnVoid.
//     5. Finalize the proto and replace the pass-A placeholder.

Bytecode Compiler::compile(const std::vector<ModuleAST*>& modules) {
    // ─── Preconditions ─────────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        AST_ASSERT_MSG(module != nullptr,
            "Compiler::compile: a null module was passed — the caller "
            "must filter nulls before invoking the compiler");
        AST_ASSERT_MSG(!module->hasErrors,
            "Compiler::compile: a module with Sema errors was passed — "
            "the caller must not invoke the compiler on a module whose "
            "Sema run failed");
    }

    // ─── The artifact under construction ───────────────────────────────
    Manifest                   manifest;
    ConstantPool               constants;
    StaticData                 staticData;
    HostSymbolTable            hostSymbols;
    std::vector<FunctionProto> functions;

    // ─── The shared pass-A state ───────────────────────────────────────
    //
    // ArtifactBuildState holds references to the containers and the
    // three index maps. Constructed once and reused across all
    // modules; it owns nothing, so reuse is safe.
    ArtifactBuildState state{
        constants,
        staticData,
        hostSymbols,
        functions,
        m_pool,
        m_functionIndex,
        m_staticDataOffsets,
        m_tableIndices,
    };

    // ─── Pass A — collect and bake ─────────────────────────────────────
    for (ModuleAST* module : modules) {
        Manifest::Module modEntry;
        modEntry.modulePath = m_pool.lookup(module->filePath);

        // ─── Imports ───────────────────────────────────────────────────
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<ImportDeclAST>()) continue;
            const auto* imp = decl->as<ImportDeclAST>();
            Manifest::Module::Import m;
            m.alias      = m_pool.lookup(imp->alias);
            m.targetPath = m_pool.lookup(imp->path);
            modEntry.imports.push_back(std::move(m));
        }

        // ─── Declarations ──────────────────────────────────────────────
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (decl->hasSyntaxError) continue;

            // Artifact-level parts: table, binding, function
            // registration, or host symbol. No code.
            emitDeclArtifacts(decl, state);

            // Manifest bookkeeping: record the declaration's mangled
            // name in the module's per-kind list. The manifest is the
            // driver's, not emitDeclArtifacts's.
            if (decl->isa<TableDeclAST>()) {
                modEntry.tables.push_back(
                    m_pool.lookup(decl->mangledName));
            } else if (decl->isa<FnDeclAST>()) {
                modEntry.functions.push_back(
                    m_pool.lookup(decl->mangledName));
            } else if (decl->isa<VarDeclAST>()) {
                modEntry.bindings.push_back(
                    m_pool.lookup(decl->mangledName));
            }
        }

        manifest.modules.push_back(std::move(modEntry));
    }

    // ─── Pass B — emit function bodies ─────────────────────────────────
    for (ModuleAST* module : modules) {
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<FnDeclAST>()) continue;

            auto* fn = decl->as<FnDeclAST>();
            if (fn->isHostBound) continue;

            auto it = m_functionIndex.find(fn);
            AST_ASSERT_MSG(it != m_functionIndex.end(),
                "Compiler::compile: a Lucid-bodied function was not "
                "registered in pass A — the driver's two passes are "
                "out of sync");

            // Construct the per-function context. It holds references
            // to the artifact containers, to this Compiler (for index
            // lookups and the pool), and to the current module (for
            // line entries).
            CompilerContext ctx(constants, hostSymbols, staticData,
                                *this, module);
            ctx.setCurrentFn(fn);

            // Prologue: parameter slots (with resource kinds), the
            // function's opening line entry.
            emitDeclPrologue(fn, ctx);

            // Body.
            emitStmt(fn->body, ctx);

            // Implicit ReturnVoid. A function whose body already ended
            // in a Return emitted one; the extra is dead code the
            // interpreter never reaches.
            ctx.emitOpcode(Opcode::Ext_ReturnVoid);

            // Finalize: assemble the FunctionProto from the accumulated
            // code, line table, slot counts, resume table, and stack
            // depth. Replaces the pass-A placeholder.
            functions[it->second] = ctx.finalizeProto();
        }
    }

    // ─── Assemble the artifact ─────────────────────────────────────────
    return Bytecode(std::move(manifest),
                    std::move(constants),
                    std::move(staticData),
                    std::move(hostSymbols),
                    std::move(functions));
}

// ─────────────────────────────────────────────────────────────────────────────
// Artifact index lookups
// ─────────────────────────────────────────────────────────────────────────────

std::optional<uint32_t> Compiler::functionIndexOf(
    const FnDeclAST* fn) const {
    auto it = m_functionIndex.find(fn);
    if (it == m_functionIndex.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> Compiler::staticDataOffsetOf(
    InternedString mangledName) const {
    auto it = m_staticDataOffsets.find(mangledName);
    if (it == m_staticDataOffsets.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> Compiler::tableIndexOf(
    InternedString mangledName) const {
    auto it = m_tableIndices.find(mangledName);
    if (it == m_tableIndices.end()) return std::nullopt;
    return it->second;
}

} // namespace lucid::bytecode::compile