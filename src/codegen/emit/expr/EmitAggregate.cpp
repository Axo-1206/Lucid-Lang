/// @file codegen/emit/expr/EmitAggregate.cpp
/// @brief Aggregate construction — struct literals and array literals.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The two expression kinds that build an aggregate value:
///
///   - `emitStructLiteral`  — `Point { x = 1, y = 2 }`, `Pair<int, string> { ... }`.
///   - `emitArrayLiteral`   — `[1, 2, 3]`, `["hello", "world"]`, `[]`.
///
/// ─── What This File Does NOT Own ──────────────────────────────────────────
///   - The storage of aggregates. `let p = Point { ... }` goes through
///     `emitVarDecl` (which allocates the slot) then `store` (which writes
///     the value into the slot). This file produces the value; the caller
///     writes it.
///   - Field and element writes after construction. `p.x = 5` goes through
///     `emitAssign` (in `expr/EmitWrite.cpp`) → `emitFieldPlace` (in
///     `EmitPlace.cpp`) → `store`.
///   - The place forms of field/element access. Those are in `EmitPlace.cpp`.
///
/// ─── Ownership Tag Convention ─────────────────────────────────────────────
/// Both emitters return `Owned`. The aggregate is freshly constructed; the
/// caller takes over the claim. The claim covers any resource fields or
/// elements the aggregate contains, which were acquired via `intoOwned`
/// at insertion time.
///
/// ─── The `insertvalue` Pattern ────────────────────────────────────────────
/// Both emitters build an aggregate by chaining `insertvalue` on an
/// `undef` value:
///
///     %0 = undef { i32, i32 }
///     %1 = insertvalue %0, i32 1, 0
///     %2 = insertvalue %1, i32 2, 1
///
/// This is LLVM's canonical way to build a struct or array value from
/// elements. The optimiser folds the chain into a single constant when
/// all elements are compile-time-known.
///
/// ─── The Resource Field Rule ──────────────────────────────────────────────
/// A field (or element) whose type owns a resource must carry its own
/// claim after construction. The emitter acquires that claim via
/// `Ownership::intoOwned(val, b)` at insertion time:
///
///   - A `Borrowed` value (a load from a binding) is deep-copied
///     (OwnedBuffer) or retained (Refcounted). The struct owns a fresh
///     claim.
///
///   - An `Owned` value (a fresh literal, a call result) transfers its
///     claim to the struct. No additional copy.
///
/// Skipping the `intoOwned` would leave the struct aliasing a claim the
/// source still holds — a double-free waiting to happen when both are
/// dropped.
///
/// ─── The Array Literal in Two Shapes ──────────────────────────────────────
/// Fixed array `[N]T` is a first-class LLVM aggregate. The emitter builds
/// it with `insertvalue` (for constants) or by spilling to a stack slot
/// (for non-constant elements) and loading the array back.
///
/// Dynamic array `[*]T` is currently a bare `ptr`. There's no defined way
/// to construct one from a literal until the array lowering stabilizes to
/// the `{ ptr, i64, i64 }` shape that slices use. This emitter diagnoses
/// the dynamic-array-literal case.

#include "../Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <cassert>
#include <unordered_map>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitStructLiteral — a struct value built from field initializers
// ─────────────────────────────────────────────────────────────────────────────
//
// Steps:
//
//   1. Resolve the struct declaration and its LLVM type.
//   2. Map each field name to its index in the struct.
//   3. Start with `undef`.
//   4. For each `FieldInitAST`, in source order:
//        a. Find the field's index.
//        b. Emit the init expression.
//        c. Coerce to the field's declared type.
//        d. Acquire a fresh claim (`intoOwned`).
//        e. `insertvalue` into the struct at that index.
//   5. For each field without an init, use its default value if it has
//      one, or a zero-of-the-field-type if it doesn't.
//   6. Return the constructed struct as `Owned`.
//
// ─── The Field Index Cache ────────────────────────────────────────────────
// `FieldInitAST` doesn't carry a field index (it's a name-value pair). The
// emitter builds a name→index map from the struct declaration once per
// literal. For a struct with a handful of fields, the map is small and
// fast.
//
// ─── Default Values ───────────────────────────────────────────────────────
// A field without an init takes its declared default value (from
// `FieldDeclAST::defaultVal`) if it has one. If it doesn't, Sema's rule
// is that the field must be zero-initializable: scalars get 0, strings
// get `{ null, 0, 0 }`, closures get a null fat pointer. The emitter
// produces `Constant::getNullValue(fieldTy)` for the no-default case.
//
// ─── The fn → cls Widening on Fields ──────────────────────────────────────
// A field declared `cls (T) -> U` accepts a `fn` initializer via the
// implicit widening. `coerceTo` handles that before the field type is
// checked against the LLVM type.

Val Emitter::emitStructLiteral(StructLiteralExprAST* expr) {
    assert(expr && "emitStructLiteral() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Resolve the struct declaration ───────────────────────────────────
    // Sema sets `expr->resolvedDecl` to the concrete (specialized, if
    // generic) struct declaration. If it's null, the literal is
    // malformed.
    StructDeclAST* structDecl = expr->resolvedDecl;
    if (!structDecl) {
        // Try resolving through the named type, as a fallback. Sema
        // usually sets both, but a stale or unset `resolvedDecl` on the
        // literal with a valid `resolvedType` should still work.
        if (expr->resolvedType && expr->resolvedType->isa<NamedTypeAST>()) {
            NamedTypeAST* named = expr->resolvedType->as<NamedTypeAST>();
            if (named->resolvedDecl
                && named->resolvedDecl->isa<StructDeclAST>()) {
                structDecl = named->resolvedDecl->as<StructDeclAST>();
            }
        }
    }
    if (!structDecl) {
        program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->loc,
            "struct literal '", program.pool.lookup(expr->typeName),
            "' has no resolved declaration");
        return {};
    }

    // ─── LLVM struct type ─────────────────────────────────────────────────
    llvm::StructType* structTy = program.types().structType(structDecl);
    if (!structTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "struct '", program.pool.lookup(structDecl->name),
            "' has no LLVM type");
        return {};
    }

    // ─── Field index map ──────────────────────────────────────────────────
    // Built once per literal. For a struct with many fields, this could
    // be cached on the struct declaration; for typical struct sizes (a
    // handful of fields), the map is cheap.
    std::unordered_map<InternedString, size_t> fieldIndex;
    fieldIndex.reserve(structDecl->fields.size());
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        fieldIndex[structDecl->fields[i]->name] = i;
    }

    // ─── Track which fields were initialized ──────────────────────────────
    // After processing the source-order initializers, we walk the fields
    // again to fill in any that weren't initialized. The `bool` vector is
    // a small, stack-friendly tracker.
    std::vector<bool> initialized(structDecl->fields.size(), false);

    // ─── Build the struct value ───────────────────────────────────────────
    llvm::Value* result = llvm::UndefValue::get(structTy);

    for (FieldInitAST* init : expr->inits) {
        if (!init) continue;

        auto it = fieldIndex.find(init->name);
        if (it == fieldIndex.end()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_FieldNotFound, init->loc,
                "struct '", program.pool.lookup(structDecl->name),
                "' has no field '", program.pool.lookup(init->name), "'");
            return {};
        }

        size_t index = it->second;
        FieldDeclAST* field = structDecl->fields[index];
        if (!field) continue;

        initialized[index] = true;

        // ─── Emit the field value ─────────────────────────────────────────
        Val fieldVal = emit(init->value);
        if (!fieldVal.isValid()) return {};

        // ─── Coerce to the field's declared type ──────────────────────────
        // Handles `fn → cls` widening, integer width, pointer cast.
        fieldVal = coerceTo(fieldVal, field->type);
        if (!fieldVal.isValid()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_TypeMismatch, init->loc,
                "field '", program.pool.lookup(init->name),
                "' initializer cannot be coerced to its declared type");
            return {};
        }

        // ─── LLVM type check ──────────────────────────────────────────────
        // After coercion, the value's LLVM type should match the struct's
        // field type. A mismatch here is a `Types` bug or an Sema bug.
        llvm::Type* expectedTy = structTy->getElementType(
            static_cast<unsigned>(index));
        if (fieldVal.v->getType() != expectedTy) {
            // Try a final LLVM-level coercion.
            llvm::Value* coerced = coerceValueToType(
                fieldVal.v, expectedTy, b);
            if (coerced) {
                fieldVal.v = coerced;
            }
            if (fieldVal.v->getType() != expectedTy) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, init->loc,
                    "field '", program.pool.lookup(init->name),
                    "' has a type mismatch between AST and LLVM");
                return {};
            }
        }

        // ─── Acquire a fresh claim ────────────────────────────────────────
        // The struct owns the field's claim. For a resource field
        // initialized from a `Borrowed` source, this deep-copies
        // (OwnedBuffer) or retains (Refcounted). For a scalar field,
        // no-op.
        Val owned = program.ownership().intoOwned(fieldVal, b);
        if (!owned.isValid()) return {};

        // ─── Insert the field into the struct ─────────────────────────────
        result = b.CreateInsertValue(
            result, owned.v, static_cast<unsigned>(index),
            "field." + program.pool.lookup(field->name));
    }

    // ─── Fill uninitialized fields with defaults ──────────────────────────
    // A field with no init in the literal takes its declared default (if
    // it has one) or its zero value (if it doesn't). Sema's rule is that
    // a field without a default and without an init must be
    // zero-initializable, which is true for scalars, null pointers, and
    // empty aggregates.
    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        if (initialized[i]) continue;

        FieldDeclAST* field = structDecl->fields[i];
        if (!field) continue;

        llvm::Type* fieldTy = structTy->getElementType(
            static_cast<unsigned>(i));

        if (field->defaultVal) {
            // ─── Field has a default: emit it ─────────────────────────────
            Val defVal = emit(field->defaultVal);
            if (!defVal.isValid()) {
                // Fall back to zero. Emitting an invalid value for a
                // default is a bug, but the zero keeps the IR well-formed.
                defVal = Val{
                    llvm::Constant::getNullValue(fieldTy),
                    field->type, Own::Owned};
            } else {
                defVal = coerceTo(defVal, field->type);
                if (!defVal.isValid()) {
                    defVal = Val{
                        llvm::Constant::getNullValue(fieldTy),
                        field->type, Own::Owned};
                }
            }

            // LLVM type check.
            if (defVal.v->getType() != fieldTy) {
                llvm::Value* coerced = coerceValueToType(
                    defVal.v, fieldTy, b);
                if (coerced) {
                    defVal.v = coerced;
                }
                if (defVal.v->getType() != fieldTy) {
                    defVal.v = llvm::Constant::getNullValue(fieldTy);
                }
            }

            // Acquire the claim.
            Val owned = program.ownership().intoOwned(defVal, b);
            if (!owned.isValid()) return {};

            result = b.CreateInsertValue(
                result, owned.v, static_cast<unsigned>(i),
                "default." + program.pool.lookup(field->name));
        } else {
            // ─── No default: zero-init ────────────────────────────────────
            // Scalar → 0; pointer → null; aggregate → zeroed aggregate.
            // No claim is acquired because there's no resource to claim.
            llvm::Value* zero = llvm::Constant::getNullValue(fieldTy);
            result = b.CreateInsertValue(
                result, zero, static_cast<unsigned>(i),
                "zero." + program.pool.lookup(field->name));
        }
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitArrayLiteral — an array value built from element expressions
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Fixed Arrays ─────────────────────────────────────────────────────────
// A fixed array `[N]T` is an LLVM `[N x T]` aggregate. The emitter has two
// strategies:
//
//   - **All elements are LLVM constants**: build the array with
//     `llvm::ConstantArray::get`. The optimiser keeps this as a constant.
//
//   - **Some element is not constant**: spill to a stack slot, store each
//     element into its slot, and load the array back. The optimiser
//     usually folds the alloca into SSA values.
//
// The second strategy is chosen whenever any element's value isn't
// `Constant`, which the emitter detects after emitting all elements.
//
// ─── Empty Literals ───────────────────────────────────────────────────────
// `[]` has no elements. The emitter produces a zero-length array for a
// fixed-array context, or a null slice for a slice context. Sema
// determines the target shape from the surrounding type.
//
// ─── Dynamic Arrays ───────────────────────────────────────────────────────
// A dynamic array `[*]T` is currently a bare `ptr`. There's no way to
// construct one from a literal: the emitter would need to allocate a
// buffer and return the pointer, but the caller has no length to go with
// it. Until the dynamic-array lowering stabilizes to the `{ ptr, i64, i64 }`
// shape, this emitter diagnoses the case.
//
// ─── Slices ───────────────────────────────────────────────────────────────
// A slice `[_]T` is a borrowed view; it can't be built from a literal
// because the literal's elements have no backing buffer to view. The
// language doesn't allow slice literals; Sema rejects them. If a slice
// type appears here, the emitter diagnoses.

Val Emitter::emitArrayLiteral(ArrayLiteralExprAST* expr) {
    assert(expr && "emitArrayLiteral() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Array type ───────────────────────────────────────────────────────
    ArrayTypeAST* arrayTy = expr->resolvedType
        ? (expr->resolvedType->isa<ArrayTypeAST>()
              ? expr->resolvedType->as<ArrayTypeAST>()
              : nullptr)
        : nullptr;
    if (!arrayTy) {
        program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->loc,
            "array literal has no resolved array type");
        return {};
    }

    llvm::Type* arrayLlvmTy = program.types().get(arrayTy);
    if (!arrayLlvmTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "array literal has no LLVM type");
        return {};
    }

    llvm::Type* elemTy = program.types().get(arrayTy->element);
    if (!elemTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "array element type is unresolvable");
        return {};
    }

    // ─── Empty literal ────────────────────────────────────────────────────
    // An empty literal is a zero-initialized value of the array type.
    // For a fixed array `[0]T` (an LLVM `[0 x T]`), the zero value is
    // the empty array. For any other array kind, Sema rejects `[]`.
    if (expr->elements.empty()) {
        return Val{
            llvm::Constant::getNullValue(arrayLlvmTy),
            expr->resolvedType, Own::Owned};
    }

    // ─── Dispatch on array kind ───────────────────────────────────────────
    if (arrayTy->isFixed()) {
        // ─── Fixed array `[N]T` ───────────────────────────────────────────
        llvm::ArrayType* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrayLlvmTy);
        if (!arrTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "fixed array type has an unexpected LLVM shape");
            return {};
        }

        // ─── Emit all elements first ──────────────────────────────────────
        // Emitting elements can have side effects (calls, panics), so
        // they're all emitted before any are inserted. If an element
        // fails, the emitter returns without producing a partial array.
        std::vector<Val> elements;
        elements.reserve(expr->elements.size());
        bool allConstant = true;

        for (ExprAST* elemExpr : expr->elements) {
            if (!elemExpr) return {};

            Val elemVal = emit(elemExpr);
            if (!elemVal.isValid()) return {};

            // Coerce to the element type.
            elemVal = coerceTo(elemVal, arrayTy->element);
            if (!elemVal.isValid()) {
                program.diagnostics.errorAt(
                    DiagCode::Sem_TypeMismatch, elemExpr->loc,
                    "array literal element cannot be coerced to the "
                    "declared element type");
                return {};
            }

            // ─── Type check ───────────────────────────────────────────────
            if (elemVal.v->getType() != elemTy) {
                llvm::Value* coerced = coerceValueToType(
                    elemVal.v, elemTy, b);
                if (coerced) {
                    elemVal.v = coerced;
                }
                if (elemVal.v->getType() != elemTy) {
                    program.diagnostics.errorAt(
                        DiagCode::Backend_CodegenError, elemExpr->loc,
                        "array element type mismatch between AST and LLVM");
                    return {};
                }
            }

            // ─── Acquire a fresh claim ────────────────────────────────────
            // The array owns the element's claim.
            Val owned = program.ownership().intoOwned(elemVal, b);
            if (!owned.isValid()) return {};

            // ─── Track whether this element is constant ───────────────────
            if (!llvm::isa<llvm::Constant>(owned.v)) {
                allConstant = false;
            }

            elements.push_back(owned);
        }

        // ─── Strategy 1: all elements are constants ───────────────────────
        if (allConstant) {
            std::vector<llvm::Constant*> constants;
            constants.reserve(elements.size());
            for (const Val& v : elements) {
                constants.push_back(llvm::cast<llvm::Constant>(v.v));
            }
            llvm::Value* arr = llvm::ConstantArray::get(arrTy, constants);
            return Val{arr, expr->resolvedType, Own::Owned};
        }

        // ─── Strategy 2: spill to a stack slot ────────────────────────────
        // The array is built by storing each element into its slot, then
        // loading the array back as a value. The optimiser usually folds
        // the alloca into SSA.
        llvm::AllocaInst* slot = createEntryAlloca(
            arrTy, "array.literal");
        if (!slot) return {};

        for (size_t i = 0; i < elements.size(); ++i) {
            // GEP to the element's slot.
            llvm::Value* elemPtr = b.CreateInBoundsGEP(
                arrTy, slot,
                {llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(program.llvmContext()), 0),
                 llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(program.llvmContext()),
                    static_cast<uint32_t>(i))},
                "array.literal.elem");
            b.CreateStore(elements[i].v, elemPtr);
        }

        // Load the array back as a value.
        llvm::Value* arr = b.CreateLoad(arrTy, slot, "array.literal.value");
        return Val{arr, expr->resolvedType, Own::Owned};
    }

    if (arrayTy->isSlice()) {
        // Slices are borrowed views; they can't be constructed from a
        // literal because there's no backing buffer to view. Sema rejects
        // slice literals. Reaching here means the emitter was called on a
        // Sema-invalid program.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "slice literals are not supported — a slice must view an "
            "existing array");
        return {};
    }

    if (arrayTy->isDynamic()) {
        // The dynamic-array lowering is in transition. Until it stabilizes
        // to `{ ptr, i64, i64 }`, there's no well-defined way to construct
        // a dynamic array from a literal.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "dynamic array literals are not yet implemented");
        return {};
    }

    program.diagnostics.errorAt(
        DiagCode::Backend_CodegenError, expr->loc,
        "unsupported array kind in array literal");
    return {};
}

} // namespace codegen