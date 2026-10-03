/// @file bytecode/compile/Compiler.cpp
/// @brief The driver: walk a resolved module set, produce a Bytecode.

#include "Compiler.hpp"
#include "BakeConstant.hpp"
#include "CompilerContext.hpp"
#include "EmitDecl.hpp"
#include "EmitStmt.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"

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
//   Pass A — collect, bake, register. For each module, in source order:
//     1. Record its imports in the Manifest.
//     2. Bake each fixed/readonly table into StaticData, and register
//        the table's artifact index in m_tableIndices.
//     3. Bake each top-level let/const into StaticData, and register
//        the binding's offset in m_staticDataOffsets.
//     4. Register each host(...) reference in HostSymbolTable.
//     5. Register each Lucid-bodied FN, reserving its FunctionProto
//        index.
//
//   Pass B — emit function bodies. For each Lucid-bodied function:
//     1. Construct a CompilerContext.
//     2. Emit the prologue (emitDecl).
//     3. Emit the body (emitStmt).
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

    // ─── Pass A — collect, bake, register ──────────────────────────────
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

            // ─── Table ─────────────────────────────────────────────────
            if (decl->isa<TableDeclAST>()) {
                const auto* table = decl->as<TableDeclAST>();
                modEntry.tables.push_back(m_pool.lookup(table->mangledName));

                // Register the table's artifact index before baking,
                // so the map is populated even if the bake is a no-op
                // (a host-backed table has no rows to bake).
                const uint32_t tableIdx =
                    static_cast<uint32_t>(staticData.tables().size());
                registerTableIndex(table->mangledName, tableIdx);

                bakeTable(table, staticData, hostSymbols);
                continue;
            }

            // ─── Function ──────────────────────────────────────────────
            if (decl->isa<FnDeclAST>()) {
                const auto* fn = decl->as<FnDeclAST>();
                modEntry.functions.push_back(m_pool.lookup(fn->mangledName));

                if (fn->isHostBound) {
                    HostSymbol sym;
                    sym.kind = HostSymbol::Kind::Function;
                    sym.name = m_pool.lookup(fn->hostName);
                    hostSymbols.add(std::move(sym));
                    continue;
                }

                // Reserve the function's artifact index and append a
                // placeholder proto. Pass B replaces it.
                const uint32_t index =
                    static_cast<uint32_t>(functions.size());
                m_functionIndex.emplace(fn, index);
                functions.push_back(makePlaceholderProto(fn));
                continue;
            }

            // ─── Top-level binding ─────────────────────────────────────
            if (decl->isa<VarDeclAST>()) {
                const auto* var = decl->as<VarDeclAST>();
                modEntry.bindings.push_back(m_pool.lookup(var->mangledName));

                // Register the binding's offset before baking, so the
                // offset matches its eventual position in
                // StaticData::bindings.
                const uint32_t bindingIdx =
                    static_cast<uint32_t>(staticData.bindings().size());
                registerStaticDataOffset(var->mangledName, bindingIdx);

                bakeTopLevelBinding(var, staticData);
                continue;
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

            CompilerContext ctx(constants, hostSymbols, staticData,
                                *this, module);
            ctx.setCurrentFn(fn);

            emitDecl(fn, ctx);
            emitStmt(fn->body, ctx);
            ctx.emitOpcode(Opcode::Ext_ReturnVoid);

            functions[it->second] = ctx.finalizeProto();
        }
    }

    // ─── Assemble the artifact ─────────────────────────────────────────
    Bytecode bc(std::move(manifest),
                std::move(constants),
                std::move(staticData),
                std::move(hostSymbols),
                std::move(functions));

    return bc;
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

// ─────────────────────────────────────────────────────────────────────────────
// Registration helpers (called from compile() during pass A)
// ─────────────────────────────────────────────────────────────────────────────

void Compiler::registerStaticDataOffset(InternedString mangledName,
                                        uint32_t offset) {
    AST_ASSERT_MSG(mangledName.isValid(),
        "Compiler::registerStaticDataOffset: an invalid mangled name — "
        "Sema's mangling pass should have run for every declaration");
    auto [it, inserted] = m_staticDataOffsets.emplace(mangledName, offset);
    AST_ASSERT_MSG(inserted,
        "Compiler::registerStaticDataOffset: two bindings share a "
        "mangled name — the mangling scheme produced a collision");
    (void)it;
}

void Compiler::registerTableIndex(InternedString mangledName,
                                  uint32_t index) {
    AST_ASSERT_MSG(mangledName.isValid(),
        "Compiler::registerTableIndex: an invalid mangled name");
    auto [it, inserted] = m_tableIndices.emplace(mangledName, index);
    AST_ASSERT_MSG(inserted,
        "Compiler::registerTableIndex: two tables share a mangled "
        "name — the mangling scheme produced a collision");
    (void)it;
}

// ─────────────────────────────────────────────────────────────────────────────
// Baking helpers
// ─────────────────────────────────────────────────────────────────────────────

void Compiler::bakeTable(const TableDeclAST* table,
                         StaticData& staticData,
                         HostSymbolTable& hostSymbols) {
    BakedTable baked;
    baked.mangledName = m_pool.lookup(table->mangledName);

    baked.isFixed      = table->isFixed;
    baked.isReadonly   = table->isReadonly;
    baked.isPacked     = table->isPacked;
    baked.isColumnar   = table->isColumnar;
    baked.isRequest    = table->isRequest;
    baked.isHostBacked = table->isHostBacked;

    if (table->isReserved) {
        baked.reservedCount = table->reservedCount;
    }

    if (table->isHostBacked) {
        HostSymbol sym;
        sym.kind = HostSymbol::Kind::Type;
        sym.name = m_pool.lookup(table->hostName);
        const uint32_t symIndex = hostSymbols.add(std::move(sym));
        baked.hostTypeSymbolIndex = static_cast<int32_t>(symIndex);
    } else {
        for (const auto* col : table->columns) {
            BakedTable::Column c;
            c.mangledName = m_pool.lookup(col->mangledName);
            c.isUnique    = col->isUnique;
            c.isPrimary   = col->isPrimary;
            c.isReadonly  = col->isReadonly;
            c.type        = makeUnknownTypeDescriptor();  // Phase 3 stub
            baked.columns.push_back(std::move(c));
        }

        for (const auto* row : table->rows) {
            std::vector<Constant> bakedRow;
            bakedRow.reserve(row->cells.size());
            for (size_t ci = 0; ci < row->cells.size(); ++ci) {
                const auto* cell = row->cells[ci];
                AST_ASSERT_MSG(cell->isConst,
                    "Compiler::bakeTable: a table cell is not a constant "
                    "expression — Sema should have folded it");
                AST_ASSERT_MSG(cell->constValue.isEvaluated(),
                    "Compiler::bakeTable: a table cell's constValue is "
                    "not evaluated — Sema should have folded it");

                const TypeDescriptor cellType =
                    (ci < baked.columns.size())
                        ? baked.columns[ci].type
                        : makeUnknownTypeDescriptor();
                bakedRow.push_back(bakeConstant(m_pool, cell->constValue,
                                                cellType, UINT32_MAX));
            }
            baked.rows.push_back(std::move(bakedRow));
        }
    }

    staticData.tables().push_back(std::move(baked));
}

void Compiler::bakeTopLevelBinding(const VarDeclAST* var,
                                   StaticData& staticData) {
    AST_ASSERT_MSG(var->init != nullptr,
        "Compiler::bakeTopLevelBinding: a top-level binding has no "
        "initializer — the grammar requires one");

    AST_ASSERT_MSG(var->init->isConst,
        "Compiler::bakeTopLevelBinding: a top-level initializer is not "
        "a constant expression — Sema should have folded it");
    AST_ASSERT_MSG(var->init->constValue.isEvaluated(),
        "Compiler::bakeTopLevelBinding: a top-level initializer's "
        "constValue is not evaluated — Sema should have folded it");

    BakedBinding baked;
    baked.mangledName = m_pool.lookup(var->mangledName);
    baked.type        = makeUnknownTypeDescriptor();  // Phase 3 stub
    baked.initialValue = bakeConstant(m_pool, var->init->constValue,
                                      baked.type, UINT32_MAX);

    staticData.bindings().push_back(std::move(baked));
}

// ─────────────────────────────────────────────────────────────────────────────
// Placeholders
// ─────────────────────────────────────────────────────────────────────────────

TypeDescriptor Compiler::makeUnknownTypeDescriptor() const {
    TypeDescriptor d;
    d.kind = TypeDescriptor::Kind::Unknown;
    return d;
}

FunctionProto Compiler::makePlaceholderProto(const FnDeclAST* fn) const {
    // A minimal legal code stream: a single ReturnVoid. Replaced in
    // pass B by the real proto.
    std::vector<uint8_t> code;
    code.push_back(0x00);                                // escape
    code.push_back(opcodeStreamByte(Opcode::Ext_ReturnVoid));

    FunctionSignature sig;   // empty; pass B fills in
    return FunctionProto(m_pool.lookup(fn->mangledName),
                         std::move(sig),
                         std::move(code),
                         {},                         // no line table
                         0,                          // no locals
                         1,                          // maxStackDepth
                         fn->isSequence,
                         {});                        // no resume table
}

} // namespace lucid::bytecode::compile