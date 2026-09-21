/// @file codegen/emit/expr/EmitTruthiness.cpp
/// @brief The truthiness rules — coercing a Lucid value to an LLVM `i1`.
///
/// ─── What Truthiness Is ───────────────────────────────────────────────────
/// Lucid's condition positions (`if`, `while`, `do-while`, `and`, `or`,
/// `not`, `??`) accept any type, not just `bool`. The language defines
/// what "truthy" means per type:
///
///   bool        — the value itself.
///   int, uint   — nonzero.
///   float       — nonzero (NaN is falsy, per IEEE-754 comparison).
///   char        — nonzero byte.
///   string      — non-empty (length > 0).
///   T?          — tag != 0 (present).
///   T!          — tag != 2 (not err).
///   T?!         — tag == 1 (present, not nil, not err).
///   [N]T        — length > 0 (always true for N > 0).
///   [_]T, [*]T  — length > 0 (from the slice's len field).
///   fn, cls     — always truthy (a function value is a value).
///   struct      — always truthy (a struct is a value; use a field to
///                 test anything meaningful).
///   Arena       — always truthy.
///   Simd        — always truthy (no meaningful zero test on a vector
///                 in the current language).
///
/// ─── Why a Coercion, Not a Comparison ─────────────────────────────────────
/// The emitter's job is to produce an `i1` — the type LLVM's `br` and
/// `select` expect. The coercion is emitted as an `icmp` / `fcmp` / tag
/// check, and the result is a fresh `i1` value. The consumer (the `if`
/// emitter, the `and` emitter, etc.) branches on it.
///
/// ─── Ownership ────────────────────────────────────────────────────────────
/// Truthiness is a read-only operation. It loads fields from the input
/// value (a string's length, a tagged slot's tag) but never stores or
/// drops. The input `Val` is unchanged; the returned `i1` is `Owned`
/// in the trivial sense that every scalar is.
///
/// ─── The `Val` Interface ──────────────────────────────────────────────────
/// Takes a `Val` rather than `(llvm::Value*, TypeAST*)` because every
/// caller already has a `Val` from `emit(expr)`. Passing the pair
/// separately would just be an un-pack-and-repack at every call site.

#include "../Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/Types.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitTruthiness — coerce a Lucid value to an LLVM `i1`
// ─────────────────────────────────────────────────────────────────────────────
//
// The dispatch is by AST type, not by LLVM type. Two different Lucid types
// can share an LLVM shape: a string and a slice are both
// `{ ptr, i64, i64 }`, but the string's length is field 1 and the slice's
// is field 1 too — same shape, same test. A nullable and a fallible are
// both `{ i8, T }`, but the tag values that mean "present" differ
// (nullable: tag != 0; fallible: tag != 2). So the AST type is the
// authoritative source of the rule.

llvm::Value* Emitter::emitTruthiness(Val val) {
    if (!val.isValid()) return nullptr;

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx);
    llvm::Value* v = val.v;
    TypeAST* ty = val.ty;

    // ─── Tagged types: T?, T!, T?! ────────────────────────────────────────
    // All three lower to `{ i8 tag, T inner }`. The tag is field 0. The
    // value that means "present" differs:
    //
    //   T?  — tag == 0 is nil; tag != 0 is present.
    //   T!  — tag == 2 is err; tag != 2 is present.
    //   T?! — tag == 1 is present; tag != 1 (nil or err) is falsy.
    //
    // The order matters: T?! is checked before T? and T! because it's a
    // distinct node type with a distinct rule. If the AST had a subtyping
    // relation between them, the order would need to reflect it; it
    // doesn't, so any order works, but checking T?! first is clearest.

    if (ty->isa<CombinedTypeAST>()) {
        llvm::Value* tag = b.CreateExtractValue(v, 0, "truth.tag");
        return b.CreateICmpEQ(
            tag,
            llvm::ConstantInt::get(tag->getType(), 1),
            "truth.present");
    }

    if (ty->isa<NullableTypeAST>()) {
        llvm::Value* tag = b.CreateExtractValue(v, 0, "truth.tag");
        return b.CreateICmpNE(
            tag,
            llvm::ConstantInt::get(tag->getType(), 0),
            "truth.not_nil");
    }

    if (ty->isa<FallibleTypeAST>()) {
        llvm::Value* tag = b.CreateExtractValue(v, 0, "truth.tag");
        return b.CreateICmpNE(
            tag,
            llvm::ConstantInt::get(tag->getType(), 2),
            "truth.not_err");
    }

    // ─── Primitives ───────────────────────────────────────────────────────
    if (ty->isa<PrimitiveTypeAST>()) {
        PrimitiveKind kind = ty->as<PrimitiveTypeAST>()->primitiveKind;

        // Bool: already `i1`.
        if (kind == PrimitiveKind::Bool) {
            return v;
        }

        // String: non-empty. The value is `lucid.String` with len at
        // field 1. If the value is instead a bare `ptr` (the transitional
        // dynamic-array lowering), test for non-null.
        if (kind == PrimitiveKind::String) {
            if (v->getType()->isStructTy()) {
                llvm::Value* len = b.CreateExtractValue(v, 1, "truth.len");
                return b.CreateICmpNE(
                    len,
                    llvm::ConstantInt::get(len->getType(), 0),
                    "truth.nonempty");
            }
            // A bare pointer (a string that hasn't been shaped yet).
            return b.CreateICmpNE(
                v,
                llvm::Constant::getNullValue(v->getType()),
                "truth.nonnull");
        }

        // Char: nonzero byte.
        if (kind == PrimitiveKind::Char) {
            return b.CreateICmpNE(
                v,
                llvm::ConstantInt::get(v->getType(), 0),
                "truth.nonzero");
        }

        // Integer kinds: nonzero.
        if (isIntegerKind(kind)) {
            return b.CreateICmpNE(
                v,
                llvm::Constant::getNullValue(v->getType()),
                "truth.nonzero");
        }

        // Float kinds: ordered-not-equal to zero. The "ordered" comparison
        // makes NaN falsy, which is the conventional choice.
        if (isFloatKind(kind)) {
            return b.CreateFCmpONE(
                v,
                llvm::ConstantFP::get(v->getType(), 0.0),
                "truth.nonzero");
        }
    }

    // ─── Arrays ───────────────────────────────────────────────────────────
    if (ty->isa<ArrayTypeAST>()) {
        ArrayTypeAST* arr = ty->as<ArrayTypeAST>();

        // Fixed array `[N]T`: truthy iff N > 0. That's a compile-time
        // constant — a fixed-size array of length 0 is never constructed
        // (the language requires `N >= 1`), so the answer is always `true`.
        if (arr->isFixed()) {
            return llvm::ConstantInt::get(i1Ty, 1);
        }

        // Slice `[_]T`: the value is `{ ptr, i64 len, i64 cap }`. Truthy
        // iff `len > 0`. Field 1 is the length.
        if (arr->isSlice()) {
            llvm::Value* len = b.CreateExtractValue(v, 1, "truth.len");
            return b.CreateICmpNE(
                len,
                llvm::ConstantInt::get(len->getType(), 0),
                "truth.nonempty");
        }

        // Dynamic array `[*]T`: the current lowering is a bare `ptr`.
        // Truthy iff non-null. Once the dynamic-array lowering becomes
        // `{ ptr, i64, i64 }`, this branch should change to the same
        // length check as the slice case.
        if (arr->isDynamic()) {
            if (v->getType()->isStructTy()) {
                llvm::Value* len = b.CreateExtractValue(v, 1, "truth.len");
                return b.CreateICmpNE(
                    len,
                    llvm::ConstantInt::get(len->getType(), 0),
                    "truth.nonempty");
            }
            return b.CreateICmpNE(
                v,
                llvm::Constant::getNullValue(v->getType()),
                "truth.nonnull");
        }
    }

    // ─── Function types ───────────────────────────────────────────────────
    // A function value is always truthy. There's no meaningful "empty"
    // state for a bare function pointer (it's either a valid symbol or
    // a bug), and a fat pointer's null env is legal (a non-capturing
    // closure), so testing the fat pointer's fields wouldn't help.
    if (ty->isa<FuncTypeAST>()) {
        return llvm::ConstantInt::get(i1Ty, 1);
    }

    // ─── Arena and ArenaDescriptor ────────────────────────────────────────
    // An Arena is always a valid value; "empty" isn't a condition the
    // language tests with truthiness. Same for ArenaDescriptor.
    if (ty->isa<ArenaTypeAST>() || ty->isa<ArenaDescriptorTypeAST>()) {
        return llvm::ConstantInt::get(i1Ty, 1);
    }

    // ─── SIMD ─────────────────────────────────────────────────────────────
    // SIMD vectors don't have a natural truthiness. The language doesn't
    // allow them in condition position (Sema rejects), so reaching here
    // means the emitter was called on a value Sema should have rejected.
    // Return the conservative answer.
    if (ty->isa<SimdTypeAST>()) {
        return llvm::ConstantInt::get(i1Ty, 1);
    }

    // ─── Structs (named types) ────────────────────────────────────────────
    // A struct is always truthy. Structs don't have a "null" state, and
    // the language doesn't define a truthiness for them — a user with a
    // struct condition wants `if p.isValid`, not `if p`. Reaching here
    // means Sema let a struct through; the emitter returns the
    // conservative answer.
    if (ty->isa<NamedTypeAST>()) {
        return llvm::ConstantInt::get(i1Ty, 1);
    }

    // ─── Pointers and references ──────────────────────────────────────────
    // A reference (`&T`) is always non-null (Sema guarantees). A raw
    // pointer (`*T`) has a meaningful null test, but Sema's "sealed
    // conduit" rules forbid using `*T` in a condition — you must go
    // through `#toRef` first. So both are always truthy if they reach
    // here, but the pointer case is the more defensible "test for
    // non-null" answer.
    if (ty->isa<PtrTypeAST>()) {
        return b.CreateICmpNE(
            v,
            llvm::Constant::getNullValue(v->getType()),
            "truth.nonnull");
    }
    if (ty->isa<RefTypeAST>()) {
        return llvm::ConstantInt::get(i1Ty, 1);
    }

    // ─── Fallback ─────────────────────────────────────────────────────────
    // An unhandled type reached `emitTruthiness`. This is either a Sema
    // bug (a type Sema should have rejected in condition position) or an
    // emitter gap (a type whose truthiness rule wasn't written down). The
    // conservative answer is "always truthy" — the branch is taken — which
    // is the safe default for a condition that should have been rejected
    // anyway.
    //
    // In debug builds, assert so the gap surfaces during development.
    assert(false
           && "emitTruthiness: unhandled type — add the truthiness rule "
              "or reject the type in Sema");
    return llvm::ConstantInt::get(i1Ty, 1);
}

} // namespace codegen