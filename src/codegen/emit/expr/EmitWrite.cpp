/// @file codegen/emit/expr/EmitWrite.cpp
/// @brief Read-then-write expression emitters — assignment, null
///        coalescing, and pipelines.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The three expression kinds that thread a value through multiple
/// evaluation points:
///
///   - `emitAssign`        — `x = y`, `x += 1`, `p.x = 5`, `arr[i] = v`.
///   - `emitNullCoalesce`  — `x ?? fallback`, `risky() ?? default`.
///   - `emitPipeline`      — `seed |> step |> step`.
///
/// ─── What This File Does NOT Own ──────────────────────────────────────────
///   - The place construction (`emitPlace`, `emitIdentifierPlace`,
///     `emitFieldPlace`, `emitIndexPlace`): `EmitPlace.cpp`.
///   - The single write path (`store`): `EmitPlace.cpp`.
///   - The binary operator emission used by compound assignment:
///     `expr/EmitScalar.cpp`'s `emitBinary` (which this file's
///     `emitAssign` doesn't call directly — it does the compound
///     operation inline, on the already-loaded old and RHS values).
///
/// ─── Ownership Tag Conventions ────────────────────────────────────────────
///   - `emitAssign`        → `Owned`. The assignment expression's value
///                            is the new value of the place, and the
///                            caller takes over its claim.
///   - `emitNullCoalesce`  → `Owned` if both arms return `Owned`,
///                            `Borrowed` if both return `Borrowed`. An
///                            assertion fires if the arms disagree.
///   - `emitPipeline`      → `Owned`. Each step is a call; the pipeline's
///                            value is the last step's result.
///
/// ─── The Self-Assign Guard ────────────────────────────────────────────────
/// The self-assign case `f = f` on a `Refcounted` binding must not drop
/// and re-retain the same environment. `store` unconditionally drops the
/// old value when the binding is alive; so `emitAssign` must not call
/// `store` on the self-assign path. The emitter emits a branch:
///
///   - Self-assign path: skip the store entirely (the place already holds
///     the right value).
///   - Normal path: call `store`.
///
/// The guard is the comparison `old.env == new.env` — a cheap pointer
/// comparison. It's only meaningful for `Refcounted` bindings; for other
/// binding kinds, the guard is skipped and the store runs unconditionally.

#include "../Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitAssign — the assignment expression
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Evaluation Order ─────────────────────────────────────────────────
// For a compound assignment (`x += 1`):
//
//   1. Load the old value of `x` (BEFORE evaluating the RHS).
//   2. Evaluate the RHS.
//   3. Compute `old op rhs`.
//   4. Store the result.
//
// For a plain assignment (`x = y`):
//
//   1. Evaluate the RHS.
//   2. Store it.
//
// The old-value load is required for compound assignment and for the
// self-assign guard. It runs before the RHS because the RHS might
// reference `x` (e.g. `x = x + 1`), and loading the old value first is
// what makes the reference see the pre-assignment value.
//
// ─── Ownership at the Store Site ──────────────────────────────────────────
// The value stored into the place is the RHS's value. `store` handles the
// ownership transitions:
//   - `intoOwned(rhs)` acquires a fresh claim for a `Borrowed` RHS (a
//     load) or transfers an `Owned` one.
//   - The old value in the place is dropped (if the binding is alive).
//   - The new value is stored.
//   - The binding is marked alive.
//
// `emitAssign` doesn't call `intoOwned` or `drop` directly. It builds a
// `Place`, evaluates the RHS, and calls `store`.
//
// ─── Return Value ─────────────────────────────────────────────────────────
// The assignment expression's value is the RHS (the new value of the
// place). It's `Owned` — the caller takes over the claim. Whether the
// store path was taken (the common case) or the self-assign path was
// (the guard fired), the returned value is the RHS.

Val Emitter::emitAssign(AssignExprAST* expr) {
    assert(expr && "emitAssign() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Step 1: Get the LHS's Place ──────────────────────────────────────
    Place place = emitPlace(expr->lhs);
    if (!place.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Sem_InvalidAssignment, expr->lhs->loc,
            "assignment target is not an l-value");
        return {};
    }

    // ─── Step 2: Identify the binding (for alive tracking) ────────────────
    // The `decl` is used by `store` to check whether the place's old value
    // should be dropped and whether the binding is already alive. For a
    // field assignment (`p.x = 5`), the `decl` is the *field*, not the
    // struct — that's a design decision. The alternative would be to
    // track the whole struct as the binding and have `store` recurse
    // into fields. The current design is per-binding tracking.
    //
    // For a plain identifier assignment (`x = 5`), the `decl` is `x`.
    // For a field assignment, the `decl` is the field. For an index
    // assignment, the `decl` is null (the element's claim is the array's
    // claim, tracked by the array's binding, not by a per-element
    // declaration).
    ValueDeclAST* decl = nullptr;
    if (expr->lhs->isa<IdentifierExprAST>()) {
        decl = expr->lhs->as<IdentifierExprAST>()->resolvedDecl;
    } else if (expr->lhs->isa<FieldAccessExprAST>()) {
        decl = expr->lhs->as<FieldAccessExprAST>()->resolvedDecl;
    }

    // ─── Step 3: Determine if this is a plain or compound assignment ──────
    const bool isCompound = (expr->op != AssignOp::Assign);

    // ─── Step 4: Load the old value (needed for compound and the guard) ───
    Val oldValue;
    const bool isAlive = (decl != nullptr) && func().isAlive(decl);

    if (isCompound || isAlive) {
        // Load the old value. It's `Borrowed` (the place still holds the
        // claim).
        oldValue = loadPlace(place, b);
        if (!oldValue.isValid()) return {};
    }

    // ─── Step 5: Compute the new value ────────────────────────────────────
    Val rhs;
    if (isCompound) {
        // ─── Compound: emit the RHS, then compute `old op rhs` ────────────
        rhs = emit(expr->rhs);
        if (!rhs.isValid()) return {};

        rhs = coerceTo(rhs, expr->lhs->resolvedType);
        if (!rhs.isValid()) return {};

        // Apply the compound operator. This mirrors `emitBinary`'s
        // dispatch but operates on `oldValue` and `rhs` (already-emitted
        // values), not on AST expressions.
        rhs = applyCompoundOp(expr->op, oldValue, rhs, expr->loc);
        if (!rhs.isValid()) return {};
    } else {
        // ─── Plain: emit the RHS ──────────────────────────────────────────
        rhs = emit(expr->rhs);
        if (!rhs.isValid()) return {};

        rhs = coerceTo(rhs, expr->lhs->resolvedType);
        if (!rhs.isValid()) return {};
    }

    // ─── Step 6: Self-assign guard (only for Refcounted) ──────────────────
    // The guard is only meaningful for `Refcounted` bindings, because
    // those are the only ones where drop-then-retain on the same value
    // is a correctness bug. `OwnedBuffer`'s drop-then-deep-copy is
    // wasteful but correct; scalars have no drop at all.
    if (decl && isAlive
        && classifyResource(decl) == ResourceKind::Refcounted
        && oldValue.isValid() && rhs.isValid()) {

        llvm::Type* oldTy = oldValue.v->getType();
        llvm::Type* newTy = rhs.v->getType();

        // Both should be `{ ptr, ptr }` fat pointers.
        if (oldTy->isStructTy() && oldTy->getStructNumElements() == 2
            && newTy->isStructTy() && newTy->getStructNumElements() == 2) {

            llvm::Value* oldEnv = b.CreateExtractValue(
                oldValue.v, 1, "self_assign.old_env");
            llvm::Value* newEnv = b.CreateExtractValue(
                rhs.v, 1, "self_assign.new_env");
            llvm::Value* isSelfAssign = b.CreateICmpEQ(
                oldEnv, newEnv, "self_assign.cmp");

            // ─── Branch on the guard ──────────────────────────────────────
            llvm::Function* fn = b.GetInsertBlock()->getParent();
            llvm::BasicBlock* normalBlock = llvm::BasicBlock::Create(
                program.llvmContext(), "assign.normal", fn);
            llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
                program.llvmContext(), "assign.merge", fn);

            b.CreateCondBr(isSelfAssign, mergeBlock, normalBlock);

            // ─── Normal path: call store ──────────────────────────────────
            b.SetInsertPoint(normalBlock);
            store(place, rhs, decl);
            b.CreateBr(mergeBlock);

            // ─── Merge ────────────────────────────────────────────────────
            // The assignment's value is `rhs`, regardless of which path
            // ran. No phi needed.
            b.SetInsertPoint(mergeBlock);

            return Val{rhs.v, expr->resolvedType, Own::Owned};
        }
    }

    // ─── Step 7: Normal path — call store ─────────────────────────────────
    // The place holds the old value (if alive), and the RHS carries the
    // new value's claim. `store` handles the transition.
    store(place, rhs, decl);

    // ─── Step 8: Return the new value ─────────────────────────────────────
    return Val{rhs.v, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// applyCompoundOp — apply a compound-assignment operator to old and rhs
// ─────────────────────────────────────────────────────────────────────────────
//
// The compound assignment `x += y` is semantically `x = x + y`. Rather
// than desugaring to a nested `BinaryExprAST` and calling `emitBinary`,
// the emitter applies the operator directly to the already-emitted values.
// This avoids a second AST walk and keeps the ownership flow linear.
//
// The operator set mirrors `emitBinary`'s. Not all operators are
// supported for all types; the emitter diagnoses the mismatches.

Val Emitter::applyCompoundOp(AssignOp op,
                             Val oldValue,
                             Val rhs,
                             SourceLocation loc) {
    if (!oldValue.isValid() || !rhs.isValid()) return {};

    llvm::IRBuilder<>& b = program.builder();
    llvm::Type* ty = oldValue.v->getType();
    const bool isInt = ty->isIntegerTy();

    llvm::Value* result = nullptr;

    switch (op) {
        case AssignOp::AddAssign:
            if (isInt) {
                result = b.CreateAdd(oldValue.v, rhs.v, "add_assign");
            } else if (ty->isFloatingPointTy()) {
                result = b.CreateFAdd(oldValue.v, rhs.v, "fadd_assign");
            } else {
                // String concatenation: `s += t`. Lower to str_concat
                // with the old value as the first operand.
                llvm::StructType* strTy = program.types().stringType();
                if (strTy && ty == strTy) {
                    llvm::AllocaInst* outSlot = createEntryAlloca(
                        strTy, "concat_assign.out");
                    llvm::AllocaInst* lhsSlot = createEntryAlloca(
                        strTy, "concat_assign.lhs");
                    llvm::AllocaInst* rhsSlot = createEntryAlloca(
                        strTy, "concat_assign.rhs");
                    if (!outSlot || !lhsSlot || !rhsSlot) return {};
                    b.CreateStore(oldValue.v, lhsSlot);
                    b.CreateStore(rhs.v, rhsSlot);
                    program.abi().StrConcat(b, outSlot, lhsSlot, rhsSlot);
                    result = b.CreateLoad(strTy, outSlot, "concat_assign.result");
                } else {
                    program.diagnostics.errorAt(
                        DiagCode::Sem_InvalidAssignment, loc,
                        "'+=' is not supported for this type");
                    return {};
                }
            }
            break;

        case AssignOp::SubAssign:
            result = isInt
                ? b.CreateSub(oldValue.v, rhs.v, "sub_assign")
                : b.CreateFSub(oldValue.v, rhs.v, "fsub_assign");
            break;

        case AssignOp::MulAssign:
            result = isInt
                ? b.CreateMul(oldValue.v, rhs.v, "mul_assign")
                : b.CreateFMul(oldValue.v, rhs.v, "fmul_assign");
            break;

        case AssignOp::DivAssign:
            if (isInt) {
                // Zero check, then sdiv.
                llvm::Value* isZero = b.CreateICmpEQ(
                    rhs.v, llvm::ConstantInt::get(rhs.v->getType(), 0),
                    "div_assign.zero_check");
                llvm::Function* fn = b.GetInsertBlock()->getParent();
                llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "div_assign.panic", fn);
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "div_assign.continue", fn);
                b.CreateCondBr(isZero, panicBlock, continueBlock);
                b.SetInsertPoint(panicBlock);
                emitFailure(FailureKind::DivisionByZero, loc);
                b.SetInsertPoint(continueBlock);
                result = b.CreateSDiv(oldValue.v, rhs.v, "sdiv_assign");
            } else {
                result = b.CreateFDiv(oldValue.v, rhs.v, "fdiv_assign");
            }
            break;

        case AssignOp::ModAssign:
            if (isInt) {
                llvm::Value* isZero = b.CreateICmpEQ(
                    rhs.v, llvm::ConstantInt::get(rhs.v->getType(), 0),
                    "mod_assign.zero_check");
                llvm::Function* fn = b.GetInsertBlock()->getParent();
                llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "mod_assign.panic", fn);
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "mod_assign.continue", fn);
                b.CreateCondBr(isZero, panicBlock, continueBlock);
                b.SetInsertPoint(panicBlock);
                emitFailure(FailureKind::ModuloByZero, loc);
                b.SetInsertPoint(continueBlock);
                result = b.CreateSRem(oldValue.v, rhs.v, "srem_assign");
            } else {
                result = b.CreateFRem(oldValue.v, rhs.v, "frem_assign");
            }
            break;

        case AssignOp::PowAssign: {
            // Integer exponentiation is not yet implemented (see emitBinary).
            if (isInt) {
                program.diagnostics.errorAt(
                    DiagCode::Sem_InvalidAssignment, loc,
                    "integer '**=' is not yet implemented");
                return {};
            }
            llvm::Function* powFn = llvm::Intrinsic::getDeclaration(
                &program.module(), llvm::Intrinsic::pow, {ty});
            result = b.CreateCall(powFn, {oldValue.v, rhs.v}, "pow_assign");
            break;
        }

        case AssignOp::BitAndAssign:
            result = b.CreateAnd(oldValue.v, rhs.v, "band_assign");
            break;

        case AssignOp::BitOrAssign:
            result = b.CreateOr(oldValue.v, rhs.v, "bor_assign");
            break;

        case AssignOp::BitXorAssign:
            result = b.CreateXor(oldValue.v, rhs.v, "bxor_assign");
            break;

        case AssignOp::ShlAssign:
            result = b.CreateShl(oldValue.v, rhs.v, "shl_assign");
            break;

        case AssignOp::ShrAssign:
            result = b.CreateAShr(oldValue.v, rhs.v, "ashr_assign");
            break;

        default:
            program.diagnostics.errorAt(
                DiagCode::Sem_InvalidAssignment, loc,
                "unsupported compound assignment operator");
            return {};
    }

    if (!result) return {};

    return Val{result, oldValue.ty, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitNullCoalesce — the null-coalescing operator
// ─────────────────────────────────────────────────────────────────────────────
//
/// ─── The `??` Context ─────────────────────────────────────────────────────
/// `emitNullCoalesce` handles three cases, dispatched by the LHS shape:
///
///   1. Tagged slot (`T?`, `T!`, `T?!`): the failure is in the value's
///      tag. Extracted and branched on directly; no cross-emitter state.
///
///   2. Risky operation (integer `/`, `%`, `arr[i]`, `arr[lo..hi]`,
///      `#toRef`, `#simd_extract`, `#simd_insert`, `arena::alloc`):
///      the failure is a panic the LHS emitter inserts a check for.
///      The `??` pushes its fallback block onto
///      `FunctionState::nullCoalesceFallbacks`, emits the LHS, pops
///      the fallback. Any `emitFailure` call inside the LHS reads the
///      tracker and branches to the fallback instead of panicking.
///
///   3. Plain value: the grammar documents `g ?? 0` as "always g
///      (legal, but dead code)". The LHS is emitted, the fallback is
///      ignored. Sema has warned.
///
/// The tracker is a stack, so `??` nests correctly:
///     `(a[i] ?? 0) + (b[j] ?? 1)`
///     `items[i] ?? (other[j] ?? 0)`
//
// ─── Ownership Tag Agreement ──────────────────────────────────────────────
// The two arms must produce values with the same `Own` tag. If the
// present arm returns `Owned` (the inner value carries a claim from the
// tagged slot, which was `Owned` when the slot was built) and the
// missing arm returns `Borrowed` (a load from a binding), the phi is
// ambiguous. Sema's type checker requires the two arms to have the same
// type, which usually implies the same tag; the emitter asserts.

Val Emitter::emitNullCoalesce(NullCoalesceExprAST* expr) {
    assert(expr && "emitNullCoalesce() with null expression");

    TypeAST* lhsTy = expr->value->resolvedType;
    if (!lhsTy) {
        program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->value->loc,
            "?? left-hand side has no resolved type");
        return {};
    }

    // ─── Case 1: Tagged slot (T?, T!, T?!) ────────────────────────────────
    // The failure is encoded in the value's tag. The `??` reads the tag
    // and picks the narrowed inner value or the fallback. No tracker
    // interaction.
    if (lhsTy->isa<NullableTypeAST>()
        || lhsTy->isa<FallibleTypeAST>()
        || lhsTy->isa<CombinedTypeAST>()) {
        return emitNullCoalesceTagged(expr, lhsTy);
    }

    // ─── Case 2: Risky operation ──────────────────────────────────────────
    // The LHS's emitter will call `emitFailure` on its runtime-check
    // failure path. The `??` pushes a fallback so that failure branches
    // here instead of panicking.
    if (isRiskyLhs(expr->value, program.pool)) {
        return emitNullCoalesceRisky(expr);
    }

    // ─── Case 3: Plain value — dead fallback ──────────────────────────────
    // Grammar: `g ?? 0` is documented as "always g (legal, but dead
    // code)". The fallback is not evaluated. Sema has warned; codegen
    // emits the LHS and ignores the fallback.
    return emit(expr->value);
}

/// `x ?? fallback` where `x`'s type is `T?`, `T!`, or `T?!`.
///
/// The LHS value is a `{ i8 tag, T inner }` struct. The lowering:
///   1. Emit the LHS. Its emitter returns the tagged struct; it does
///      not branch on the tag.
///   2. Extract the tag, compute "present" (per-kind: `T?` is
///      tag != 0, `T!` is tag != 2, `T?!` is tag == 1).
///   3. Branch to present/missing blocks.
///   4. Present: extract the inner value, branch to merge.
///   5. Missing: emit the fallback, coerce to the inner type, branch
///      to merge.
///   6. Merge: PHI the two values.
///
/// No tracker interaction — the failure is in the value, not in an
/// emitter-inserted branch.
Val Emitter::emitNullCoalesceTagged(NullCoalesceExprAST* expr,
                                    TypeAST* lhsTy) {
    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Emit the LHS ─────────────────────────────────────────────────────
    Val lhs = emit(expr->value);
    if (!lhs.isValid()) return {};

    if (!lhs.v->getType()->isStructTy()
        || lhs.v->getType()->getStructNumElements() != 2) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->value->loc,
            "?? left-hand side is not a tagged slot");
        return {};
    }

    // ─── Determine the "present" predicate ────────────────────────────────
    llvm::Value* tag = b.CreateExtractValue(lhs.v, 0, "coalesce.tag");
    llvm::Value* isPresent = nullptr;
    if (lhsTy->isa<CombinedTypeAST>()) {
        isPresent = b.CreateICmpEQ(
            tag, llvm::ConstantInt::get(tag->getType(), 1),
            "coalesce.present");
    } else if (lhsTy->isa<NullableTypeAST>()) {
        isPresent = b.CreateICmpNE(
            tag, llvm::ConstantInt::get(tag->getType(), 0),
            "coalesce.present");
    } else {
        isPresent = b.CreateICmpNE(
            tag, llvm::ConstantInt::get(tag->getType(), 2),
            "coalesce.present");
    }

    // ─── Create the three blocks ──────────────────────────────────────────
    llvm::BasicBlock* presentBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "coalesce.present", fn);
    llvm::BasicBlock* missingBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "coalesce.missing", fn);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "coalesce.merge", fn);

    b.CreateCondBr(isPresent, presentBlock, missingBlock);

    // ─── Present path ─────────────────────────────────────────────────────
    b.SetInsertPoint(presentBlock);
    llvm::Value* inner = b.CreateExtractValue(lhs.v, 1, "coalesce.inner");
    b.CreateBr(mergeBlock);
    llvm::BasicBlock* presentEnd = b.GetInsertBlock();

    // ─── Missing path ─────────────────────────────────────────────────────
    b.SetInsertPoint(missingBlock);

    TypeAST* innerTy =
        lhsTy->isa<NullableTypeAST>()
            ? lhsTy->as<NullableTypeAST>()->inner
        : lhsTy->isa<FallibleTypeAST>()
            ? lhsTy->as<FallibleTypeAST>()->inner
            : lhsTy->as<CombinedTypeAST>()->inner;

    Val fallback = emit(expr->fallback);
    if (!fallback.isValid()) return {};

    if (innerTy) {
        fallback = coerceTo(fallback, innerTy);
        if (!fallback.isValid()) return {};
    }

    b.CreateBr(mergeBlock);
    llvm::BasicBlock* missingEnd = b.GetInsertBlock();

    // ─── Merge ────────────────────────────────────────────────────────────
    b.SetInsertPoint(mergeBlock);

    llvm::Value* fallbackV = fallback.v;
    if (fallbackV->getType() != inner->getType()) {
        llvm::Value* coerced = coerceValueToType(
            fallbackV, inner->getType(), b);
        if (!coerced) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "?? arms have mismatched types");
            return {};
        }
        fallbackV = coerced;
    }

    llvm::PHINode* phi = b.CreatePHI(
        inner->getType(), 2, "coalesce.result");
    phi->addIncoming(inner, presentEnd);
    phi->addIncoming(fallbackV, missingEnd);

    Own resultOwn = (lhs.own == fallback.own) ? lhs.own : Own::Owned;
    TypeAST* resultTy = innerTy ? innerTy : expr->resolvedType;
    return Val{phi, resultTy, resultOwn};
}

/// `x ?? fallback` where `x` is a risky operation (see
/// `isRiskyLhs` in `FailureKind.hpp` for the full list).
///
/// The lowering:
///   1. Create the fallback and merge blocks.
///   2. Push the fallback onto `FunctionState::nullCoalesceFallbacks`.
///   3. Emit the LHS. Its emitter's runtime check calls
///      `emitFailure`, which reads the tracker and branches to the
///      fallback on failure. On success, control reaches whatever
///      block the LHS emitter leaves the builder in.
///   4. Pop the tracker.
///   5. Branch the success path to the merge block.
///   6. Emit the fallback in the fallback block; branch to merge.
///   7. PHI the two values.
///
/// The LHS emitter is not modified — it calls `emitFailure` the same
/// way it would outside a `??`. Only the tracker's state differs, and
/// `emitFailure` reads it.
Val Emitter::emitNullCoalesceRisky(NullCoalesceExprAST* expr) {
    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Create the two join blocks ───────────────────────────────────────
    llvm::BasicBlock* fallbackBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "coalesce.fallback", fn);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "coalesce.merge", fn);

    // ─── Push the tracker, emit the LHS, pop the tracker ──────────────────
    func().pushNullCoalesceFallback(fallbackBlock);
    Val risky = emit(expr->value);
    func().popNullCoalesceFallback();

    if (!risky.isValid()) {
        return {};
    }

    // ─── Success path → merge ─────────────────────────────────────────────
    assert(!b.GetInsertBlock()->getTerminator()
           && "LHS emitter terminated its success block — a value "
              "cannot reach the merge PHI");
    llvm::BasicBlock* successEnd = b.GetInsertBlock();
    b.CreateBr(mergeBlock);

    // ─── Fallback path → emit the RHS → merge ─────────────────────────────
    b.SetInsertPoint(fallbackBlock);
    Val fallback = emit(expr->fallback);
    if (!fallback.isValid()) return {};

    if (expr->resolvedType) {
        fallback = coerceTo(fallback, expr->resolvedType);
        if (!fallback.isValid()) return {};
    }

    b.CreateBr(mergeBlock);
    llvm::BasicBlock* fallbackEnd = b.GetInsertBlock();

    // ─── Merge ────────────────────────────────────────────────────────────
    b.SetInsertPoint(mergeBlock);

    llvm::Value* successV = risky.v;
    llvm::Value* fallbackV = fallback.v;
    if (successV->getType() != fallbackV->getType()) {
        llvm::Value* coerced = coerceValueToType(
            fallbackV, successV->getType(), b);
        if (!coerced) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "?? arms have mismatched types");
            return {};
        }
        fallbackV = coerced;
    }

    llvm::PHINode* phi = b.CreatePHI(
        successV->getType(), 2, "coalesce.risky.result");
    phi->addIncoming(successV, successEnd);
    phi->addIncoming(fallbackV, fallbackEnd);

    Own resultOwn = (risky.own == fallback.own) ? risky.own : Own::Owned;
    return Val{phi, expr->resolvedType, resultOwn};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitPipeline — the pipeline operator
// ─────────────────────────────────────────────────────────────────────────────
//
// `seed |> step1 |> step2 |> ...` threads a value through a sequence of
// calls. Each step is a callable (a function reference or an anonymous
// function) that receives the upstream value as one of its arguments.
//
// ─── Argument Injection ───────────────────────────────────────────────────
// A step's callable is called with:
//
//   - The upstream value, as the first argument.
//   - Any additional arguments from the step's `packArgs`, if the step
//     was written with an explicit argument pack (`f(a, b)!`).
//
// The `!` marker in `f(a, b)!` means "this argument list is intentionally
// incomplete; inject the upstream value." The upstream value goes in
// first. The current AST doesn't have a positional marker (the upstream
// is always first); when positional injection lands, this emitter reads
// the marker from the `PipelineStepAST`.
//
// ─── Ownership Flow ───────────────────────────────────────────────────────
// The upstream value is `intoOwned`-ed before each step's call, so the
// callee's parameter binding takes over the claim. The previous step's
// result (an `Owned` value from a call) transfers without an extra copy.
// A `Borrowed` upstream (a load) is acquired fresh at each step.
//
// ─── Return Value ─────────────────────────────────────────────────────────
// The pipeline's value is the last step's result. If the seed is `Owned`
// and there are no steps, the pipeline's value is the seed itself.

Val Emitter::emitPipeline(PipelineExprAST* expr) {
    assert(expr && "emitPipeline() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Seed ─────────────────────────────────────────────────────────────
    Val current = emit(expr->seed);
    if (!current.isValid()) return {};

    // ─── Steps ────────────────────────────────────────────────────────────
    for (PipelineStepAST* step : expr->steps) {
        if (!step) continue;

        // ─── Emit the callable ────────────────────────────────────────────
        Val calleeVal = emit(step->callable);
        if (!calleeVal.isValid()) return {};

        // ─── Callable's function type ─────────────────────────────────────
        FuncTypeAST* calleeFnTy = step->callable->resolvedType
            ? (step->callable->resolvedType->isa<FuncTypeAST>()
                  ? step->callable->resolvedType->as<FuncTypeAST>()
                  : nullptr)
            : nullptr;
        if (!calleeFnTy) {
            program.diagnostics.errorAt(
                DiagCode::Sem_NotCallable, step->callable->loc,
                "pipeline step callable is not a function type");
            return {};
        }

        llvm::FunctionType* fnTy = program.types().functionType(
            calleeFnTy, /*isClosure=*/false);
        if (!fnTy) return {};

        // ─── Build the argument list ──────────────────────────────────────
        // Upstream first, then the step's pack args.
        std::vector<llvm::Value*> args;

        // Upstream injection.
        {
            // The parameter type for the upstream value is the first
            // parameter's type.
            TypeAST* upstreamParamTy = (!calleeFnTy->params.empty())
                ? calleeFnTy->params[0]->type
                : nullptr;

            Val upstreamVal = current;
            if (upstreamParamTy) {
                upstreamVal = coerceArgument(upstreamVal, upstreamParamTy);
                if (!upstreamVal.isValid()) return {};
            }

            llvm::Value* materialized = materializeArgument(upstreamVal);
            if (!materialized) return {};

            Val matVal{materialized, upstreamVal.ty, upstreamVal.own};
            Val owned = program.ownership().intoOwned(matVal, b);
            if (!owned.isValid()) return {};

            args.push_back(owned.v);
        }

        // Pack args.
        for (size_t i = 0; i < step->packArgs.size(); ++i) {
            ExprAST* argExpr = step->packArgs[i];
            if (!argExpr) continue;

            Val argVal = emit(argExpr);
            if (!argVal.isValid()) return {};

            // Parameter type for this argument is at index i+1 (offset by
            // the injected upstream).
            TypeAST* paramTy = (i + 1 < calleeFnTy->params.size())
                ? calleeFnTy->params[i + 1]->type
                : nullptr;
            if (paramTy) {
                argVal = coerceArgument(argVal, paramTy);
                if (!argVal.isValid()) return {};
            }

            llvm::Value* materialized = materializeArgument(argVal);
            if (!materialized) return {};

            Val matVal{materialized, argVal.ty, argVal.own};
            Val owned = program.ownership().intoOwned(matVal, b);
            if (!owned.isValid()) return {};

            args.push_back(owned.v);
        }

        // ─── Emit the call ────────────────────────────────────────────────
        llvm::Value* result = emitCallableCall(
            calleeVal.v, args, fnTy, calleeFnTy->shape, "pipeline.call");
        if (!result) return {};

        // The result becomes the next step's upstream.
        TypeAST* returnTy = calleeFnTy->returnType;
        if (!returnTy) {
            // A pipeline step that returns void produces no value; the
            // pipeline's result is invalid.
            current = {};
        } else {
            current = Val{result, returnTy, Own::Owned};
        }
    }

    return current;
}

} // namespace codegen