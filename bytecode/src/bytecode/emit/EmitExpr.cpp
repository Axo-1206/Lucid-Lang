/// @file compile/EmitExpr.cpp
/// @brief Lower an expression into instructions, leaving its result on
///        the value stack.
///
/// ─── Scope of this file ───────────────────────────────────────────────────
/// emitExpr handles every ExprAST subclass. Assignments are NOT
/// expressions (grammar §12 makes assignment a statement), so there is
/// no AssignExprAST and nothing to handle here for assignment. Every
/// expression form in the grammar (§6) is handled below.
///
/// ─── Design: types are read, not re-derived ───────────────────────────────
/// Sema resolved every expression's type. The emitter reads
/// expr->resolvedType and translates it. It does not infer types and
/// does not re-check type compatibility. If a node's resolvedType is
/// null, that is a Sema bug and the assert fires.
///
/// ─── Design: typed opcodes, chosen from the resolved type ─────────────────
/// The opcode set has typed arithmetic, comparison, and bitwise
/// opcodes (Add_I32, Eq_F64, BitAnd_U16, ...). The emitter picks the
/// typed opcode from the operands' resolved type. A single helper
/// (arithmeticOpcodeFor, comparisonOpcodeFor, bitwiseOpcodeFor) maps
/// a BinaryOp or UnaryOp and a TypeDescriptor to an Opcode.
///
/// ─── Design: short-circuit operators lower to branches ────────────────────
/// `and`, `or`, and `??` do not have opcodes of their own. They lower
/// to a branch sequence that evaluates the RHS only when needed. The
/// lowering is in emitBinaryExpr's And/Or/NullCoalesce cases.

#include "../compile/BakeConstant.hpp"
#include "../compile/CompilerContext.hpp"
#include "../compile/EmitBinaryOps.hpp"
#include "../compile/TypeTranslation.hpp"
#include "bytecode/compile/Compiler.hpp"
#include "EmitExpr.hpp"
#include "EmitPlace.hpp"

#include "contract/ResourcePlan.hpp"
#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// True if a value of this type owns a resource the compiler must
/// track. A string, a dynamic array, a host handle, and a fixed array
/// of resources own a resource; primitives, row references, and
/// function values do not.
///
/// The classification is a pure function of the type. This is a
/// convenience wrapper around planForType's ownsResources() so the
/// emitters can write "if (ownsResource(t))" instead of threading a
/// ResourcePlan through.
bool ownsResource(const TypeDescriptor& t) {
    return planForType(t).ownsResources();
}

/// Emit a raw constant-pool index for a Constant. The bake happens
/// once; the pool's dedup map returns the same index for equal values.
uint32_t internConstant(CompilerContext& ctx,
                        const ConstantValue& value,
                        const TypeDescriptor& type) {
    // For a Kind::Function constant, the compiler's function-index
    // lookup resolves the FnDeclAST* to a FunctionProto index. The
    // other kinds pass UINT32_MAX as the sentinel (ignored).
    uint32_t fnIndex = UINT32_MAX;
    if (value.kind == ConstantValue::Kind::Function) {
        const FnDeclAST* fn = value.asFunction();
        AST_ASSERT_MSG(fn != nullptr,
            "internConstant: a Function-kind ConstantValue has a null "
            "FnDeclAST* — Sema should have resolved it");
        auto idx = ctx.compiler().functionIndexOf(fn);
        AST_ASSERT_MSG(idx.has_value(),
            "internConstant: a Function-kind ConstantValue names a "
            "function that was not registered with the compiler — "
            "the driver's two passes are out of sync");
        fnIndex = *idx;
    }

    Constant c = bakeConstant(ctx.compiler().pool(), value, type, fnIndex);
    return ctx.pool().add(std::move(c));
}

// ─── Table method dispatch ─────────────────────────────────────────────────

/// Emit a call to a table method: T.ADD(...), T.FIND(...), T.COUNT(),
/// T.VERSION(), T.AT(i), T.REMOVE(i), T.CLEAR(), T.SHRINK(),
/// T.by<Column>(v).
///
/// The receiver is a table, not a value on the stack — the opcode
/// carries the table's artifact index. The arguments are the method's
/// actual arguments, emitted in source order.
void emitTableMethodCall(CallExprAST* call,
                         FieldAccessExprAST* fa,
                         CompilerContext& ctx) {
    AST_ASSERT_MSG(fa->object->isa<IdentifierExprAST>(),
        "emitTableMethodCall: the table method's receiver is not an "
        "identifier — Sema should have rejected this form");
    const auto* id = fa->object->as<IdentifierExprAST>();
    AST_ASSERT_MSG(id->resolvedDecl != nullptr
                && id->resolvedDecl->isa<TableDeclAST>(),
        "emitTableMethodCall: the receiver did not resolve to a "
        "table — Sema should have resolved it");
    const auto* table = id->resolvedDecl->as<TableDeclAST>();

    const auto tableIdx = ctx.compiler().tableIndexOf(table->mangledName);
    AST_ASSERT_MSG(tableIdx.has_value(),
        "emitTableMethodCall: the table has no artifact index — "
        "the compiler's pass A did not register it");
    const uint32_t tableIndex = *tableIdx;

    const std::string method = ctx.compiler().pool().lookup(fa->fieldName);

    // The receiver's column, if this is a `by<Column>` access. Sema
    // sets resolvedColumn when the method name matches a column's
    // primary key (or the generated lookup for a `@primary` column).
    // For every other method, resolvedColumn is null.

    // ─── T.ADD(cells...) ──────────────────────────────────────────────
    if (method == "ADD") {
        for (auto* arg : call->args) {
            emitExpr(arg, ctx);
        }
        ctx.emitOpcode(Opcode::Ext_TableAdd);
        ctx.emitU32(tableIndex);
        const int8_t pops = static_cast<int8_t>(call->args.size());
        const int8_t pushes = 1;   // ADD returns a &T
        ctx.noteStackEffect(pops, pushes);
        return;
    }

    // ─── T.REMOVE(i) ──────────────────────────────────────────────────
    if (method == "REMOVE") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitTableMethodCall: REMOVE takes one argument");
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_TableRemove);
        ctx.emitU32(tableIndex);
        ctx.noteStackEffect(1, 0);
        return;
    }

    // ─── T.CLEAR() ────────────────────────────────────────────────────
    if (method == "CLEAR") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitTableMethodCall: CLEAR takes no arguments");
        ctx.emitOpcode(Opcode::Ext_TableClear);
        ctx.emitU32(tableIndex);
        return;
    }

    // ─── T.SHRINK() ───────────────────────────────────────────────────
    if (method == "SHRINK") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitTableMethodCall: SHRINK takes no arguments");
        ctx.emitOpcode(Opcode::Ext_TableShrink);
        ctx.emitU32(tableIndex);
        return;
    }

    // ─── T.COUNT() ────────────────────────────────────────────────────
    if (method == "COUNT") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitTableMethodCall: COUNT takes no arguments");
        ctx.emitOpcode(Opcode::Ext_TableCount);
        ctx.emitU32(tableIndex);
        return;
    }

    // ─── T.VERSION() ──────────────────────────────────────────────────
    if (method == "VERSION") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitTableMethodCall: VERSION takes no arguments");
        ctx.emitOpcode(Opcode::Ext_TableVersion);
        ctx.emitU32(tableIndex);
        return;
    }

    // ─── T.FIND(predicate) ────────────────────────────────────────────
    if (method == "FIND") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitTableMethodCall: FIND takes one predicate argument");
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_TableFind);
        ctx.emitU32(tableIndex);
        ctx.noteStackEffect(1, 1);
        return;
    }

    // ─── T.AT(i) ──────────────────────────────────────────────────────
    if (method == "AT") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitTableMethodCall: AT takes one argument");
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_TableAt);
        ctx.emitU32(tableIndex);
        ctx.noteStackEffect(1, 1);
        return;
    }

    // ─── T.by<Column>(value) ──────────────────────────────────────────
    //
    // Sema recognizes a `by<PrimaryColumn>` call and records the
    // column on fa->resolvedColumn. The opcode carries both the
    // table index and the column index.
    if (fa->resolvedColumn != nullptr) {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitTableMethodCall: a by<Column> lookup takes one "
            "argument");
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_TableByPrimary);
        ctx.emitU32(tableIndex);
        ctx.emitU16(static_cast<uint16_t>(
            fa->resolvedColumn->columnIndex));
        ctx.noteStackEffect(1, 1);
        return;
    }

    AST_ASSERT_MSG(false,
        "emitTableMethodCall: unhandled table method — the emitter "
        "is out of sync with the language's table methods");
}

// ─── Column-view method dispatch ───────────────────────────────────────────

/// Emit a call to a column-view method: Person.age.TOARRAY().
///
/// A column view is a compile-time identity. It has no runtime
/// representation; the emitter reads the table's artifact index and
/// the column's index directly and emits Ext_ColumnToArray with both
/// operands. The opcode builds a fresh dynamic array of the column's
/// values.
///
/// Only TOARRAY is defined for a column view.
void emitColumnViewMethodCall(CallExprAST* call,
                              FieldAccessExprAST* fa,
                              CompilerContext& ctx) {
    AST_ASSERT_MSG(fa->resolvedColumn != nullptr,
        "emitColumnViewMethodCall: the column view has no "
        "resolvedColumn — Sema should have resolved it");
    AST_ASSERT_MSG(call->args.size() == 0,
        "emitColumnViewMethodCall: TOARRAY takes no arguments");

    const std::string method = ctx.compiler().pool().lookup(fa->fieldName);
    AST_ASSERT_MSG(method == "TOARRAY",
        "emitColumnViewMethodCall: unhandled column-view method — "
        "only TOARRAY is defined");

    // The column view's receiver is the table. It is either a bare
    // identifier (`Person.age`) or a module-qualified access
    // (`entities.Person.age`); columnViewTable resolves either.
    const auto* table = columnViewTable(fa);

    const auto tableIdx =
        ctx.compiler().tableIndexOf(table->mangledName);
    AST_ASSERT_MSG(tableIdx.has_value(),
        "emitColumnViewMethodCall: the column view's table has no "
        "artifact index — the compiler's pass A did not register it");

    // Ext_ColumnToArray <tableIndex> <colIndex> produces a fresh
    // dynamic array of the column's values. No stack inputs; the
    // auto-bookkeeping pushes one BitCopy entry for the array.
    ctx.emitOpcode(Opcode::Ext_ColumnToArray);
    ctx.emitU32(*tableIdx);
    ctx.emitU16(static_cast<uint16_t>(
        fa->resolvedColumn->columnIndex));

    // The resulting array owns its heap buffer. Upgrade the entry.
    ctx.owned().markTopAsOwned();
}

// ─── Array method dispatch ─────────────────────────────────────────────────

/// Emit a call to an array method: arr.ADD(x), arr.REMOVE(i),
/// arr.CLEAR(), arr.LENGTH(), arr.CONTAINS(x), arr.SORT(cmp).
///
/// The receiver is an array value on the stack. The emitter emits the
/// receiver first, then the arguments, then the opcode.
void emitArrayMethodCall(CallExprAST* call,
                         FieldAccessExprAST* fa,
                         CompilerContext& ctx) {
    const std::string method = ctx.compiler().pool().lookup(fa->fieldName);

    // ─── arr.ADD(x) ───────────────────────────────────────────────────
    if (method == "ADD") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitArrayMethodCall: ADD takes one argument");
        emitExpr(fa->object, ctx);      // receiver
        emitExpr(call->args[0], ctx);   // element
        ctx.emitOpcode(Opcode::Ext_ArrayAdd);
        ctx.noteStackEffect(2, 0);
        return;
    }

    // ─── arr.REMOVE(i) ────────────────────────────────────────────────
    if (method == "REMOVE") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitArrayMethodCall: REMOVE takes one argument");
        emitExpr(fa->object, ctx);
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_ArrayRemove);
        ctx.noteStackEffect(2, 0);
        return;
    }

    // ─── arr.CLEAR() ──────────────────────────────────────────────────
    if (method == "CLEAR") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitArrayMethodCall: CLEAR takes no arguments");
        emitExpr(fa->object, ctx);
        ctx.emitOpcode(Opcode::Ext_ArrayClear);
        ctx.noteStackEffect(1, 0);
        return;
    }

    // ─── arr.LENGTH() ─────────────────────────────────────────────────
    if (method == "LENGTH") {
        AST_ASSERT_MSG(call->args.size() == 0,
            "emitArrayMethodCall: LENGTH takes no arguments");

        const TypeDescriptor receiverType =
            translateType(fa->object->resolvedType,
                          ctx.compiler().pool());
        AST_ASSERT_MSG(receiverType.isArray(),
            "emitArrayMethodCall: LENGTH's receiver is not an array — "
            "the caller's dispatch is out of sync");

        if (receiverType.arrayKind == ArrayKind::Fixed) {
            // A fixed array's length is a compile-time constant. Do
            // not evaluate the receiver; just push the count.
            Constant c;
            c.kind = Constant::Kind::Int;
            c.type = translateType(call->resolvedType,
                                   ctx.compiler().pool());
            c.value = static_cast<int64_t>(receiverType.fixedSize);
            const uint32_t idx = ctx.pool().add(std::move(c));
            ctx.emitOpcode(Opcode::LoadConst);
            ctx.emitU32(idx);
            return;
        }

        // A dynamic array's length is a runtime value.
        emitExpr(fa->object, ctx);
        ctx.emitOpcode(Opcode::Ext_ArrayLength);
        // The opcode's table entry: pops=1, pushes=1. Fixed-effect;
        // no noteStackEffect needed.
        return;
    }

    // ─── arr.CONTAINS(x) ──────────────────────────────────────────────
    if (method == "CONTAINS") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitArrayMethodCall: CONTAINS takes one argument");
        emitExpr(fa->object, ctx);
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_ArrayContains);
        ctx.noteStackEffect(2, 1);
        return;
    }

    // ─── arr.SORT(cmp) ────────────────────────────────────────────────
    //
    // The grammar requires a comparator argument. There is no
    // zero-argument SORT; the core library provides convenience
    // comparators for common element types.
    if (method == "SORT") {
        AST_ASSERT_MSG(call->args.size() == 1,
            "emitArrayMethodCall: SORT takes one comparator argument "
            "— the grammar has no zero-argument SORT");
        emitExpr(fa->object, ctx);
        emitExpr(call->args[0], ctx);
        ctx.emitOpcode(Opcode::Ext_ArraySort);
        ctx.emitU8(1);   // flag = 1: comparator form
        ctx.noteStackEffect(2, 0);
        return;
    }

    AST_ASSERT_MSG(false,
        "emitArrayMethodCall: unhandled array method — the emitter "
        "is out of sync with the language's array methods");
}

} // namespace

// ─── Column-view receiver resolution ───────────────────────────────────────

const TableDeclAST* columnViewTable(const FieldAccessExprAST* colView) {
    AST_ASSERT_MSG(colView != nullptr,
        "columnViewTable: null column view");
    AST_ASSERT_MSG(colView->isColumnView,
        "columnViewTable: the expression is not a column view — "
        "the caller's dispatch is out of sync");

    const ExprAST* obj = colView->object;
    AST_ASSERT_MSG(obj != nullptr,
        "columnViewTable: the column view has no object — the "
        "parser should have produced one");

    // Bare form: `Person.age`.
    if (obj->isa<IdentifierExprAST>()) {
        const auto* id = obj->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr
                    && id->resolvedDecl->isa<TableDeclAST>(),
            "columnViewTable: a bare column view's receiver did "
            "not resolve to a table — Sema should have resolved it");
        return id->resolvedDecl->as<TableDeclAST>();
    }

    // Qualified form: `mod.Person.age`.
    if (obj->isa<FieldAccessExprAST>()) {
        const auto* fa = obj->as<FieldAccessExprAST>();
        AST_ASSERT_MSG(fa->isModuleAccess,
            "columnViewTable: a qualified column view's receiver "
            "is a field access but isModuleAccess is not set — "
            "Sema should have classified it as a module access");
        AST_ASSERT_MSG(fa->resolvedDecl != nullptr
                    && fa->resolvedDecl->isa<TableDeclAST>(),
            "columnViewTable: a qualified column view's receiver "
            "did not resolve to a table — Sema should have resolved "
            "it");
        return fa->resolvedDecl->as<TableDeclAST>();
    }

    AST_ASSERT_MSG(false,
        "columnViewTable: a column view's receiver is neither an "
        "identifier nor a module access — the emitter is out of "
        "sync with the parser's receiver forms");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitExpr — the dispatcher
// ─────────────────────────────────────────────────────────────────────────────

void emitExpr(ExprAST* expr, CompilerContext& ctx) {
    AST_ASSERT_MSG(expr != nullptr,
        "emitExpr: null expression — the caller should not have "
        "dispatched a null expr");

    // Every expression has a resolved type. The assert is the
    // compiler's trust in Sema; a null resolvedType here is a
    // Sema bug.
    AST_ASSERT_MSG(expr->resolvedType != nullptr,
        "emitExpr: an expression has no resolved type — "
        "Sema should have resolved every expression's type");

    // Record the current line. The emitter walks the tree in source
    // order; the line table ends up sorted by code offset without
    // further work.
    ctx.noteLine(expr->loc, ctx.module()->filePath);

    switch (expr->kind) {
        case ASTKind::LiteralExpr:      emitLiteralExpr(expr->as<LiteralExprAST>(), ctx); return;
        case ASTKind::IdentifierExpr:   emitIdentifierExpr(expr->as<IdentifierExprAST>(), ctx); return;
        case ASTKind::ArrayLiteralExpr: emitArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx); return;
        case ASTKind::FieldAccessExpr:  emitFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx); return;
        case ASTKind::IndexExpr:        emitIndexExpr(expr->as<IndexExprAST>(), ctx); return;
        case ASTKind::CallExpr:         emitCallExpr(expr->as<CallExprAST>(), ctx); return;
        case ASTKind::LambdaExpr:       emitLambdaExpr(expr->as<LambdaExprAST>(), ctx); return;
        case ASTKind::StartExpr:        emitStartExpr(expr->as<StartExprAST>(), ctx); return;
        case ASTKind::UnaryExpr:        emitUnaryExpr(expr->as<UnaryExprAST>(), ctx); return;
        case ASTKind::BinaryExpr:       emitBinaryExpr(expr->as<BinaryExprAST>(), ctx); return;
        case ASTKind::ParenExpr:        emitParenExpr(expr->as<ParenExprAST>(), ctx); return;
        case ASTKind::RangeExpr:        emitRangeExpr(expr->as<RangeExprAST>(), ctx); return;
        default:
            AST_ASSERT_MSG(false,
                "emitExpr: unhandled ExprAST subclass — the emitter is "
                "out of sync with the AST");
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Literal
// ─────────────────────────────────────────────────────────────────────────────

void emitLiteralExpr(LiteralExprAST* e, CompilerContext& ctx) {
    // Every literal is a compile-time constant. Sema folded it; the
    // constant value is on the node.
    AST_ASSERT_MSG(e->isConst,
        "emitLiteralExpr: a literal expression is not constant — "
        "Sema should have folded every literal");
    AST_ASSERT_MSG(e->constValue.isEvaluated(),
        "emitLiteralExpr: a literal's constValue is not evaluated — "
        "Sema should have folded it");

    const TypeDescriptor type =
        translateType(e->resolvedType, ctx.compiler().pool());
    const uint32_t index = internConstant(ctx, e->constValue, type);

    ctx.emitOpcode(Opcode::LoadConst);   // auto-pushes one BitCopy entry
    ctx.emitU32(index);

    // A string literal is a fresh heap buffer that the value owns.
    // Every other literal (int, float, bool, char, nil) owns nothing.
    if (ownsResource(type)) {
        ctx.owned().markTopAsOwned();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Identifier
// ─────────────────────────────────────────────────────────────────────────────

void emitIdentifierExpr(IdentifierExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->resolvedDecl != nullptr,
        "emitIdentifierExpr: an identifier has no resolvedDecl — "
        "Sema should have resolved every identifier");

    DeclAST* decl = e->resolvedDecl;

    // ─── A local, a parameter, or a top-level binding ──────────────────
    //
    // A ParamAST or a VarDeclAST is a local binding if the slot
    // allocator has a slot for its name; otherwise it is a top-level
    // binding (a VarDeclAST whose value lives in the artifact's
    // static data).
    if (decl->isa<ParamAST>() || decl->isa<VarDeclAST>()) {
        auto slot = ctx.slots().slotFor(decl->name);
        if (slot.has_value()) {
            ctx.emitOpcode(Opcode::LoadLocal);   // auto-push BitCopy
            ctx.emitU16(*slot);
            // The loaded value owns a resource if the slot's type
            // owns one. A string slot produces an Owned value; an
            // int slot produces a BitCopy.
            if (ownsResource(ctx.slots().typeOf(*slot))) {
                ctx.owned().markTopAsOwned();
            }
            return;
        }
        if (decl->isa<VarDeclAST>()) {
            // Top-level binding.
            const auto offset =
                ctx.compiler().staticDataOffsetOf(decl->mangledName);
            AST_ASSERT_MSG(offset.has_value(),
                "emitIdentifierExpr: a top-level binding has no "
                "static-data offset — the compiler's pass A did not "
                "register it");
            ctx.emitOpcode(Opcode::LoadStaticData);   // auto-push BitCopy
            ctx.emitU32(*offset);
            // The binding's declared type decides the loaded value's
            // ownership. A string binding produces an Owned value; an
            // int binding produces a BitCopy.
            AST_ASSERT_MSG(decl->as<VarDeclAST>()->type != nullptr,
                "emitIdentifierExpr: a top-level binding has no "
                "resolved type — Sema should have resolved it");
            const TypeDescriptor bindingType =
                translateType(decl->as<VarDeclAST>()->type, ctx.compiler().pool());
            if (ownsResource(bindingType)) {
                ctx.owned().markTopAsOwned();
            }
            return;
        }
        AST_ASSERT_MSG(false,
            "emitIdentifierExpr: a parameter has no slot — the "
            "function's prologue should have allocated one");
        return;
    }

    // ─── A function ────────────────────────────────────────────────────
    if (decl->isa<FnDeclAST>()) {
        // A bare function name in expression position is a
        // function value: a compile-time code address. A function
        // value owns no resource; the auto-pushed BitCopy entry is
        // correct and no mark is needed.
        const auto* fn = decl->as<FnDeclAST>();
        auto idx = ctx.compiler().functionIndexOf(fn);
        AST_ASSERT_MSG(idx.has_value(),
            "emitIdentifierExpr: an identifier resolved to a function "
            "that was not registered — the driver's two passes are "
            "out of sync");
        ctx.emitOpcode(Opcode::LoadFunction);   // auto-push BitCopy
        ctx.emitU32(*idx);
        return;
    }

    // ─── A table name ──────────────────────────────────────────────────
    if (decl->isa<TableDeclAST>()) {
        // A bare table name in expression position is a reference to
        // the sheet. The sheet itself is a compile-time identity; a
        // table reference on the stack carries no runtime data. Sema
        // produces this only in contexts that then apply a table
        // operation (`T.ADD`, `T.FIND`) or a for-loop iterable.
        //
        // A bare table value is not consumed by any opcode we have;
        // the table operations take the table as a compile-time
        // operand (their opcode carries a table index), not as a
        // runtime value. So a bare table name in expression position
        // is emitted as a Nop — the value stack gains nothing, and
        // the following instruction (the table operation) reads the
        // table from its opcode operand, not from the stack.
        //
        // This is only correct because the compiler never emits a
        // bare table name in a position where a value is expected
        // (assignment, return, argument). Sema rejects those; the
        // grammar has no way to write them.
        AST_ASSERT_MSG(false,
            "emitIdentifierExpr: a bare table name reached the "
            "emitter in expression position — Sema should have "
            "rejected every context where a bare table value is "
            "used as a value");
        return;
    }

    // ─── Unreachable ───────────────────────────────────────────────────
    // Every resolved declaration form is handled above: a ParamAST or
    // VarDeclAST with a slot (local), a VarDeclAST without a slot
    // (top-level binding), a FnDeclAST, or a TableDeclAST. This point
    // is reached only if Sema produced a declaration kind the emitter
    // does not know about.
    AST_ASSERT_MSG(false,
        "emitIdentifierExpr: an identifier resolved to a declaration "
        "kind the emitter does not handle — the emitter is out of "
        "sync with the declaration set");
}

// ─────────────────────────────────────────────────────────────────────────────
// Array literal
// ─────────────────────────────────────────────────────────────────────────────

void emitArrayLiteralExpr(ArrayLiteralExprAST* e, CompilerContext& ctx) {
    // The array literal's elements are on the stack when the array is
    // constructed. The array type (dynamic vs. fixed) is on the
    // resolved type.
    const TypeDescriptor type =
        translateType(e->resolvedType, ctx.compiler().pool());
    AST_ASSERT_MSG(type.isArray(),
        "emitArrayLiteralExpr: an array literal's resolved type is not "
        "an array — Sema should have resolved it");

    // Emit each element, in source order. The stack has them
    // left-to-right; the constructor pops them in reverse.
    for (auto* elem : e->elements) {
        emitExpr(elem, ctx);
    }

    const uint32_t count = static_cast<uint32_t>(e->elements.size());
    if (type.arrayKind == ArrayKind::Dynamic) {
        ctx.emitOpcode(Opcode::Ext_NewArray);
    } else {
        ctx.emitOpcode(Opcode::Ext_NewFixedArray);
    }
    ctx.emitU32(count);

    AST_ASSERT_MSG(count <= 127,
        "emitArrayLiteralExpr: an array literal with more than 127 "
        "elements — the noteStackEffect parameter is int8_t. Widen "
        "noteStackEffect's signature or split the construction.");

    // The opcode's stack effect depends on the element count: it pops
    // `count` element values and pushes one array. OpcodeInfo carries
    // -1 for both, so the emitter supplies the actual effect.
    ctx.noteStackEffect(static_cast<int8_t>(count), 1);

    // A dynamic array owns its heap buffer regardless of element
    // type. A fixed array owns resources only if its element type
    // owns resources — a fixed array of ints is BitCopy, but a fixed
    // array of strings owns its elements.
    //
    // The plan captures both cases: planForType([int]) returns
    // DeepCopyArray / FreeArray (the buffer is heap-allocated);
    // planForType([4, int]) returns BitCopy / None;
    // planForType([4, string]) returns ElementWise / ElementWise.
    // ownsResource(t) reads the drop kind, so the same predicate
    // covers all cases.
    if (ownsResource(type)) {
        ctx.owned().markTopAsOwned();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Field access
// ─────────────────────────────────────────────────────────────────────────────
//
// A FieldAccessExprAST covers four distinct operations, classified by
// Sema:
//
//   - module member access (isModuleAccess)
//   - table method call (isTableMethod)  — handled in emitCallExpr
//   - cell access (resolvedColumn set)
//   - compile-time row reference (isCompileTimeRowRef)  — a &T constant
//
// The dispatcher below handles each.

void emitFieldAccessExpr(FieldAccessExprAST* e, CompilerContext& ctx) {
    if (e->isColumnView) {
        AST_ASSERT_MSG(false,
            "emitFieldAccessExpr: a column view reached the emitter in "
            "expression position — a column view is only valid as a "
            "for-loop iterable or as a TOARRAY receiver. Sema should "
            "have rejected every other use.");
    }

    // ─── Module member access ──────────────────────────────────────────
    if (e->isModuleAccess) {
        AST_ASSERT_MSG(e->resolvedDecl != nullptr,
            "emitFieldAccessExpr: a module member access has no "
            "resolvedDecl — Sema should have resolved it");
        // A module member is either a function, a table, or a
        // top-level binding. Functions and tables are handled the
        // same way as their identifier counterparts; the access
        // resolves to the same decl.
        DeclAST* decl = e->resolvedDecl;
        if (decl->isa<FnDeclAST>()) {
            const auto* fn = decl->as<FnDeclAST>();
            auto idx = ctx.compiler().functionIndexOf(fn);
            AST_ASSERT_MSG(idx.has_value(),
                "emitFieldAccessExpr: a module function was not "
                "registered — the driver's two passes are out of sync");
            ctx.emitOpcode(Opcode::LoadFunction);
            ctx.emitU32(*idx);
            return;
        }
        AST_ASSERT_MSG(false,
            "emitFieldAccessExpr: a module member access resolved to "
            "something other than a function — the emitter does not "
            "yet handle other module members");
        return;
    }

    // ─── Compile-time row reference (Direction.North) ─────────────────────────────
    //
    // A fixed-table member reference is a compile-time &T constant: the
    // row's index in its table. Sema resolved the sugar's target table
    // (resolvedDecl) and the row's index (compileTimeRowIndex) during
    // resolution. The compiler translates those into a RowRef constant
    // and emits LoadConst with the constant's pool index.
    //
    // The compiler's table-index map (populated in pass A) gives the
    // table's artifact index. The pool dedups the constant, so two
    // references to the same member produce the same pool entry.
    if (e->isCompileTimeRowRef) {
        AST_ASSERT_MSG(e->resolvedDecl != nullptr,
            "emitFieldAccessExpr: a compile-time row reference has no "
            "resolvedDecl — Sema should have resolved it");
        AST_ASSERT_MSG(e->resolvedDecl->isa<TableDeclAST>(),
            "emitFieldAccessExpr: a compile-time row reference resolved to a "
            "non-table declaration — Sema should have rejected this");
        const auto* table = e->resolvedDecl->as<TableDeclAST>();

        const auto tableIdx =
            ctx.compiler().tableIndexOf(table->mangledName);
        AST_ASSERT_MSG(tableIdx.has_value(),
            "emitFieldAccessExpr: the compile-time row reference's table has no "
            "artifact index — the compiler's pass A did not register "
            "it");
        AST_ASSERT_MSG(e->hasCompileTimeRow,
            "emitFieldAccessExpr: a compile-time row reference has no resolved "
            "row index — Sema's resolveTableMemberAccess should have "
            "set it");

        Constant c;
        c.kind = Constant::Kind::RowRef;
        // The type is a &T. Sema resolved the expression's type; the
        // emitter translates it.
        c.type = translateType(e->resolvedType, ctx.compiler().pool());
        c.value = RowRefConstant{
            *tableIdx,
            e->compileTimeRowIndex
        };

        const uint32_t index = ctx.pool().add(std::move(c));
        ctx.emitOpcode(Opcode::LoadConst);
        ctx.emitU32(index);
        return;
    }

    // ─── Cell access (row.field) ───────────────────────────────────────
    AST_ASSERT_MSG(e->resolvedColumn != nullptr,
        "emitFieldAccessExpr: a cell access has no resolvedColumn — "
        "Sema should have resolved it");

    const ColumnDeclAST* col = e->resolvedColumn;

    // The object (the row reference) is on the stack. The cell
    // access pops it and pushes the cell's value.
    emitExpr(e->object, ctx);
    ctx.emitOpcode(Opcode::LoadField);   // auto-pop row ref, auto-push cell
    ctx.emitU16(static_cast<uint16_t>(col->columnIndex));

    // A cell whose column type owns a resource (a string column, a
    // host-handle column) produces an Owned value; every other
    // column produces a BitCopy.
    AST_ASSERT_MSG(col->type != nullptr,
        "emitFieldAccessExpr: a column has no resolved type — "
        "Sema should have resolved it");
    const TypeDescriptor cellType =
        translateType(col->type, ctx.compiler().pool());
    if (ownsResource(cellType)) {
        ctx.owned().markTopAsOwned();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Index
// ─────────────────────────────────────────────────────────────────────────────
//
// `container[index]`. The container is a table (Person[i]) or an array
// (arr[i]). The two cases are classified by the target's type.

void emitIndexExpr(IndexExprAST* e, CompilerContext& ctx) {
    const TypeDescriptor targetType =
        translateType(e->target->resolvedType, ctx.compiler().pool());

    if (targetType.isArray()) {
        // Array element access: emit target, emit index, then
        // LoadIndex. The order is target then index, matching how
        // the interpreter pops.
        emitExpr(e->target, ctx);
        emitExpr(e->index, ctx);
        ctx.emitOpcode(Opcode::LoadIndex);   // auto-pop array+index, auto-push element

        // The element owns a resource if the array's element type
        // owns one. A [string] produces an Owned element; an [int]
        // produces a BitCopy.
        AST_ASSERT_MSG(targetType.component != nullptr,
            "emitIndexExpr: an array type has no element type — "
            "Sema should have resolved it");
        if (ownsResource(*targetType.component)) {
            ctx.owned().markTopAsOwned();
        }
        return;
    }

    if (targetType.isNamed()) {
        // Table row access. The table is a compile-time identity; the
        // target expression is an identifier that Sema resolved to a
        // TableDeclAST. Emit the index, then LoadRow with the table's
        // artifact index.
        emitExpr(e->index, ctx);

        AST_ASSERT_MSG(e->target->isa<IdentifierExprAST>(),
            "emitIndexExpr: a table row access has a non-identifier "
            "target — Sema should have rejected this form");
        auto* id = e->target->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr
                    && id->resolvedDecl->isa<TableDeclAST>(),
            "emitIndexExpr: a table row access target did not resolve "
            "to a table — Sema should have resolved it");
        const auto* table = id->resolvedDecl->as<TableDeclAST>();

        const auto tableIdx =
            ctx.compiler().tableIndexOf(table->mangledName);
        AST_ASSERT_MSG(tableIdx.has_value(),
            "emitIndexExpr: a table has no artifact index — the "
            "compiler's pass A did not register it");
        ctx.emitOpcode(Opcode::LoadRow);
        ctx.emitU32(*tableIdx);
        return;
    }

    AST_ASSERT_MSG(false,
        "emitIndexExpr: the target of an index expression is neither "
        "an array nor a table — Sema should have rejected this");
}

// ─────────────────────────────────────────────────────────────────────────────
// Call
// ─────────────────────────────────────────────────────────────────────────────
//
// A call's callee is one of:
//   - an identifier resolving to a top-level FN  (Call)
//   - a field access resolving to a module FN    (Call)
//   - a field access with isTableMethod          (CallTableMeth)
//   - a field access with isColumnView           (ColumnToArray)
//   - an array method (isTableMethod on an array receiver, or a
//     dispatch on the receiver's type)           (CallArrayMeth)
//   - a function-typed value on the stack        (indirect call;
//     not in the current opcode set)
//
// Phase 3 handles the first three. Array methods and indirect calls
// are Phase 4 additions (they need the table-index and array-method
// maps).

void emitCallExpr(CallExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->callee != nullptr,
        "emitCallExpr: a call has no callee — the parser should have "
        "produced one");

    // ─── Direct call to a top-level FN ─────────────────────────────────
    //
    // The callee is either a Lucid-bodied function (registered in
    // m_functionIndex) or a host-bound one (registered in
    // m_hostSymbolIndex). The two cases emit different opcodes:
    // Ext_Call for a Lucid-bodied function, Ext_CallHost for a
    // host-bound one.
    if (e->callee->isa<IdentifierExprAST>()) {
        auto* id = e->callee->as<IdentifierExprAST>();
        AST_ASSERT_MSG(id->resolvedDecl != nullptr
                    && id->resolvedDecl->isa<FnDeclAST>(),
            "emitCallExpr: an identifier callee did not resolve to a "
            "function — Sema should have resolved it");
        const auto* fn = id->resolvedDecl->as<FnDeclAST>();

        // Emit arguments left-to-right. The calling convention is
        // that the callee's parameter i receives the i-th argument.
        // The interpreter's frame setup reads them off the stack in
        // the order they were pushed.
        for (auto* arg : e->args) {
            emitExpr(arg, ctx);
        }

        // The opcode's stack effect depends on the argument count and
        // the callee's return type. Sema resolved the CallExpr's type
        // to the callee's return type; a void return pushes nothing.
        const TypeDescriptor retType =
            translateType(e->resolvedType, ctx.compiler().pool());
        const bool returnsValue = !(retType.isPrimitive()
                                    && retType.primitive == PrimitiveKind::Void);
        const int8_t pushes = returnsValue ? 1 : 0;
        const int8_t pops = static_cast<int8_t>(e->args.size());
        AST_ASSERT_MSG(pops >= 0,
            "emitCallExpr: negative pops — an int8_t overflow in the "
            "argument count");

        if (fn->isHostBound) {
            auto idx = ctx.compiler().hostSymbolIndexOf(fn);
            AST_ASSERT_MSG(idx.has_value(),
                "emitCallExpr: a host-bound function was not "
                "registered — the driver's two passes are out of sync");
            ctx.emitOpcode(Opcode::Ext_CallHost);
            ctx.emitU32(*idx);
        } else {
            auto idx = ctx.compiler().functionIndexOf(fn);
            AST_ASSERT_MSG(idx.has_value(),
                "emitCallExpr: the called function was not "
                "registered — the driver's two passes are out of sync");
            ctx.emitOpcode(Opcode::Ext_Call);
            ctx.emitU32(*idx);
        }

        // noteStackEffect pops the argument entries and pushes one
        // entry for the return value (or zero for a void return).
        ctx.noteStackEffect(pops, pushes);

        // The return value owns a resource if its type owns one.
        // A `-> string` call produces an Owned value; a `-> int`
        // call produces a BitCopy; a `-> void` call produces no
        // value, and the mark would fire on the wrong entry.
        if (returnsValue && ownsResource(retType)) {
            ctx.owned().markTopAsOwned();
        }
        return;
    }

    // ─── Method-style call (obj.method(args)) ──────────────────────────
    if (e->callee->isa<FieldAccessExprAST>()) {
        auto* fa = e->callee->as<FieldAccessExprAST>();

        // A module function: mod.fn(args). The target is either a
        // Lucid-bodied function in the imported module, or a
        // host-bound function that module declares. Same dispatch as
        // the direct-identifier case above.
        if (fa->isModuleAccess) {
            AST_ASSERT_MSG(fa->resolvedDecl != nullptr
                        && fa->resolvedDecl->isa<FnDeclAST>(),
                "emitCallExpr: a module-member callee did not resolve "
                "to a function — Sema should have resolved it");
            const auto* fn = fa->resolvedDecl->as<FnDeclAST>();

            for (auto* arg : e->args) {
                emitExpr(arg, ctx);
            }

            if (fn->isHostBound) {
                auto idx = ctx.compiler().hostSymbolIndexOf(fn);
                AST_ASSERT_MSG(idx.has_value(),
                    "emitCallExpr: a host-bound module function was "
                    "not registered — the driver's two passes are "
                    "out of sync");
                ctx.emitOpcode(Opcode::Ext_CallHost);
                ctx.emitU32(*idx);
            } else {
                auto idx = ctx.compiler().functionIndexOf(fn);
                AST_ASSERT_MSG(idx.has_value(),
                    "emitCallExpr: the called module function was not "
                    "registered — the driver's two passes are out of "
                    "sync");
                ctx.emitOpcode(Opcode::Ext_Call);
                ctx.emitU32(*idx);
            }

            // The stack effect depends on the argument count and the
            // return type.
            const TypeDescriptor retType =
                translateType(e->resolvedType, ctx.compiler().pool());
            const bool returnsValue = !(retType.isPrimitive()
                                        && retType.primitive == PrimitiveKind::Void);
            const int8_t pushes = returnsValue ? 1 : 0;
            const int8_t pops = static_cast<int8_t>(e->args.size());
            AST_ASSERT_MSG(pops >= 0,
                "emitCallExpr: negative pops — an int8_t overflow in "
                "the argument count");
            ctx.noteStackEffect(pops, pushes);

            if (returnsValue && ownsResource(retType)) {
                ctx.owned().markTopAsOwned();
            }
            return;
        }

        // ─── Table method: T.ADD(...), T.FIND(...), T.COUNT(), ... ────
        if (fa->isTableMethod) {
            emitTableMethodCall(e, fa, ctx);
            return;
        }

        // ─── Column-view method: Person.age.TOARRAY() ─────────────────
        if (fa->isColumnView) {
            emitColumnViewMethodCall(e, fa, ctx);
            return;
        }

        // ─── Array method: arr.ADD(x), arr.LENGTH(), ... ──────────────
        //
        // An array method is not classified by Sema. The callee is a
        // field access whose object's type is an array. The emitter
        // detects it here and dispatches to the array-method lowering.
        {
            const TypeDescriptor objectType =
                translateType(fa->object->resolvedType,
                              ctx.compiler().pool());
            if (objectType.isArray()) {
                emitArrayMethodCall(e, fa, ctx);
                return;
            }
        }

        // The callee is not a table method, not a column view, and
        // its object is not an array. The only remaining possibility
        // is an indirect call through a function-typed value (a
        // function-typed cell, §4.1.1c and §5.0). The opcode set has
        // no indirect call; this is a genuine opcode-set gap.
        AST_ASSERT_MSG(false,
            "emitCallExpr: a field-access callee is neither a module "
            "access, a table method, a column view, nor an array "
            "method — the only remaining form is an indirect call "
            "through a function-typed value, and the opcode set has "
            "no indirect-call opcode");
        return;
    }

    AST_ASSERT_MSG(false,
        "emitCallExpr: the callee is neither an identifier nor a "
        "field access — the emitter is out of sync with the parser's "
        "callee forms");
}

// ─────────────────────────────────────────────────────────────────────────────
// Lambda
// ─────────────────────────────────────────────────────────────────────────────
//
// Sema collects every LambdaExprAST into its module's `lambdas` span.
// The compiler's LambdaLift synthesizes a top-level FnDeclAST for each
// one and registers it. By the time emitLambdaExpr runs, the lambda
// has a FunctionProto index reachable through the lift's mapping.
//
// A lambda value is a compile-time code address: LoadFunction with the
// synthesized function's index. A function value owns no resource, so
// the auto-pushed BitCopy ownership entry is correct and no mark is
// needed.

void emitLambdaExpr(LambdaExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e != nullptr,
        "emitLambdaExpr: null lambda expression");

    auto idx = ctx.compiler().lambdaFunctionIndexFor(e);
    AST_ASSERT_MSG(idx.has_value(),
        "emitLambdaExpr: the lambda was not lifted — the compiler's "
        "LambdaLift did not see it, or Sema did not collect it into "
        "the module's lambdas span");

    ctx.emitOpcode(Opcode::LoadFunction);   // auto-push BitCopy
    ctx.emitU32(*idx);
    // A function value is a compile-time code address. It owns no
    // resource; BitCopy is correct.
}

// ─────────────────────────────────────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────────────────────────────────────

void emitStartExpr(StartExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->call != nullptr,
        "emitStartExpr: a start expression has no call — the parser "
        "should have produced one");
    AST_ASSERT_MSG(e->call->callee != nullptr
                && e->call->callee->isa<IdentifierExprAST>(),
        "emitStartExpr: the callee of a start expression is not an "
        "identifier — Sema should have resolved it");

    auto* id = e->call->callee->as<IdentifierExprAST>();
    AST_ASSERT_MSG(id->resolvedDecl != nullptr
                && id->resolvedDecl->isa<FnDeclAST>(),
        "emitStartExpr: the callee of a start expression did not "
        "resolve to a function — Sema should have resolved it");
    const auto* fn = id->resolvedDecl->as<FnDeclAST>();
    AST_ASSERT_MSG(fn->isSequence,
        "emitStartExpr: the callee of a start expression is not a "
        "@sequence function — Sema should have rejected this");

    // A start expression takes no arguments. (§9.2's start_expr
    // production is `'start' call_expr`, and a @sequence function's
    // parameters are passed at start time; but the current grammar
    // has no syntax for start arguments, so the function is invoked
    // with no arguments and every parameter must have a default —
    // which the grammar does not support either. In practice a
    // @sequence function takes no parameters.)
    for (auto* arg : e->call->args) {
        emitExpr(arg, ctx);
    }

    auto idx = ctx.compiler().functionIndexOf(fn);
    AST_ASSERT_MSG(idx.has_value(),
        "emitStartExpr: the started function was not registered — "
        "the driver's two passes are out of sync");
    ctx.emitOpcode(Opcode::Ext_StartSequence);   // auto-push BitCopy
    ctx.emitU32(*idx);

    // A start expression produces a &Coroutine handle, which is a
    // host-backed type: it owns a resource and the caller must drop
    // it. Ext_StartSequence's table entry records pushes=1, so the
    // auto-bookkeeping pushed one BitCopy entry; upgrade it to Owned.
    ctx.owned().markTopAsOwned();
}

// ─────────────────────────────────────────────────────────────────────────────
// Unary
// ─────────────────────────────────────────────────────────────────────────────

void emitUnaryExpr(UnaryExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->operand != nullptr,
        "emitUnaryExpr: a unary expression has no operand — "
        "the parser should have produced one");

    emitExpr(e->operand, ctx);

    // The operand's type decides the opcode. Sema has already
    // resolved the operand type.
    const TypeDescriptor opType =
        translateType(e->operand->resolvedType, ctx.compiler().pool());
    auto pk = primitiveOf(opType);
    AST_ASSERT_MSG(pk.has_value(),
        "emitUnaryExpr: the operand's type is not a primitive — "
        "Sema should have rejected this unary expression");

    const Opcode op = unaryOpcode(e->op, *pk);
    ctx.emitOpcode(op);
}

// ─────────────────────────────────────────────────────────────────────────────
// Binary
// ─────────────────────────────────────────────────────────────────────────────

void emitBinaryExpr(BinaryExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->left != nullptr && e->right != nullptr,
        "emitBinaryExpr: a binary expression is missing an operand — "
        "the parser should have produced both");

    // ─── Short-circuit operators ───────────────────────────────────────
    //
    // `and` and `or` do not evaluate the RHS unconditionally. They
    // lower to a branch sequence:
    //
    //   and:   <LHS>                      ; push LHS
    //          JumpIfFalse  end           ; if false, result is false
    //          <RHS>                      ; else push RHS
    //   end:
    //
    //   or:    <LHS>
    //          JumpIfTrue   end
    //          <RHS>
    //   end:
    //
    // The JumpIfFalse/JumpIfTrue opcodes pop their operand and branch
    // on its value. The stack shape after the sequence is: one value,
    // which is the result of the whole expression.
    if (e->op == BinaryOp::And || e->op == BinaryOp::Or) {
        // Both operands are bool (grammar §6.14).
        const TypeDescriptor lhsType =
            translateType(e->left->resolvedType, ctx.compiler().pool());
        auto pk = primitiveOf(lhsType);
        AST_ASSERT_MSG(pk.has_value() && *pk == PrimitiveKind::Bool,
            "emitBinaryExpr: an `and`/`or` operand is not a bool — "
            "grammar §6.14 requires a bool");

        emitExpr(e->left, ctx);

        // Emit the branch with a placeholder offset. The offset is
        // the relative distance from the instruction after the
        // operand to the target. We patch it after emitting the
        // RHS.
        ctx.emitOpcode(e->op == BinaryOp::And
                           ? Opcode::Ext_JumpIfFalse
                           : Opcode::Ext_JumpIfTrue);
        const uint32_t branchOperandOffset = ctx.here();
        ctx.emitI32(0);   // placeholder

        emitExpr(e->right, ctx);

        // Patch the branch. The target is the instruction after the
        // RHS, which is the end of the expression. The relative
        // offset is computed from the position *after* the operand,
        // which is `branchOperandOffset + 4`.
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base   = static_cast<int32_t>(branchOperandOffset + 4);
        ctx.patchU32(branchOperandOffset,
                     static_cast<uint32_t>(target - base));
        return;
    }

    // ─── Null coalescing ───────────────────────────────────────────────
    //
    // `a ?? b`: if `a` is not nil, the result is `a`; otherwise it is
    // `b`. The lowering keeps a copy of the LHS on the stack while it
    // checks nil, so neither path needs to re-evaluate the LHS.
    //
    //   <LHS>                    ; stack: [LHS]
    //   Dup                      ; stack: [LHS, LHS]
    //   IsNil                    ; stack: [LHS, isNil]
    //   JumpIfFalse  keep        ; pops isNil; if LHS is not nil, jump
    //   Pop                      ; stack: []       (discard the nil LHS)
    //   <RHS>                    ; stack: [RHS]
    //   keep:                    ; stack: [LHS] or [RHS]
    //
    // The stack after the sequence holds exactly one value — the
    // expression's result — regardless of which path was taken.
    if (e->op == BinaryOp::NullCoalesce) {
        // The LHS is a nilable type; its non-nil form is the RHS's
        // type. Both are already resolved by Sema; the emitter does
        // not need to know which is which — it just emits the two
        // sub-expressions and the branch sequence.
        emitExpr(e->left, ctx);

        ctx.emitOpcode(Opcode::Ext_Dup);
        ctx.emitOpcode(Opcode::Ext_IsNil);

        // The JumpIfFalse pops the isNil bool and branches if false
        // (i.e., if the LHS is *not* nil, keep it).
        ctx.emitOpcode(Opcode::Ext_JumpIfFalse);
        const uint32_t keepBranchOffset = ctx.here();
        ctx.emitI32(0);   // placeholder

        // Discard the nil LHS and evaluate the RHS.
        ctx.emitOpcode(Opcode::Ext_Pop);
        emitExpr(e->right, ctx);

        // Patch the branch to land here (after the RHS).
        const int32_t target = static_cast<int32_t>(ctx.here());
        const int32_t base   = static_cast<int32_t>(keepBranchOffset + 4);
        ctx.patchU32(keepBranchOffset,
                     static_cast<uint32_t>(target - base));
        return;
    }

    // ─── Identity comparisons on &T and function values ────────────────
    //
    // These have their own opcodes because they compare identities,
    // not values.
    if (e->op == BinaryOp::Eq || e->op == BinaryOp::Ne) {
        const TypeDescriptor lhsType =
            translateType(e->left->resolvedType, ctx.compiler().pool());
        const TypeDescriptor rhsType =
            translateType(e->right->resolvedType, ctx.compiler().pool());

        emitExpr(e->left, ctx);
        emitExpr(e->right, ctx);

        if (lhsType.isRowRef() || rhsType.isRowRef()) {
            // A &T comparison. The opcode is identity.
            ctx.emitOpcode(e->op == BinaryOp::Eq
                               ? Opcode::Eq_RowRef
                               : Opcode::Ne_RowRef);
            return;
        }
        if (lhsType.isFunction() || rhsType.isFunction()) {
            ctx.emitOpcode(e->op == BinaryOp::Eq
                               ? Opcode::Eq_Function
                               : Opcode::Ne_Function);
            return;
        }

        // A value comparison. Fall through to the typed dispatch
        // below. The operand type is the LHS's type; Sema checked
        // that both sides match.
        auto pk = primitiveOf(lhsType);
        AST_ASSERT_MSG(pk.has_value(),
            "emitBinaryExpr: an Eq/Ne comparison has a non-primitive "
            "operand type that is not a row reference or function — "
            "Sema should have rejected this");
        ctx.emitOpcode(comparisonOpcode(e->op, *pk));
        return;
    }

    // ─── Ordinary typed binary operations ──────────────────────────────
    //
    // Arithmetic, ordering, bitwise. Both operands have the same
    // type; Sema checked. The opcode is chosen by the operand type.
    emitExpr(e->left, ctx);
    emitExpr(e->right, ctx);

    const TypeDescriptor lhsType =
        translateType(e->left->resolvedType, ctx.compiler().pool());
    auto pk = primitiveOf(lhsType);
    AST_ASSERT_MSG(pk.has_value(),
        "emitBinaryExpr: the operand's type is not a primitive — "
        "Sema should have rejected this binary expression");

    switch (e->op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
        case BinaryOp::Pow: {
            const Opcode op = arithmeticOpcode(e->op, *pk);
            ctx.emitOpcode(op);   // auto-pop 2, auto-push 1 (BitCopy)
            // Concat_Str produces a fresh heap-allocated string,
            // which owns a resource. Every other arithmetic opcode
            // produces a primitive (BitCopy).
            if (op == Opcode::Concat_Str) {
                ctx.owned().markTopAsOwned();
            }
            return;
        }

        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
            ctx.emitOpcode(comparisonOpcode(e->op, *pk));
            return;

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            ctx.emitOpcode(bitwiseOpcode(e->op, *pk));
            return;

        case BinaryOp::And:
        case BinaryOp::Or:
        case BinaryOp::NullCoalesce:
            // Handled above; unreachable.
            break;

        case BinaryOp::Eq:
        case BinaryOp::Ne:
            // Handled above; unreachable.
            break;
    }

    AST_ASSERT_MSG(false,
        "emitBinaryExpr: unhandled BinaryOp — the emitter is out of "
        "sync with the operator set");
}

// ─────────────────────────────────────────────────────────────────────────────
// Paren
// ─────────────────────────────────────────────────────────────────────────────

void emitParenExpr(ParenExprAST* e, CompilerContext& ctx) {
    AST_ASSERT_MSG(e->inner != nullptr,
        "emitParenExpr: a paren expression has no inner expression — "
        "the parser should have produced one");
    // Parens are transparent to codegen; the tree shape already
    // encodes precedence.
    emitExpr(e->inner, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Range
// ─────────────────────────────────────────────────────────────────────────────
//
// A range is never a standalone value. Sema rejects it anywhere a real
// type is expected; it appears only as a for-loop iterable or a
// switch-case value. If emitExpr reaches a RangeExprAST, the caller
// (EmitStmt) missed a case.

void emitRangeExpr(RangeExprAST* e, CompilerContext& ctx) {
    (void)e;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "emitRangeExpr: a range expression reached the emitter — "
        "Sema rejects ranges everywhere except for-loop iterables and "
        "switch-case values; a range here is a Sema bug");
}

} // namespace lucid::bytecode::compile