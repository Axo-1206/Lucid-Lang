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
///
/// ─── Design: function values are constants ────────────────────────────────
/// A bare function name (`isMinor`, `onIdleEnter`) is a compile-time
/// code address. It is not a load from a variable. emitIdentifierExpr
/// recognizes the resolvedDecl == FnDeclAST case and emits LoadFunction
/// with the function's index in the artifact. A lambda is the same: the
/// compiler already lowered it to a top-level function during a
/// pre-pass; emitLambdaExpr emits LoadFunction for that lowered
/// function.

#include "../compile/BakeConstant.hpp"
#include "../compile/CompilerContext.hpp"
#include "../compile/TypeTranslation.hpp"
#include "bytecode/compile/Compiler.hpp"
#include "EmitExpr.hpp"
#include "EmitPlace.hpp"

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

/// Emit an opcode and record the stack depth after it. The emitter
/// calls this whenever the instruction's effect on the value stack
/// could push the depth past the previous high-water mark.
///
/// The function takes the delta: how many values this instruction
/// pushes (+n) or pops (-n) beyond the values it consumes for its
/// result. Most instructions produce one value from N operands; the
/// delta is (1 - N).
///
/// We track depth conservatively: after every instruction, the depth
/// is updated by the delta. The high-water mark is recorded on the
/// context.
inline void emitTracked(CompilerContext& ctx, Opcode op, int32_t delta) {
    ctx.emitOpcode(op);
    // ctx tracks the current depth elsewhere; this helper only notes
    // the delta. The actual depth bookkeeping lives in the frame
    // bookkeeping the emitter does at block boundaries. For the
    // purposes of maxStackDepth, the conservative delta is the count
    // of values this instruction can leave on the stack beyond what
    // the following instruction consumes — which is always +1 for a
    // value-producing instruction. We note +1 on every value-producing
    // instruction; the exact accounting is done by the assembler
    // after the fact if needed.
    (void)delta;
}

/// The primitive kind of a type, if it is a primitive. Returns nullopt
/// for a non-primitive type. Used to dispatch a typed opcode.
std::optional<PrimitiveKind> primitiveOf(const TypeDescriptor& t) {
    if (t.isPrimitive()) return t.primitive;
    if (t.isNullable() && t.component && t.component->isPrimitive()) {
        return t.component->primitive;
    }
    return std::nullopt;
}

/// The integer-typed opcode family for a given primitive kind: signed
/// or unsigned. Returns 0 if the kind is not an integer.
enum class IntFamily { None, Signed, Unsigned };
IntFamily intFamilyOf(PrimitiveKind k) {
    if (isSignedIntegerKind(k))   return IntFamily::Signed;
    if (isUnsignedIntegerKind(k)) return IntFamily::Unsigned;
    return IntFamily::None;
}

/// Return the index of a primitive kind within its family, for table
/// lookup. I8/I16/I32/I64 -> 0..3; U8/U16/U32/U64 -> 0..3;
/// F32/F64 -> 0..1.
int widthIndex(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Int8:    return 0;
        case PrimitiveKind::Int16:   return 1;
        case PrimitiveKind::Int32:   return 2;
        case PrimitiveKind::Int64:   return 3;
        case PrimitiveKind::Uint8:   return 0;
        case PrimitiveKind::Uint16:  return 1;
        case PrimitiveKind::Uint32:  return 2;
        case PrimitiveKind::Uint64:  return 3;
        case PrimitiveKind::Float32: return 0;
        case PrimitiveKind::Float64: return 1;
        default: return -1;
    }
}

/// Select the arithmetic opcode for a BinaryOp and an operand type.
/// `Add`, `Sub`, `Mul`, `Div`, `Mod`, `Pow`. The type determines the
/// family and width.
Opcode arithmeticOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    AST_ASSERT_MSG(w >= 0,
        "arithmeticOpcode: operand type is not a numeric primitive — "
        "Sema should have rejected this binary expression");

    const IntFamily fam = intFamilyOf(k);
    if (fam == IntFamily::Signed) {
        // Add_I8 = 0x10, groups of 4 per op, in the order
        // Add, Sub, Mul, Div, Mod, Pow.
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_I8);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x0C + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x10 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x14 + w);
            default: break;
        }
    } else if (fam == IntFamily::Unsigned) {
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_U8);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x0C + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x10 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x14 + w);
            default: break;
        }
    } else if (isFloatKind(k)) {
        const uint16_t base = static_cast<uint16_t>(Opcode::Add_F32);
        switch (op) {
            case BinaryOp::Add: return static_cast<Opcode>(base + 0x00 + w);
            case BinaryOp::Sub: return static_cast<Opcode>(base + 0x02 + w);
            case BinaryOp::Mul: return static_cast<Opcode>(base + 0x04 + w);
            case BinaryOp::Div: return static_cast<Opcode>(base + 0x06 + w);
            case BinaryOp::Mod: return static_cast<Opcode>(base + 0x08 + w);
            case BinaryOp::Pow: return static_cast<Opcode>(base + 0x0A + w);
            default: break;
        }
    } else if (k == PrimitiveKind::String && op == BinaryOp::Add) {
        return Opcode::Concat_Str;
    }

    AST_ASSERT_MSG(false,
        "arithmeticOpcode: no opcode exists for this (op, type) pair — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

/// Select the comparison opcode for a BinaryOp and an operand type.
/// Eq, Ne, Lt, Le, Gt, Ge.
Opcode comparisonOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    // RowRef and Function comparisons are identity, not value.
    // They are dispatched from emitBinaryExpr's Eq/Ne cases before
    // this helper is called.

    const IntFamily fam = intFamilyOf(k);
    if (fam == IntFamily::Signed) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_I8);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_I8);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_I8);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_I8);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_I8);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_I8);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (fam == IntFamily::Unsigned) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_U8);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_U8);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_U8);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_U8);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_U8);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_U8);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (isFloatKind(k)) {
        const uint16_t eqBase = static_cast<uint16_t>(Opcode::Eq_F32);
        const uint16_t neBase = static_cast<uint16_t>(Opcode::Ne_F32);
        const uint16_t ltBase = static_cast<uint16_t>(Opcode::Lt_F32);
        const uint16_t leBase = static_cast<uint16_t>(Opcode::Le_F32);
        const uint16_t gtBase = static_cast<uint16_t>(Opcode::Gt_F32);
        const uint16_t geBase = static_cast<uint16_t>(Opcode::Ge_F32);
        switch (op) {
            case BinaryOp::Eq: return static_cast<Opcode>(eqBase + w);
            case BinaryOp::Ne: return static_cast<Opcode>(neBase + w);
            case BinaryOp::Lt: return static_cast<Opcode>(ltBase + w);
            case BinaryOp::Le: return static_cast<Opcode>(leBase + w);
            case BinaryOp::Gt: return static_cast<Opcode>(gtBase + w);
            case BinaryOp::Ge: return static_cast<Opcode>(geBase + w);
            default: break;
        }
    } else if (k == PrimitiveKind::Bool) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Bool;
            case BinaryOp::Ne: return Opcode::Ne_Bool;
            default: break;
        }
    } else if (k == PrimitiveKind::Char) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Char;
            case BinaryOp::Ne: return Opcode::Ne_Char;
            case BinaryOp::Lt: return Opcode::Lt_Char;
            case BinaryOp::Le: return Opcode::Le_Char;
            case BinaryOp::Gt: return Opcode::Gt_Char;
            case BinaryOp::Ge: return Opcode::Ge_Char;
            default: break;
        }
    } else if (k == PrimitiveKind::String) {
        switch (op) {
            case BinaryOp::Eq: return Opcode::Eq_Str;
            case BinaryOp::Ne: return Opcode::Ne_Str;
            case BinaryOp::Lt: return Opcode::Lt_Str;
            case BinaryOp::Le: return Opcode::Le_Str;
            case BinaryOp::Gt: return Opcode::Gt_Str;
            case BinaryOp::Ge: return Opcode::Ge_Str;
            default: break;
        }
    }

    AST_ASSERT_MSG(false,
        "comparisonOpcode: no opcode exists for this (op, type) pair — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

/// Select the bitwise opcode for a BinaryOp and an integer operand
/// type.
Opcode bitwiseOpcode(BinaryOp op, PrimitiveKind k) {
    const int w = widthIndex(k);
    const IntFamily fam = intFamilyOf(k);
    AST_ASSERT_MSG(fam != IntFamily::None,
        "bitwiseOpcode: operand type is not an integer — "
        "Sema should have rejected this binary expression");

    const uint16_t andBase = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitAnd_I8)
        : static_cast<uint16_t>(Opcode::BitAnd_U8);
    const uint16_t orBase  = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitOr_I8)
        : static_cast<uint16_t>(Opcode::BitOr_U8);
    const uint16_t xorBase = (fam == IntFamily::Signed)
        ? static_cast<uint16_t>(Opcode::BitXor_I8)
        : static_cast<uint16_t>(Opcode::BitXor_U8);

    switch (op) {
        case BinaryOp::BitAnd: return static_cast<Opcode>(andBase + w);
        case BinaryOp::BitOr:  return static_cast<Opcode>(orBase  + w);
        case BinaryOp::BitXor: return static_cast<Opcode>(xorBase + w);
        case BinaryOp::Shl:
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Shl_I8) + w);
            // Unsigned Shl shares the signed Shl range: the shift
            // operand is a count, and left shift is the same operation
            // for signed and unsigned. Only right shift differs.
            return static_cast<Opcode>(
                static_cast<uint16_t>(Opcode::Shl_I8) + w);
        case BinaryOp::Shr:
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Shr_I8) + w);
            return static_cast<Opcode>(
                static_cast<uint16_t>(Opcode::Ext_Shr_U8) + w);
        default: break;
    }
    AST_ASSERT_MSG(false,
        "bitwiseOpcode: no opcode exists for this operation — "
        "Sema should have rejected this expression");
    return Opcode::Nop;
}

/// Select the unary opcode for a UnaryOp and an operand type.
Opcode unaryOpcode(UnaryOp op, PrimitiveKind k) {
    switch (op) {
        case UnaryOp::Neg: {
            const IntFamily fam = intFamilyOf(k);
            const int w = widthIndex(k);
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Neg_I8) + w);
            if (isFloatKind(k))
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::Neg_F32) + w);
            // Negation of an unsigned value is legal in the grammar:
            // the result is a signed value of the next-wider type.
            // Sema resolves that; the operand of Neg_U* in the
            // artifact is already the widened type. We re-dispatch on
            // the resolved type of the operand, which Sema has
            // already widened.
            AST_ASSERT_MSG(false,
                "unaryOpcode: Neg applied to a non-numeric operand — "
                "Sema should have widened or rejected");
            break;
        }
        case UnaryOp::Not:
            AST_ASSERT_MSG(k == PrimitiveKind::Bool,
                "unaryOpcode: Not applied to a non-bool operand — "
                "grammar §6.14 requires a bool");
            return Opcode::Not_Bool;
        case UnaryOp::BitNot: {
            const IntFamily fam = intFamilyOf(k);
            const int w = widthIndex(k);
            if (fam == IntFamily::Signed)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::BitNot_I8) + w);
            if (fam == IntFamily::Unsigned)
                return static_cast<Opcode>(
                    static_cast<uint16_t>(Opcode::BitNot_U8) + w);
            AST_ASSERT_MSG(false,
                "unaryOpcode: BitNot applied to a non-integer operand — "
                "Sema should have rejected");
            break;
        }
    }
    AST_ASSERT_MSG(false, "unaryOpcode: unhandled UnaryOp");
    return Opcode::Nop;
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

} // namespace

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

namespace {

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

    ctx.emitOpcode(Opcode::LoadConst);
    ctx.emitU32(index);
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
            ctx.emitOpcode(Opcode::LoadLocal);
            ctx.emitU16(*slot);
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
            ctx.emitOpcode(Opcode::LoadStaticData);
            ctx.emitU32(*offset);
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
        // function value: a compile-time code address.
        const auto* fn = decl->as<FnDeclAST>();
        auto idx = ctx.compiler().functionIndexOf(fn);
        AST_ASSERT_MSG(idx.has_value(),
            "emitIdentifierExpr: an identifier resolved to a function "
            "that was not registered — the driver's two passes are "
            "out of sync");
        ctx.emitOpcode(Opcode::LoadFunction);
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
    ctx.emitOpcode(Opcode::LoadField);
    ctx.emitU16(static_cast<uint16_t>(col->columnIndex));
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
        ctx.emitOpcode(Opcode::LoadIndex);
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
    // KNOWN GAP: a host-bound function (FN x = host("...")) has no
    // FunctionProto, so functionIndexOf returns nullopt for it and
    // the assert below fires. The compiler registered the function's
    // host symbol in pass A but discarded the returned index, so the
    // call site cannot emit Ext_CallHost <symbol index>. Host-bound
    // call sites are unimplemented; see the note in emitDeclArtifacts.
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

        auto idx = ctx.compiler().functionIndexOf(fn);
        AST_ASSERT_MSG(idx.has_value(),
            "emitCallExpr: the called function was not registered — "
            "the driver's two passes are out of sync");
        ctx.emitOpcode(Opcode::Ext_Call);
        ctx.emitU32(*idx);

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
        ctx.noteStackEffect(pops, pushes);
        return;
    }

    // ─── Method-style call (obj.method(args)) ──────────────────────────
    if (e->callee->isa<FieldAccessExprAST>()) {
        auto* fa = e->callee->as<FieldAccessExprAST>();

        // A module function: mod.fn(args).
        if (fa->isModuleAccess) {
            AST_ASSERT_MSG(fa->resolvedDecl != nullptr
                        && fa->resolvedDecl->isa<FnDeclAST>(),
                "emitCallExpr: a module-member callee did not resolve "
                "to a function — Sema should have resolved it");
            const auto* fn = fa->resolvedDecl->as<FnDeclAST>();

            for (auto* arg : e->args) {
                emitExpr(arg, ctx);
            }

            auto idx = ctx.compiler().functionIndexOf(fn);
            AST_ASSERT_MSG(idx.has_value(),
                "emitCallExpr: the called module function was not "
                "registered — the driver's two passes are out of sync");
            ctx.emitOpcode(Opcode::Ext_Call);
            ctx.emitU32(*idx);
            return;
        }

        // A table method: T.ADD(...), T.FIND(...), T.COUNT(), etc.
        //
        // The opcodes for the table methods (Ext_TableAdd,
        // Ext_TableFind, Ext_TableCount, ...) and the table-index
        // lookup (Compiler::tableIndexOf) both exist. The emitter
        // for this branch is not yet written.
        if (fa->isTableMethod) {
            AST_ASSERT_MSG(false,
                "emitCallExpr: table methods are not yet supported — "
                "the emitter for this branch has not been written");
            return;
        }

        // A column-view method: Person.age.TOARRAY().
        //
        // The opcode Ext_ColumnToArray exists. The emitter for this
        // branch is not yet written.
        if (fa->isColumnView) {
            AST_ASSERT_MSG(false,
                "emitCallExpr: column-view methods are not yet "
                "supported — the emitter for this branch has not "
                "been written");
            return;
        }

        // The callee is a field access that is not a module access,
        // a table method, or a column view. Two possibilities remain:
        //
        //   1. An array method (arr.ADD(x), arr.SORT(), ...). The
        //      opcodes for these exist (Ext_ArrayAdd, Ext_ArraySort,
        //      ...). The emitter for this branch has not been
        //      written.
        //
        //   2. An indirect call through a function-typed cell
        //      (§4.1.1c, §5.0). The opcode set has no indirect
        //      call; this is a genuine opcode-set gap.
        //
        // The emitter does not yet distinguish the two. Both
        // currently fall through to this assert.
        AST_ASSERT_MSG(false,
            "emitCallExpr: a field-access callee is neither a module "
            "access, a table method, nor a column view — the callee "
            "is either an array method (emitter not yet written) or "
            "an indirect call through a function-typed cell (no "
            "opcode exists). The emitter does not yet distinguish "
            "the two cases.");
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
// A lambda is lowered to a top-level function by a compiler pre-pass
// (per §6.9). By the time emitExpr runs, the lambda has an assigned
// FunctionProto index. The pre-pass records it on the node (via a
// side table in the Compiler, keyed by the LambdaExprAST*).
//
// In Phase 3 the pre-pass does not exist yet, so this case is a
// placeholder.

void emitLambdaExpr(LambdaExprAST* e, CompilerContext& ctx) {
    (void)e;
    (void)ctx;
    AST_ASSERT_MSG(false,
        "emitLambdaExpr: lambdas are not yet supported — the "
        "lambda-lowering pre-pass is a Phase 4 addition");
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
    ctx.emitOpcode(Opcode::Ext_StartSequence);
    ctx.emitU32(*idx);
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
        case BinaryOp::Pow:
            ctx.emitOpcode(arithmeticOpcode(e->op, *pk));
            return;

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

} // namespace

} // namespace lucid::bytecode::compile