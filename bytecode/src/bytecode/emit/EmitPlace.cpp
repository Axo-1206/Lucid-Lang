/// @file compile/EmitPlace.cpp
/// @brief Resolve an lvalue to a *place* and emit the store.
///
/// ─── What a "place" is ────────────────────────────────────────────────────
/// An lvalue names a location that a store can write to. In a stack
/// machine, the emitter's job for an assignment `lhs = rhs` is:
///
///   1. Resolve `lhs` to a place. The place is a small piece of
///      description — "the value at frame slot N", "the column C of
///      the row whose reference is on the stack", "the element I of
///      the array whose value is on the stack" — plus whatever
///      computed operands that place needs (the row reference, the
///      array, the index).
///   2. Emit `rhs`, leaving its value on the stack.
///   3. Emit a store that consumes the place's operands and the value.
///
/// ─── Design: places are resolved, not emitted ─────────────────────────────
/// emitPlace does not leave a place "value" on the stack. It emits
/// whatever *operands* the store needs — the row ref, the index — and
/// the store opcode consumes them. There is no Place value type in the
/// opcode set. This is the simplest convention and the one the opcode
/// set was designed around (LoadField/StoreField take a column operand
/// and a row ref on the stack, LoadIndex/StoreIndex take an index on
/// the stack, etc.).
///
/// ─── Design: two functions, one responsibility each ───────────────────────
///   - emitPlace:          emit the operands a store needs (the row
///                         reference, the index). Does NOT emit the
///                         store opcode.
///   - emitStoreIntoPlace: emit the store opcode, given that the
///                         operands and the value are already on the
///                         stack.
///
/// The caller (EmitStmt's AssignStmtAST case) does:
///
///     emitPlace(lhs, ctx);      // operands for the store
///     emitExpr(rhs, ctx);       // the value
///     emitStoreIntoPlace(lhs, ctx);   // the store itself
///
/// This split lets the caller compute the RHS *after* the place's
/// operands, so a compound assignment `x += f()` can evaluate `x`'s
/// place operands once, evaluate `f()` once, add, and store — no
/// re-evaluation of the target.
///
/// ─── Design: the store kind is decided by the lvalue's resolved type ──────
/// Both functions read the lvalue's resolvedDecl and its resolvedType
/// (set by Sema) to decide what kind of place it is. There is no
/// re-derivation; the classification is already on the node.

#include "EmitPlace.hpp"
#include "EmitExpr.hpp"
#include "../compile/CompilerContext.hpp"
#include "bytecode/compile/Compiler.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"

namespace lucid::bytecode::compile {

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
    case ASTKind::IdentifierExpr: {
        auto* id = lhs->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr,
            "emitStoreIntoPlace: an lvalue identifier has no "
            "resolvedDecl");

        DeclAST* decl = id->resolvedDecl;

        if (decl->isa<ParamAST>() || decl->isa<VarDeclAST>()) {
            // A local or parameter: the slot allocator knows about it.
            auto slot = ctx.slots().slotFor(decl->name);
            if (slot.has_value()) {
                ctx.emitOpcode(Opcode::StoreLocal);
                ctx.emitU16(*slot);
                return;
            }
            // Otherwise, a top-level binding. Store into its static-
            // data slot.
            if (decl->isa<VarDeclAST>()) {
                const auto offset =
                    ctx.compiler().staticDataOffsetOf(decl->mangledName);
                AST_ASSERT_MSG(offset.has_value(),
                    "emitStoreIntoPlace: a top-level binding has no "
                    "static-data offset — the compiler's pass A did "
                    "not register it");
                ctx.emitOpcode(Opcode::StoreStaticData);
                ctx.emitU32(*offset);
                return;
            }
            AST_ASSERT_MSG(false,
                "emitStoreIntoPlace: a parameter has no slot — the "
                "function's prologue should have allocated one");
            return;
        }

        AST_ASSERT_MSG(false,
            "emitStoreIntoPlace: an lvalue identifier resolved to "
            "something other than a variable or parameter — Sema "
            "should have rejected this assignment");
        return;
    }

    // ─── Field access: row.column = value ──────────────────────────────
    case ASTKind::FieldAccessExpr: {
        auto* fa = lhs->as<FieldAccessExprAST>();
        AST_ASSERT_MSG(fa->resolvedColumn != nullptr,
            "emitPlace: a field-access lvalue has no resolvedColumn — "
            "Sema should have resolved it");

        // The store's stack operand is the row reference. Emit the
        // object expression; that's what puts the row ref on the
        // stack.
        //
        // The column index is an operand of the store opcode
        // (StoreField), not a stack value. So emitPlace emits only
        // the object.
        emitExpr(fa->object, ctx);
        return;
    }

    // ─── Index: arr[i] = value, or T[i] = ... (rejected by Sema) ───────
    case ASTKind::IndexExpr: {
        auto* ix = lhs->as<IndexExprAST>();

        // The store's stack operands are the array and the index.
        // Emit the array expression, then the index expression.
        // StoreIndex consumes them in order.
        emitExpr(ix->target, ctx);
        emitExpr(ix->index, ctx);
        return;
    }

    // ─── Everything else: not a legal lvalue ───────────────────────────
    // Sema sets isLValue only on the three forms above. Any other
    // expression reaching here means Sema accepted an illegal lvalue.
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

    // ─── Local store ───────────────────────────────────────────────────
    case ASTKind::IdentifierExpr: {
        auto* id = lhs->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr,
            "emitStoreIntoPlace: an lvalue identifier has no "
            "resolvedDecl");

        DeclAST* decl = id->resolvedDecl;

        if (decl->isa<ParamAST>() || decl->isa<VarDeclAST>()) {
            auto slot = ctx.slots().slotFor(decl->name);
            AST_ASSERT_MSG(slot.has_value(),
                "emitStoreIntoPlace: the target binding has no slot — "
                "its declaration was never emitted");
            ctx.emitOpcode(Opcode::StoreLocal);
            ctx.emitU16(*slot);
            return;
        }

        if (decl->isa<VarDeclAST>()) {
            AST_ASSERT_MSG(false,
                "emitStoreIntoPlace: storing to a top-level binding "
                "is not yet supported — it needs the static-data "
                "offset map (Phase 4 addition)");
            return;
        }

        AST_ASSERT_MSG(false,
            "emitStoreIntoPlace: unhandled identifier resolution");
        return;
    }

    // ─── Field store ───────────────────────────────────────────────────
    case ASTKind::FieldAccessExpr: {
        auto* fa = lhs->as<FieldAccessExprAST>();
        AST_ASSERT_MSG(fa->resolvedColumn != nullptr,
            "emitStoreIntoPlace: a field-access store has no "
            "resolvedColumn");
        const ColumnDeclAST* col = fa->resolvedColumn;

        ctx.emitOpcode(Opcode::StoreField);
        ctx.emitU16(static_cast<uint16_t>(col->columnIndex));
        return;
    }

    // ─── Index store ───────────────────────────────────────────────────
    case ASTKind::IndexExpr: {
        // Array element store. (Table row store is rejected by Sema
        // — a table's rows are referenced by &T, and the language
        // has no syntax for assigning to a row slot directly.)
        ctx.emitOpcode(Opcode::StoreIndex);
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