/// @file compile/EmitDecl.cpp
/// @brief Lower a declaration: artifact-level parts (pass A) and
///        per-function prologue (pass B).

#include "EmitDecl.hpp"
#include "../compile/BakeConstant.hpp"
#include "../compile/TypeTranslation.hpp"
#include "bytecode/compile/Compiler.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Pass A — artifact-level parts of a declaration
// ─────────────────────────────────────────────────────────────────────────────
//
// Emits no code. Produces StaticData entries, host symbols, and
// function-index reservations. The baking helpers below are called by
// emitDeclArtifacts and by nothing else.

namespace {

// ─── Baking a table ────────────────────────────────────────────────────────

void bakeTable(const TableDeclAST* table, ArtifactBuildState& state) {
    BakedTable baked;
    baked.mangledName = state.pool.lookup(table->mangledName);

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
        sym.name = state.pool.lookup(table->hostName);
        const uint32_t symIndex = state.hostSymbols.add(std::move(sym));
        baked.hostTypeSymbolIndex = static_cast<int32_t>(symIndex);
    } else {
        // ─── Columns ───────────────────────────────────────────────────
        //
        // Translate each column's declared type. Assert that the
        // column's resource kind matches the type's classification;
        // a mismatch means Sema's cached value is stale.
        for (const auto* col : table->columns) {
            BakedTable::Column c;
            c.mangledName = state.pool.lookup(col->mangledName);
            c.isUnique    = col->isUnique;
            c.isPrimary   = col->isPrimary;
            c.isReadonly  = col->isReadonly;

            AST_ASSERT_MSG(col->type != nullptr,
                "bakeTable: a column has no type — Sema should have "
                "resolved it");
            c.type = translateType(col->type, state.pool);

            baked.columns.push_back(std::move(c));
        }

        // ─── Rows ──────────────────────────────────────────────────────
        //
        // Each row's cells are folded constant values. Translate each
        // into a serializable Constant.
        for (const auto* row : table->rows) {
            std::vector<Constant> bakedRow;
            bakedRow.reserve(row->cells.size());
            for (size_t ci = 0; ci < row->cells.size(); ++ci) {
                const auto* cell = row->cells[ci];
                AST_ASSERT_MSG(cell->isConst,
                    "bakeTable: a table cell is not a constant "
                    "expression — Sema should have folded it");
                AST_ASSERT_MSG(cell->constValue.isEvaluated(),
                    "bakeTable: a table cell's constValue is not "
                    "evaluated — Sema should have folded it");

                const TypeDescriptor cellType =
                    (ci < baked.columns.size())
                        ? baked.columns[ci].type
                        : TypeDescriptor{};
                bakedRow.push_back(bakeConstant(state.pool, cell->constValue,
                                                cellType, UINT32_MAX));
            }
            baked.rows.push_back(std::move(bakedRow));
        }
    }

    state.staticData.tables().push_back(std::move(baked));
}

// ─── Baking a top-level binding ────────────────────────────────────────────

void bakeTopLevelBinding(const VarDeclAST* var, ArtifactBuildState& state) {
    AST_ASSERT_MSG(var->init != nullptr,
        "bakeTopLevelBinding: a top-level binding has no initializer — "
        "the grammar requires one");

    AST_ASSERT_MSG(var->type != nullptr,
        "bakeTopLevelBinding: a top-level binding has no type — "
        "Sema should have resolved it");

    AST_ASSERT_MSG(var->init->isConst,
        "bakeTopLevelBinding: a top-level initializer is not a "
        "constant expression — Sema should have folded it");
    AST_ASSERT_MSG(var->init->constValue.isEvaluated(),
        "bakeTopLevelBinding: a top-level initializer's constValue is "
        "not evaluated — Sema should have folded it");

    BakedBinding baked;
    baked.mangledName = state.pool.lookup(var->mangledName);
    baked.type        = translateType(var->type, state.pool);
    baked.initialValue = bakeConstant(state.pool, var->init->constValue,
                                      baked.type, UINT32_MAX);

    state.staticData.bindings().push_back(std::move(baked));
}

} // namespace

void emitDeclArtifacts(DeclAST* decl, ArtifactBuildState& state) {
    AST_ASSERT_MSG(decl != nullptr,
        "emitDeclArtifacts: null declaration — the driver should not "
        "have dispatched a null decl");

    // Skip a declaration produced by parser error recovery. Sema
    // rejects the module before the compiler runs, so this case is
    // defensive.
    if (decl->hasSyntaxError) {
        return;
    }

    // ─── Import ────────────────────────────────────────────────────────
    //
    // The manifest's per-module import list is populated by the driver
    // (it owns the manifest's Module entry). emitDeclArtifacts has
    // nothing to do for an import; the driver handles it directly.
    if (decl->isa<ImportDeclAST>()) {
        return;
    }

    // ─── Table ─────────────────────────────────────────────────────────
    if (decl->isa<TableDeclAST>()) {
        const auto* table = decl->as<TableDeclAST>();

        // Register the table's artifact index before baking, so the
        // index is correct even if the bake is a no-op (host-backed).
        const uint32_t tableIdx =
            static_cast<uint32_t>(state.staticData.tables().size());
        auto [it, inserted] = state.tableIndices.emplace(
            table->mangledName, tableIdx);
        AST_ASSERT_MSG(inserted,
            "emitDeclArtifacts: two tables share a mangled name — the "
            "mangling scheme produced a collision");

        bakeTable(table, state);
        return;
    }

    // ─── Function ──────────────────────────────────────────────────────
    if (decl->isa<FnDeclAST>()) {
        const auto* fn = decl->as<FnDeclAST>();

        if (fn->isHostBound) {
            // A host-bound function registers its host symbol and
            // nothing else. It has no FunctionProto (its body is a
            // single CallHost emitted by the call site, not a proto
            // of its own).
            HostSymbol sym;
            sym.kind = HostSymbol::Kind::Function;
            sym.name = state.pool.lookup(fn->hostName);
            state.hostSymbols.add(std::move(sym));
            return;
        }

        // A Lucid-bodied function reserves a FunctionProto index.
        // Pass B fills in the proto; for now, append a placeholder
        // so the index is stable.
        const uint32_t index =
            static_cast<uint32_t>(state.functions.size());
        auto [it, inserted] = state.functionIndex.emplace(fn, index);
        AST_ASSERT_MSG(inserted,
            "emitDeclArtifacts: a function was registered twice — the "
            "driver's pass A walked the same declaration twice");

        // Placeholder proto: a minimal legal code stream (a single
        // ReturnVoid). Pass B replaces it with the real proto.
        std::vector<uint8_t> code;
        code.push_back(0x00);                                // escape
        code.push_back(opcodeStreamByte(Opcode::Ext_ReturnVoid));

        FunctionSignature sig;
        state.functions.push_back(FunctionProto(
            state.pool.lookup(fn->mangledName),
            std::move(sig),
            std::move(code),
            {},                         // no line table
            0,                          // no locals
            1,                          // maxStackDepth
            fn->isSequence,
            {}));                       // no resume table
        return;
    }

    // ─── Top-level binding ─────────────────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        const auto* var = decl->as<VarDeclAST>();

        // Register the binding's offset before baking, so the offset
        // matches its eventual position in StaticData::bindings.
        const uint32_t bindingIdx =
            static_cast<uint32_t>(state.staticData.bindings().size());
        auto [it, inserted] = state.staticDataOffsets.emplace(
            var->mangledName, bindingIdx);
        AST_ASSERT_MSG(inserted,
            "emitDeclArtifacts: two bindings share a mangled name — "
            "the mangling scheme produced a collision");

        bakeTopLevelBinding(var, state);
        return;
    }

    // ─── Unknown / error-recovery ──────────────────────────────────────
    AST_ASSERT_MSG(false,
        "emitDeclArtifacts: unhandled declaration kind — the emitter "
        "is out of sync with the AST");
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass B — per-function prologue
// ─────────────────────────────────────────────────────────────────────────────

void emitDeclPrologue(FnDeclAST* fn, CompilerContext& ctx) {
    AST_ASSERT_MSG(fn != nullptr,
        "emitDeclPrologue: null function — the driver should not have "
        "dispatched a null decl");

    // ─── Preconditions ─────────────────────────────────────────────────
    AST_ASSERT_MSG(!fn->isHostBound,
        "emitDeclPrologue: a host-bound function reached the emitter — "
        "a host-bound function has no body to emit");
    AST_ASSERT_MSG(fn->body != nullptr,
        "emitDeclPrologue: a Lucid-bodied function has no body block — "
        "the parser should have produced one");
    AST_ASSERT_MSG(fn->returnType != nullptr,
        "emitDeclPrologue: a function has no resolved return type — "
        "Sema should have resolved it");

    // ─── Function's opening line entry ─────────────────────────────────
    //
    // Record the function's source location at code offset 0. This is
    // what a stack trace names when a panic occurs before any user
    // statement has been reached.
    ctx.noteLine(fn->loc, ctx.module()->filePath);

    // ─── Parameter slots ───────────────────────────────────────────────
    //
    // Allocate one slot per parameter, in declaration order. The
    // calling convention is: argument i arrives in slot i, for i in
    // [0, paramCount). The interpreter's frame setup matches this.
    //
    // Each parameter's resource kind is recorded alongside its slot,
    // so the scope-exit drop emission knows which parameters own
    // resources that must be released.
    for (auto* param : fn->params) {
        AST_ASSERT_MSG(param != nullptr,
            "emitDeclPrologue: a function has a null parameter node — "
            "the parser should have produced a real ParamAST");
        AST_ASSERT_MSG(param->type != nullptr,
            "emitDeclPrologue: a parameter has no resolved type — "
            "Sema should have resolved it");

        // Translate the parameter's type and record it on the slot.
        const TypeDescriptor type =
            translateType(param->type, ctx.compiler().pool());

        ctx.slots().allocateParam(param->name, type);
    }

    // ─── Return-type setup ─────────────────────────────────────────────
    //
    // Nothing to emit. The return type is recorded on the FunctionProto
    // by finalizeProto; the body's return statements are what actually
    // produce the value.
    //
    // The return type's resource kind matters at the call site, not
    // here: the caller decides whether to take ownership of the
    // returned value. The function itself doesn't drop its return
    // value — ownership transfers to the caller.

    // ─── Body ──────────────────────────────────────────────────────────
    //
    // emitDeclPrologue does NOT emit the body. The driver calls
    // emitStmt(fn->body, ctx) after this returns, then emits an
    // implicit ReturnVoid, then calls ctx.finalizeProto().
}

} // namespace lucid::bytecode::compile