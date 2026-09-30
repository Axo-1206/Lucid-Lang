/// @file bytecode/compile/Compiler.cpp
/// @brief The driver: walk a resolved module set, produce a Bytecode.

#include "Compiler.hpp"
#include "BakeConstant.hpp"
#include "CompilerContext.hpp"
#include "EmitDecl.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"

#include <unordered_map>

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
// The driver walks the module set twice:
//
//   Pass A — for each module, in source order:
//     1. Collect the module's declarations into the Manifest's Module
//        entry (functions, bindings, tables).
//     2. Collect the module's imports into the Manifest.
//     3. Bake every fixed/readonly table's rows into StaticData.
//     4. Bake every top-level binding's initial value into StaticData.
//     5. Collect every host(...) reference into HostSymbolTable.
//     6. Register each Lucid-bodied FN for emission and reserve its
//        FunctionProto index.
//
//   Pass B — for each function in the work list, in registration order:
//     1. Create a CompilerContext for the function.
//     2. Emit the function's prologue (parameter slots, etc.).
//     3. Emit the function's body. (Phase 3; empty in Phase 2.)
//     4. Emit an implicit ReturnVoid if the body does not end in a
//        return.
//     5. Finalize the FunctionProto and append it to the function list.
//
// The two passes exist because a function may reference another
// function (a call, a function value in a fixed-table cell) before the
// referenced function's proto has been emitted. Pass A reserves every
// index; pass B fills them in. A forward reference in pass B resolves
// through the index map populated in pass A.

Bytecode Compiler::compile(const std::vector<ModuleAST*>& modules) {
    // ─── Preconditions ─────────────────────────────────────────────────
    //
    // Every module reaching the compiler must be Sema-validated. A
    // module with hasErrors set is a pipeline bug: the CLI should not
    // have invoked the compiler on it.
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
    Manifest        manifest;
    ConstantPool    constants;
    StaticData      staticData;
    HostSymbolTable hostSymbols;
    std::vector<FunctionProto> functions;
    std::unordered_map<const FnDeclAST*, uint32_t> functionIndex;

    // ─── Pass A — collect, bake, register ──────────────────────────────
    for (ModuleAST* module : modules) {
        // Every module gets a Manifest entry, in the order the modules
        // were passed to the compiler. The order is the source order
        // the CLI chose when it assembled the module list.
        Manifest::Module modEntry;
        modEntry.modulePath = m_pool.lookup(module->filePath);

        // Imports: the manifest records what each module imported, by
        // alias and target path. Sema already resolved the imports;
        // the manifest's job is to record them, not to re-resolve.
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<ImportDeclAST>()) continue;
            const auto* imp = decl->as<ImportDeclAST>();
            Manifest::Module::Import m;
            m.alias      = m_pool.lookup(imp->alias);
            m.targetPath = m_pool.lookup(imp->path);
            modEntry.imports.push_back(std::move(m));
        }

        // Declarations: walk the module's top-level declarations.
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (decl->hasSyntaxError) continue;

            // A table declaration contributes:
            //   - its name to the manifest's table list
            //   - its baked rows to StaticData (if it has an initializer)
            //   - its host type symbol to HostSymbolTable (if host-backed)
            if (decl->isa<TableDeclAST>()) {
                const auto* table = decl->as<TableDeclAST>();
                modEntry.tables.push_back(m_pool.lookup(table->mangledName));
                bakeTable(table, staticData, hostSymbols);
                continue;
            }

            // A function declaration contributes:
            //   - its name to the manifest's function list
            //   - its host symbol to HostSymbolTable (if host-bound)
            //   - a FunctionProto index reservation (if Lucid-bodied)
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

                // Reserve a FunctionProto index. The proto itself is
                // appended in pass B; for now we only record the
                // function's index so forward references can be
                // resolved.
                const uint32_t index = static_cast<uint32_t>(functions.size());
                functionIndex.emplace(fn, index);

                // Append a placeholder FunctionProto. It will be
                // replaced in pass B. The placeholder is valid (it
                // satisfies FunctionProto's invariants — see the
                // placeholder construction below), so a forward
                // reference from another function's emission can look
                // it up without tripping the invariant check.
                functions.push_back(makePlaceholderProto(fn));
                continue;
            }

            // A top-level variable declaration contributes:
            //   - its name to the manifest's binding list
            //   - its initial value to StaticData
            if (decl->isa<VarDeclAST>()) {
                const auto* var = decl->as<VarDeclAST>();
                modEntry.bindings.push_back(m_pool.lookup(var->mangledName));
                bakeTopLevelBinding(var, staticData);
                continue;
            }

            // An import is already handled above. No other declaration
            // kind exists at module level.
        }

        manifest.modules.push_back(std::move(modEntry));
    }

    // ─── Pass B — emit function bodies ─────────────────────────────────
    //
    // In Phase 2, the body emitters do not exist yet. Every function's
    // proto remains the placeholder constructed in pass A. In Phase 3,
    // this loop is where the emitters run: for each function, build a
    // CompilerContext, call EmitDecl to emit the prologue, call
    // EmitStmt on the body, emit an implicit ReturnVoid, and replace
    // the placeholder with the final proto.
    //
    // The loop is written here in Phase 2 (rather than deferred) so
    // the Phase 3 change is local to this function.

    for (ModuleAST* module : modules) {
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<FnDeclAST>()) continue;

            const auto* fn = decl->as<FnDeclAST>();
            if (fn->isHostBound) continue;

            auto it = functionIndex.find(fn);
            AST_ASSERT_MSG(it != functionIndex.end(),
                "Compiler::compile: a Lucid-bodied function was not "
                "registered in pass A — the driver's two passes are "
                "out of sync");

            // ── Phase 2: emit nothing ──
            //
            // Phase 3 will replace this block with:
            //
            //   CompilerContext ctx(constants, hostSymbols, staticData,
            //                       *this, module);
            //   ctx.setCurrentFn(fn);
            //   emitDecl(fn, ctx);
            //   emitStmt(fn->body, ctx);
            //   ctx.emitOpcode(Opcode::Ext_ReturnVoid);
            //   functions[it->second] = ctx.finalizeProto();
            //
            // For Phase 2, the placeholder remains. It is a valid
            // FunctionProto that names the function and carries an
            // empty-but-legal code stream.
            (void)it;
        }
    }

    // ─── Assemble the artifact ─────────────────────────────────────────
    Bytecode bc(std::move(manifest),
                std::move(constants),
                std::move(staticData),
                std::move(hostSymbols),
                std::move(functions));

    // The Bytecode constructor runs checkInvariants. Any cross-
    // component invariant violation fires there.
    return bc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Function index lookup
// ─────────────────────────────────────────────────────────────────────────────

std::optional<uint32_t> Compiler::functionIndexOf(const FnDeclAST* fn) const {
    auto it = m_functionIndex.find(fn);
    if (it == m_functionIndex.end()) return std::nullopt;
    return it->second;
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
        // A host-backed table has no columns and no rows. Its type
        // symbol goes into the host symbol table.
        HostSymbol sym;
        sym.kind = HostSymbol::Kind::Type;
        sym.name = m_pool.lookup(table->hostName);
        const uint32_t symIndex = hostSymbols.add(std::move(sym));
        baked.hostTypeSymbolIndex = static_cast<int32_t>(symIndex);
    } else {
        // A columned table: bake its columns and rows.
        for (const auto* col : table->columns) {
            BakedTable::Column c;
            c.mangledName = m_pool.lookup(col->mangledName);
            c.isUnique    = col->isUnique;
            c.isPrimary   = col->isPrimary;
            c.isReadonly  = col->isReadonly;

            // Translate the column's type. The type was resolved by
            // Sema; TypeDescriptor::fromTypeAST is a helper defined
            // in EmitDecl.cpp. In Phase 2 it does not exist yet; this
            // call is a placeholder.
            c.type = makeUnknownTypeDescriptor();
            baked.columns.push_back(std::move(c));
        }

        // Bake the table's initial rows. Each row's cells are
        // ConstantValue expressions folded by Sema. The BakeConstant
        // translation turns each into a Constant.
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
    baked.type        = makeUnknownTypeDescriptor();   // Phase 3: real type
    baked.initialValue = bakeConstant(m_pool, var->init->constValue,
                                      baked.type, UINT32_MAX);

    staticData.bindings().push_back(std::move(baked));
}

// ─────────────────────────────────────────────────────────────────────────────
// Placeholders
// ─────────────────────────────────────────────────────────────────────────────
//
// Phase 2 needs two things that will be provided by Phase 3:
//
//   1. A TypeDescriptor built from a resolved TypeAST.
//   2. A FunctionProto with the function's final code.
//
// In Phase 2, both are placeholders. The placeholders are valid values
// that satisfy their respective invariants, so the artifact assembles
// cleanly and can be inspected. When Phase 3 lands, the placeholders
// are replaced by real values.

TypeDescriptor Compiler::makeUnknownTypeDescriptor() const {
    TypeDescriptor d;
    d.kind = TypeDescriptor::Kind::Unknown;
    return d;
}

FunctionProto Compiler::makePlaceholderProto(const FnDeclAST* fn) const {
    // The placeholder proto names the function and carries a minimal
    // legal code stream: a single ReturnVoid instruction. Its
    // signature is empty for now (Phase 3 fills it in from the
    // function's resolved parameter and return types).
    //
    // A ReturnVoid is one byte: its stream encoding is 0x00 (escape)
    // followed by its low byte. That's two bytes total.
    std::vector<uint8_t> code;
    code.push_back(0x00);                                // escape
    code.push_back(opcodeStreamByte(Opcode::Ext_ReturnVoid));

    FunctionSignature sig;   // empty; Phase 3 fills in
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