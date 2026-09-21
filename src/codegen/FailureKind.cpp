/// @file codegen/FailureKind.cpp
/// @brief The `FailureKind` metadata table and the `isRiskyLhs` predicate.

#include "FailureKind.hpp"

#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/registry/IntrinsicRegistry.hpp"

#include <unordered_map>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// The metadata table
// ─────────────────────────────────────────────────────────────────────────────
//
// One row per FailureKind. This is the source of truth for:
//   - The runtime error kind (used to build the panic message).
//   - Whether `??` can intercept the failure.
//   - Which AST kinds produce it.

namespace {

const std::unordered_map<FailureKind, FailureInfo>& table() {
    static const std::unordered_map<FailureKind, FailureInfo> t = {
        // ─── Arithmetic ────────────────────────────────────────────────
        { FailureKind::DivisionByZero, {
            RuntimeErrorKind::DivisionByZero,
            /*interceptableByCoalesce=*/true,
            { ASTKind::BinaryExpr, ASTKind::AssignExpr },
            "integer division by zero"
        }},
        { FailureKind::ModuloByZero, {
            RuntimeErrorKind::ModuloByZero,
            /*interceptableByCoalesce=*/true,
            { ASTKind::BinaryExpr, ASTKind::AssignExpr },
            "integer modulo by zero"
        }},

        // ─── Array / slice indexing ────────────────────────────────────
        { FailureKind::FixedArrayIndexOutOfBounds, {
            RuntimeErrorKind::ArrayIndexOutOfBounds,
            /*interceptableByCoalesce=*/true,
            { ASTKind::IndexExpr },
            "fixed-array index out of bounds"
        }},
        { FailureKind::SliceIndexOutOfBounds, {
            RuntimeErrorKind::ArrayIndexOutOfBounds,
            /*interceptableByCoalesce=*/true,
            { ASTKind::IndexExpr },
            "slice index out of bounds"
        }},
        { FailureKind::DynamicArrayIndexOutOfBounds, {
            RuntimeErrorKind::ArrayIndexOutOfBounds,
            /*interceptableByCoalesce=*/true,
            { ASTKind::IndexExpr },
            "dynamic-array index out of bounds"
        }},
        { FailureKind::SliceBoundsOutOfRange, {
            RuntimeErrorKind::SliceBoundsOutOfRange,
            /*interceptableByCoalesce=*/true,
            { ASTKind::SliceExpr },
            "slice bounds out of range"
        }},

        // ─── Pointers ──────────────────────────────────────────────────
        { FailureKind::NullPointerDereference, {
            RuntimeErrorKind::NullPointerDereference,
            /*interceptableByCoalesce=*/true,
            { ASTKind::IntrinsicCallExpr },
            "null pointer dereference"
        }},

        // ─── SIMD ──────────────────────────────────────────────────────
        { FailureKind::SimdLaneOutOfBounds, {
            RuntimeErrorKind::ArrayIndexOutOfBounds,
            /*interceptableByCoalesce=*/true,
            { ASTKind::IntrinsicCallExpr },
            "SIMD lane index out of bounds"
        }},

        // ─── Arena ─────────────────────────────────────────────────────
        { FailureKind::ArenaOutOfCapacity, {
            RuntimeErrorKind::ArenaOutOfCapacity,
            /*interceptableByCoalesce=*/true,
            { ASTKind::ArenaAccessExpr },
            "arena out of capacity"
        }},
    };
    return t;
}

} // anonymous namespace

const FailureInfo& failureInfo(FailureKind kind) {
    const auto& t = table();
    auto it = t.find(kind);
    assert(it != t.end() && "FailureKind has no metadata row");
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// isRiskyLhs
// ─────────────────────────────────────────────────────────────────────────────
//
// The switch below MUST agree with the metadata table above. Every AST
// kind listed in a row's `astKinds` (for an interceptable failure) must
// return `true` from this function, after the per-kind refinement.
//
// When adding a new FailureKind:
//   1. Add the enum value in FailureKind.hpp.
//   2. Add its row to the table above.
//   3. Add the corresponding case below (or extend an existing one).
//   4. Call `emitter.emitFailure(FailureKind::X, loc)` at the check site.
//
// Steps 1–3 all touch this file pair. If you do them together, they
// can't drift.

bool isRiskyLhs(const ExprAST* expr, StringPool& pool) {
    if (!expr) return false;

    switch (expr->kind) {
        // ─── BinaryExprAST: only Div and Mod are risky ────────────────
        //
        // Add is not risky. Sub is not risky. Mul is not risky (integer
        // overflow is not checked by the language today; if it becomes
        // checked, add a case here).
        //
        // Pow is float-only today; it doesn't panic. If integer `**`
        // ever panics, add a case here.
        case ASTKind::BinaryExpr: {
            BinaryOp op = expr->as<BinaryExprAST>()->op;
            return op == BinaryOp::Div || op == BinaryOp::Mod;
        }

        // ─── AssignExprAST: only DivAssign and ModAssign are risky ────
        case ASTKind::AssignExpr: {
            AssignOp op = expr->as<AssignExprAST>()->op;
            return op == AssignOp::DivAssign || op == AssignOp::ModAssign;
        }

        // ─── IndexExprAST and SliceExprAST: always risky ──────────────
        //
        // Every array/slice access carries a runtime bounds check.
        // (Fixed-array access with a provably in-bounds literal index
        // skips the check in the emitter, but the AST is still risky —
        // the emitter's decision is an optimization, not a language
        // rule. `??` on such an expression is still legal, and its
        // fallback is dead code that gets pruned.)
        case ASTKind::IndexExpr:
        case ASTKind::SliceExpr:
            return true;

        // ─── IntrinsicCallExprAST: only #toRef, #simd_extract, ────────
        //                            #simd_insert are risky ────────────
        case ASTKind::IntrinsicCallExpr: {
            const IntrinsicCallExprAST* ic =
                expr->as<IntrinsicCallExprAST>();
            IntrinsicRegistry& reg = IntrinsicRegistry::getInstance(pool);
            const IntrinsicInfo* info = reg.getInfo(ic->intrinsicName);
            if (!info) return false;
            switch (info->kind) {
                case IntrinsicKind::ToRef:
                case IntrinsicKind::SimdExtract:
                case IntrinsicKind::SimdInsert:
                    return true;
                default:
                    return false;
            }
        }

        // ─── ArenaAccessExprAST: only instance-form `::alloc` is risky ─
        //
        // `Arena::create` returns `Arena!` (a tagged slot — handled
        // by the tagged case of `emitNullCoalesce`, not by the tracker).
        // `Arena::empty` cannot fail. Every other method is a pure read.
        case ASTKind::ArenaAccessExpr: {
            const ArenaAccessExprAST* aa = expr->as<ArenaAccessExprAST>();
            if (aa->isStatic) return false;
            return pool.lookupView(aa->methodName) == "alloc";
        }

        default:
            return false;
    }
}

} // namespace codegen