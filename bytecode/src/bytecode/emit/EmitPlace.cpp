/// @file emit/EmitPlace.cpp
/// @brief Resolve an lvalue to a place and emit the store that writes
///        to it.
///
/// ─── The two functions ────────────────────────────────────────────────────
///   - emitPlace:          emit the *operands* a store needs (a row
///                         reference, an array, an index). Do NOT emit
///                         the store opcode.
///   - emitStoreIntoPlace: emit the store opcode, given that the
///                         operands (if any) and the value are already
///                         on the value stack.
///
/// The caller (EmitStmt's assignment case) does:
///
///     emitPlace(lhs, ctx);           // operands for the store
///     emitExpr(rhs, ctx);            // the value
///     emitStoreIntoPlace(lhs, ctx);  // the store itself
///
/// ─── Why emitPlace emits nothing for a local ──────────────────────────────
/// A local's store operand is the slot index, and the slot index is an
/// operand of the StoreLocal opcode, not a value on the value stack.
/// So a local place has zero stack operands and emitPlace emits
/// nothing. The same is true for a top-level binding's StoreStaticData:
/// the offset is an opcode operand, not a stack value.
///
/// Only field access and array index have stack operands: the row
/// reference for a field store, the array and index for an index
/// store. emitPlace emits those.
///
/// ─── Assignment drops ─────────────────────────────────────────────────────
/// If the target of an assignment holds a resource-typed value, that
/// value's resources must be released before it is overwritten.
/// emitStoreIntoPlace drops the old value first, for the target kinds
/// where the compiler owns the storage:
///
///   - Local slot:   drop the slot's current value before the store.
///   - Static-data:  the interpreter's load-time storage owns the old
///                   value; the store instruction replaces it and the
///                   interpreter drops the old one. No compiler-
///                   emitted drop here.
///   - Field cell:   the interpreter's StoreField drops the old cell
///                   value, using the column's schema. No compiler-
///                   emitted drop here.
///   - Array index:  same — the interpreter's StoreIndex drops the
///                   old element value using the array's element type.
///
/// So only the local case needs a compiler-emitted drop. The other
/// cases are handled by the interpreter, which knows the destination's
/// storage layout and schema.
///
/// ─── Compound assignment ──────────────────────────────────────────────────
/// Compound assignment (`x op= y`) is not yet implemented in
/// emitAssignStmt; it asserts. The intended lowering is:
///
///     x = x op y
///
/// with the place's operands evaluated once. For a local slot, the
/// lowering is LoadLocal <x>, <y>, <op>, then the ordinary store
/// sequence (which drops the old value). For a field or index lvalue,
/// the place's operands must be duplicated before the load, so the
/// same operands can be reused for the store. The drop-of-old-value
/// behavior of emitStoreIntoPlace applies unchanged: a compound
/// assignment to a resource-typed local drops the old value, exactly
/// as a plain assignment does.

#include "EmitPlace.hpp"
#include "EmitExpr.hpp"

#include "bytecode/compile/Compiler.hpp"
#include "bytecode/compile/CompilerContext.hpp"
#include "bytecode/compile/TypeTranslation.hpp"
#include "bytecode/memory/EmitDrop.hpp"
#include "bytecode/memory/ResourcePlan.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"

namespace lucid::bytecode::compile {

using memory::ResourcePlan;

// ─────────────────────────────────────────────────────────────────────────────
// emitPlace — the operands a store needs
// ─────────────────────────────────────────────────────────────────────────────

void emitPlace(ExprAST* lhs, CompilerContext& ctx) {
    AST_ASSERT_MSG(lhs != nullptr,
        "emitPlace: null lvalue — the caller should not have "
        "dispatched a null expr");

    AST_ASSERT_MSG(lhs->isLValue,
        "emitPlace: the expression is not an lvalue — Sema should have "
        "rejected the assignment before codegen");

    switch (lhs->kind) {

    // ─── Identifier: local, parameter, or top-level binding ────────────
    //
    // A local slot and a static-data offset are both opcode operands,
    // not stack values. So the place has no stack operands and emitPlace
    // emits nothing. The store opcode (StoreLocal / StoreStaticData)
    // carries the slot or offset directly.
    case ASTKind::IdentifierExpr: {
        auto* id = lhs->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr,
            "emitPlace: an lvalue identifier has no resolvedDecl — "
            "Sema should have resolved it");

        DeclAST* decl = id->resolvedDecl;

        // The decl is a ParamAST, a VarDeclAST, or (if Sema accepted
        // it, which it should not) something else. All cases emit no
        // place operands.
        AST_ASSERT_MSG(decl->isa<ParamAST>() || decl->isa<VarDeclAST>(),
            "emitPlace: an lvalue identifier resolved to something "
            "other than a variable or parameter — Sema should have "
            "rejected this assignment");
        return;
    }

    // ─── Field access: row.column = value ──────────────────────────────
    //
    // The store's stack operand is the row reference. Emit the object
    // expression; that puts the row ref on the stack. The column index
    // is an operand of the StoreField opcode.
    case ASTKind::FieldAccessExpr: {
        auto* fa = lhs->as<FieldAccessExprAST>();
        AST_ASSERT_MSG(fa->resolvedColumn != nullptr,
            "emitPlace: a field-access lvalue has no resolvedColumn — "
            "Sema should have resolved it");

        emitExpr(fa->object, ctx);
        return;
    }

    // ─── Index: arr[i] = value ─────────────────────────────────────────
    //
    // The store's stack operands are the array and the index. Emit
    // both; StoreIndex consumes them.
    case ASTKind::IndexExpr: {
        auto* ix = lhs->as<IndexExprAST>();

        emitExpr(ix->target, ctx);
        emitExpr(ix->index, ctx);
        return;
    }

    // ─── Everything else: not a legal lvalue ───────────────────────────
    default:
        AST_ASSERT_MSG(false,
            "emitPlace: the lvalue is not an identifier, field access, "
            "or index — Sema should have rejected the assignment");
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitStoreIntoPlace — the store opcode
// ─────────────────────────────────────────────────────────────────────────────

void emitStoreIntoPlace(ExprAST* lhs, CompilerContext& ctx) {
    AST_ASSERT_MSG(lhs != nullptr,
        "emitStoreIntoPlace: null lvalue");
    AST_ASSERT_MSG(lhs->isLValue,
        "emitStoreIntoPlace: the expression is not an lvalue");

    switch (lhs->kind) {

    // ─── Identifier: local, parameter, or top-level binding ────────────
    case ASTKind::IdentifierExpr: {
        auto* id = lhs->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr,
            "emitStoreIntoPlace: an lvalue identifier has no resolvedDecl");

        DeclAST* decl = id->resolvedDecl;

        // Local or parameter: the slot allocator knows it.
        auto slot = ctx.slots().slotFor(decl->name);
        if (slot.has_value()) {
            const TypeDescriptor& type = ctx.slots().typeOf(*slot);
            const memory::ResourcePlan plan = memory::planForType(type);

            if (plan.needsDropForStorage()) {
                ctx.emitOpcode(Opcode::LoadLocal);
                ctx.emitU16(*slot);
                ctx.owned().pushOwned();
                memory::emitDropIfOwned(ctx, plan);
            }

            ctx.emitOpcode(Opcode::StoreLocal);
            ctx.emitU16(*slot);
            ctx.owned().pop();
            return;
        }

        // Top-level binding: static-data slot.
        if (decl->isa<VarDeclAST>()) {
            const auto offset =
                ctx.compiler().staticDataOffsetOf(decl->mangledName);
            AST_ASSERT_MSG(offset.has_value(),
                "emitStoreIntoPlace: a top-level binding has no static-data offset");
            ctx.emitOpcode(Opcode::StoreStaticData);
            ctx.emitU32(*offset);
            ctx.owned().pop();
            return;
        }

        AST_ASSERT_MSG(false, "emitStoreIntoPlace: a parameter has no slot");
        return;
    }

    // ─── Field store: row.column = value ───────────────────────────────
    //
    // The old cell's value is dropped by the interpreter's StoreField
    // operation, using the column's schema. No compiler-emitted drop
    // here.
    case ASTKind::FieldAccessExpr: {
        auto* fa = lhs->as<FieldAccessExprAST>();
        AST_ASSERT_MSG(fa->resolvedColumn != nullptr,
            "emitStoreIntoPlace: a field-access store has no resolvedColumn");
        const ColumnDeclAST* col = fa->resolvedColumn;

        ctx.emitOpcode(Opcode::StoreField);
        ctx.emitU16(static_cast<uint16_t>(col->columnIndex));

        // StoreField consumed the row reference (from emitPlace) and
        // the new value (from emitExpr). Pop both ownership entries.
        //
        // Order: emitPlace pushed the row ref's ownership first, then
        // emitExpr pushed the value's. StoreField pops them in reverse
        // (value, then row ref). Pop the value's entry, then the row
        // ref's entry.
        ctx.owned().pop();   // the value
        ctx.owned().pop();   // the row reference
        return;
    }

    // ─── Index store: arr[i] = value ───────────────────────────────────
    //
    // The old element's value is dropped by the interpreter's
    // StoreIndex operation, using the array's element type. No
    // compiler-emitted drop here.
    case ASTKind::IndexExpr: {
        ctx.emitOpcode(Opcode::StoreIndex);

        // StoreIndex consumed the array, the index, and the value.
        // Pop their ownership entries in reverse push order.
        ctx.owned().pop();   // the value
        ctx.owned().pop();   // the index
        ctx.owned().pop();   // the array
        return;
    }

    default:
        AST_ASSERT_MSG(false,
            "emitStoreIntoPlace: unhandled lvalue kind — "
            "emitPlace and emitStoreIntoPlace are out of sync");
        return;
    }
}

} // namespace lucid::bytecode::compile