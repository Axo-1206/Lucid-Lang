/// @file codegen/emit/EmitExpr.cpp
/// @brief The entry point of the expression-lowering subsystem: the
///        `Emitter::emit(ExprAST*)` dispatch and the const-fold
///        short-circuit.
///
/// ─── This File Is The Map ─────────────────────────────────────────────────
/// Expression lowering is spread across five files. This one is the index:
///
///   - `EmitExpr.cpp`         (this file) — `emit(ExprAST*)` and
///                            `emitFoldedConstant`.
///   - `expr/EmitScalar.cpp`  — `emitLiteral`, `emitIdentifier`,
///                            `emitBinary`, `emitUnary`, `emitIf`,
///                            `emitRange`.
///   - `expr/EmitAccess.cpp`  — `emitIndex`, `emitSlice`,
///                            `emitFieldAccess`, `emitModuleAccess`,
///                            `emitArenaAccess`.
///   - `expr/EmitAggregate.cpp` — `emitStructLiteral`, `emitArrayLiteral`.
///   - `expr/EmitWrite.cpp`   — `emitAssign`, `emitNullCoalesce`,
///                            `emitPipeline`.
///
/// `emitCall` and `emitIntrinsic` live in `EmitCall.cpp`. `emitAnonFunc`
/// lives in `EmitClosure.cpp`. This file references them by name only.
///
/// ─── The Ownership Tag ────────────────────────────────────────────────────
/// Every expression emitter returns a `Val` with an `Own` tag:
///
///   - `Owned` — a fresh value carrying a claim the receiver takes over.
///               Literals, calls, struct literals, closures, binary ops.
///
///   - `Borrowed` — an alias to a claim held elsewhere. Identifier loads,
///                  field loads, index loads.
///
/// The tag replaces the old `isFreshExpression` heuristic. Where the old
/// code inspected the AST to guess the tag, the new emitters know it
/// because they produced the value.
///
/// ─── The Tag Table ────────────────────────────────────────────────────────
/// Every expression kind and its tag, so a new emitter author can check
/// their work at a glance:
///
///   Expression kind              | Own tag  | Why
///   -----------------------------|----------|-----------------------------
///   Literal (int, float, bool)   | Owned    | No claim to transfer.
///   Literal (char)               | Owned    | Scalar.
///   Literal (string)             | Owned    | Fresh; cap==0 marks static,
///                                |          | intoOwned skips the copy.
///   Literal (nil, err)           | Owned    | Sentinel; zero-valued slot.
///   Identifier (local load)      | Borrowed | The alloca holds the claim.
///   Identifier (param load)      | Borrowed | The param alloca holds it.
///   Identifier (fn-shaped Fn)    | Borrowed | A global symbol.
///   Identifier (cls-shaped Fn)   | Borrowed | Value map holds the fat ptr.
///   Identifier (enum variant)    | Owned    | A constant; no claim.
///   Binary (arith, cmp, bitwise) | Owned    | Fresh scalar.
///   Binary (string concat)       | Owned    | Fresh buffer from __lucid_str_concat.
///   Unary                        | Owned    | Fresh scalar.
///   If (both arms agree)         | either   | Phi merges the arms.
///   If (arms disagree)           | BUG      | Assertion fires.
///   NullCoalesce (both agree)    | either   | Phi merges the arms.
///   NullCoalesce (disagree)      | BUG      | Assertion fires.
///   Call                         | Owned    | Rule 3: callee transfers.
///   Intrinsic                    | Owned    | Same as a call.
///   Array literal (fixed)        | Owned    | Fresh constant / aggregate.
///   Array literal (dynamic)      | Owned    | Fresh heap buffer.
///   Struct literal               | Owned    | Freshly built.
///   Index                        | Borrowed | Container holds the claim.
///   Slice                        | Owned    | Fresh view; owns no buffer.
///   Field access                 | Borrowed | Struct holds the field's claim.
///   Module access (var)          | Borrowed | Module instance holds the claim.
///   Module access (fn)           | Borrowed | A symbol reference.
///   Arena access                 | Owned    | Freshly allocated.
///   Assign                       | Owned    | The new value of the place.
///   Pipeline                     | Owned    | The last step's result.
///   AnonFunc                     | Owned    | Fresh fat pointer.
///   Range                        | invalid  | Never a value.
///
/// ─── The Rules ────────────────────────────────────────────────────────────
/// Two rules govern every emitter in the subsystem:
///
///   Rule 1 — An expression emitter never writes to storage. It produces a
///   value. The only write path is `Emitter::store`, which is called from
///   `emitAssign`, `emitVarDecl`, `store`'s callers, and `store` itself.
///
///   Rule 2 — An expression emitter never calls `Ownership::intoOwned` on
///   its own result. It returns a `Val` with a tag; the consumer (a store,
///   a return, an argument pass) decides whether to acquire a fresh claim.
///   The two exceptions are `emitReturnStmt` (which isn't in this
///   subsystem) and `store` (which isn't either).
///
/// ─── Folded Constants ─────────────────────────────────────────────────────
/// Sema folds some expressions to compile-time `ConstantValue`s. When it
/// does, `emit(ExprAST*)` short-circuits to `emitFoldedConstant` before
/// dispatching on the expression kind. The folded value bypasses the
/// per-kind emitter entirely.
///
/// This is a single point of behavior for the whole subsystem. The
/// alternative — a check inside every emitter — would be the same check
/// repeated thirteen times, with thirteen chances to forget it.
///
/// ─── The File-Level Headers ───────────────────────────────────────────────
/// Each sub-file's header comment explains what that subsystem's emitters
/// share and what's specific to each. Read them in the order listed above
/// for the full picture; this file's header gives the shape of the whole.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/ASTStrings.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emit — the public dispatch
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::emit(ExprAST* expr) {
    if (!expr) return {};

    // ─── Folded constant short-circuit ────────────────────────────────────
    // Sema folds some expressions to `ConstantValue`s. If this one was
    // folded, emit the constant directly. `emitFoldedConstant` returns an
    // invalid `Val` for constants it can't lower (structs, arrays,
    // function pointers), which fall through to the per-kind emitter.
    if (expr->isConst && expr->constValue.isEvaluated()) {
        if (Val folded = emitFoldedConstant(expr)) {
            return folded;
        }
    }

    // ─── Per-kind dispatch ────────────────────────────────────────────────
    // The `subsystem` comment on each row names the file that implements
    // the emitter, so a reader can jump directly to it.
    switch (expr->kind) {
        // ─── EmitScalar.cpp ───────────────────────────────────────────────
        case ASTKind::LiteralExpr:
            return emitLiteral(expr->as<LiteralExprAST>());
        case ASTKind::IdentifierExpr:
            return emitIdentifier(expr->as<IdentifierExprAST>());
        case ASTKind::BinaryExpr:
            return emitBinary(expr->as<BinaryExprAST>());
        case ASTKind::UnaryExpr:
            return emitUnary(expr->as<UnaryExprAST>());
        case ASTKind::IfExpr:
            return emitIf(expr->as<IfExprAST>());
        case ASTKind::RangeExpr:
            return emitRange(expr->as<RangeExprAST>());

        // ─── EmitAccess.cpp ───────────────────────────────────────────────
        case ASTKind::IndexExpr:
            return emitIndex(expr->as<IndexExprAST>());
        case ASTKind::SliceExpr:
            return emitSlice(expr->as<SliceExprAST>());
        case ASTKind::FieldAccessExpr:
            return emitFieldAccess(expr->as<FieldAccessExprAST>());
        case ASTKind::ModuleAccessExpr:
            return emitModuleAccess(expr->as<ModuleAccessExprAST>());
        case ASTKind::ArenaAccessExpr:
            return emitArenaAccess(expr->as<ArenaAccessExprAST>());

        // ─── EmitAggregate.cpp ────────────────────────────────────────────
        case ASTKind::ArrayLiteralExpr:
            return emitArrayLiteral(expr->as<ArrayLiteralExprAST>());
        case ASTKind::StructLiteralExpr:
            return emitStructLiteral(expr->as<StructLiteralExprAST>());

        // ─── EmitWrite.cpp ────────────────────────────────────────────────
        case ASTKind::AssignExpr:
            return emitAssign(expr->as<AssignExprAST>());
        case ASTKind::NullCoalesceExpr:
            return emitNullCoalesce(expr->as<NullCoalesceExprAST>());
        case ASTKind::PipelineExpr:
            return emitPipeline(expr->as<PipelineExprAST>());

        // ─── EmitCall.cpp ─────────────────────────────────────────────────
        case ASTKind::CallExpr:
            return emitCall(expr->as<CallExprAST>());
        case ASTKind::IntrinsicCallExpr:
            return emitIntrinsic(expr->as<IntrinsicCallExprAST>());

        // ─── EmitClosure.cpp ──────────────────────────────────────────────
        case ASTKind::AnonFuncExpr:
            return emitAnonFunc(expr->as<AnonFuncExprAST>());

        default:
            // Sema should have rejected unknown kinds. Reaching here is a
            // compiler bug; emit a diagnostic so the failure is visible
            // rather than silently producing a null value.
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "unsupported expression kind: ",
                astKindToString(expr->kind));
            return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFoldedConstant — the const-eval short-circuit
// ─────────────────────────────────────────────────────────────────────────────
//
// Sema's ConstEvaluator fills `expr->constValue` for expressions it can
// evaluate at compile time. When it does, the emitter's job is to emit the
// constant directly rather than re-lowering the expression tree.
//
// ─── What This Handles ────────────────────────────────────────────────────
// Scalars (int, float, bool, char), strings, and nil/err sentinels. All of
// these lower to constants with no side effects.
//
// If `emitFoldedConstant` returns an invalid `Val`, the caller
// (`emit(ExprAST*)`) falls through to the normal per-kind emitter, which
// handles the kinds this function doesn't:
//   - Structs and arrays: they have aggregate construction paths that
//     the constant path doesn't want to re-derive.
//   - Function pointers: they resolve to an `llvm::Function*` or a
//     closure fat pointer, which the per-kind emitter handles uniformly.
//   - The `Void`, `Error`, and `Unknown` constant kinds: they have no
//     LLVM value.
//
// ─── Why It Returns `Val` and Not `llvm::Value*` ──────────────────────────
// The caller needs the AST type to tag the result. Deriving the type from
// the folded value alone would be lossy: an `i32` folded from a `uint`
// looks the same as one folded from an `int`. The AST carries
// `expr->resolvedType`, which is authoritative.

Val Emitter::emitFoldedConstant(ExprAST* expr) {
    assert(expr && "emitFoldedConstant() with null expression");
    assert(expr->isConst && expr->constValue.isEvaluated()
           && "emitFoldedConstant() called on a non-folded expression");

    const ConstantValue& cv = expr->constValue;
    TypeAST* ty = expr->resolvedType;
    if (!ty) return {};

    llvm::IRBuilder<>& b = program.builder();

    switch (cv.kind) {
        case ConstantValue::Kind::Bool: {
            llvm::Type* llvmTy = program.types().get(ty);
            if (!llvmTy) return {};
            llvm::Value* v = llvm::ConstantInt::get(
                llvmTy, cv.asBool() ? 1 : 0);
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::Int: {
            llvm::Type* llvmTy = program.types().get(ty);
            if (!llvmTy) return {};
            llvm::Value* v = llvm::ConstantInt::get(
                llvmTy, static_cast<uint64_t>(cv.asInt()),
                /*isSigned=*/true);
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::Float: {
            llvm::Type* llvmTy = program.types().get(ty);
            if (!llvmTy) return {};
            llvm::Value* v = llvm::ConstantFP::get(llvmTy, cv.asFloat());
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::String: {
            std::string str = program.pool.lookup(cv.asString());
            llvm::Value* v = program.types().stringLiteral(str, b);
            if (!v) return {};
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::Char: {
            // A char is stored as an `InternedString` in the ConstantValue.
            // Take its first byte.
            std::string str = program.pool.lookup(cv.asString());
            llvm::Type* llvmTy = program.types().get(ty);
            if (!llvmTy) return {};
            uint8_t byte = str.empty() ? 0 : static_cast<uint8_t>(str[0]);
            llvm::Value* v = llvm::ConstantInt::get(llvmTy, byte);
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err: {
            // A sentinel in a tagged-slot context. Emit a zero-initialized
            // slot; the tag is set by the enclosing tagged-slot
            // construction site.
            llvm::Type* llvmTy = program.types().get(ty);
            if (!llvmTy) return {};
            llvm::Value* v = llvm::Constant::getNullValue(llvmTy);
            return Val{v, ty, Own::Owned};
        }

        case ConstantValue::Kind::Void:
            // A void folded value has no LLVM value.
            return {};

        default:
            // Structs, arrays, function pointers, and the Error/Unknown
            // kinds fall through to the per-kind emitter.
            return {};
    }
}

} // namespace codegen