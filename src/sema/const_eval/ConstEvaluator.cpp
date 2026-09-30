/// @file ConstEvaluator.cpp
/// @brief Compile-time constant folding — the dispatcher.
///
/// ─── The three return states ─────────────────────────────────────────────
/// `evaluate` returns a `ConstantValue` in one of three states:
///
///   - **Evaluated** — the expression is a compile-time constant, and its
///     value is in the returned `ConstantValue`.
///   - **Unknown** — the expression is not a compile-time constant. No
///     diagnostic. The caller decides whether that is acceptable.
///   - **Error** — the expression *is* a constant-shaped expression
///     whose fold failed (division by zero, integer overflow, ...). A
///     diagnostic has been emitted.
///
/// The dispatcher's job is to walk the expression, call the per-form
/// helpers, and propagate the first `Unknown` or `Error` upward. It
/// never emits a diagnostic on its own — only the leaf folders do.
///
/// ─── Design: no diagnostic for Unknown ───────────────────────────────────
/// "Not a constant" is only an error in the positions that require one.
/// The evaluator returns `Unknown` and lets the caller decide. A
/// `switch` case that is not constant emits `Type_Mismatch` at the
/// case; a `const` binding whose initializer is not constant simply
/// does not fold, and the binding becomes a `let`-like `const` with no
/// cached value.
///
/// ─── Design: the evaluator does not cache on the AST ─────────────────────
/// The evaluator returns a value. The caller sets `expr->isConst` and
/// `expr->constValue`. This keeps the evaluator a pure function of
/// `(expr, ctx)` — a unit test can call `evaluate` and inspect the
/// result without an AST to mutate.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// evaluate — the public entry point
// ═════════════════════════════════════════════════════════════════════════════

ConstantValue evaluate(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    // A parser error-recovery node is never a constant; the parser
    // already reported the syntax error, and Sema stays out of the way.
    if (expr->hasSyntaxError) return ConstantValue::unknown();

    // ─── If the expression is already folded, return the cached value ──
    //
    // This makes the evaluator idempotent: a second call on the same
    // expression returns the same `ConstantValue` without re-walking.
    // The cached value is only set by a previous successful fold, so a
    // cached `Unknown` is not possible — a failed fold is not cached.
    if (expr->isConst) return expr->constValue;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return evaluateLiteral(expr->as<LiteralExprAST>(), ctx);

        case ASTKind::IdentifierExpr:
            return evaluateIdentifier(expr->as<IdentifierExprAST>(), ctx);

        case ASTKind::ParenExpr:
            // A parenthesized expression is transparent: fold the inner.
            return evaluate(expr->as<ParenExprAST>()->inner, ctx);

        case ASTKind::UnaryExpr:
            return evaluateUnaryExpr(expr->as<UnaryExprAST>(), ctx);

        case ASTKind::BinaryExpr:
            return evaluateBinaryExpr(expr->as<BinaryExprAST>(), ctx);

        case ASTKind::FieldAccessExpr:
            return evaluateFieldAccess(expr->as<FieldAccessExprAST>(), ctx);

        case ASTKind::ArrayLiteralExpr:
            return evaluateArrayLiteral(expr->as<ArrayLiteralExprAST>(), ctx);

        // ─── Not constant-shaped nodes ──────────────────────────────────
        //
        // An index, a call, a lambda, a start expression, and a range
        // are never compile-time constants. Returning Unknown tells the
        // caller "not constant", which is the correct answer.
        //
        //   - `T[i]` / `arr[i]` — a runtime operation. A fixed table's
        //     index is theoretically foldable, but the effort is not
        //     worth it: a fixed table's rows are what a switch checks
        //     against, not what a `const_expr` reads.
        //   - A call has side effects, by definition.
        //   - A lambda's value is a code address, but the evaluator
        //     does not model code addresses; the compiler does.
        //   - `start f()` launches a sequence.
        //   - A range is not a first-class value.
        case ASTKind::IndexExpr:
        case ASTKind::CallExpr:
        case ASTKind::LambdaExpr:
        case ASTKind::StartExpr:
        case ASTKind::RangeExpr:
            return ConstantValue::unknown();

        default:
            // A future expression form that this dispatcher does not
            // handle. Returning Unknown is the safe default — the
            // expression is simply not a constant, and the caller
            // will produce the position-specific diagnostic.
            return ConstantValue::unknown();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// evaluateIdentifier
// ═════════════════════════════════════════════════════════════════════════════
//
// A bare identifier in a `const_expr` position is one of two things:
//
//   - a top-level `FN` name, which produces a `Function` constant;
//   - a reference to a `const` binding whose initializer has already
//     been folded, which produces the binding's cached value.
//
// Everything else — a `let`, an unfolder `const`, a parameter, a table
// name, a module alias — is not a compile-time constant.

ConstantValue evaluateIdentifier(IdentifierExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    ValueLookup lv = ctx.lookupValue(expr->name);
    if (!lv.found()) return ConstantValue::unknown();

    // ─── A bare `FN` name: a compile-time code address ──────────────────
    //
    // A function name is a valid `const_expr` because it names a
    // compile-time-known code address. Its `ConstantValue` is the
    // `FnDeclAST*` itself; downstream code that needs the address reads
    // it from the declaration.
    if (lv.kind == ValueLookup::Kind::Function) {
        return ConstantValue(lv.function);
    }

    // ─── A `const` binding: use the cached value, if folded ─────────────
    //
    // A `const` binding's initializer is folded by `resolveVarDecl`
    // during pass 2. If the reference appears in a declaration that
    // pass 2 has already reached, the binding's `init->constValue` is
    // populated and the reference is a compile-time constant.
    //
    // If the reference appears before the binding has been folded (a
    // forward reference to a later `const`, or a reference to a `const`
    // whose own initializer was not foldable), the reference is not a
    // compile-time constant. The evaluator returns Unknown, and the
    // caller decides whether that is an error.
    //
    // A `let` binding is never a compile-time constant, even when its
    // initializer is literal: the binding is mutable, so its value at a
    // later point is not determined by the initializer.
    if (lv.kind == ValueLookup::Kind::Variable) {
        VarDeclAST* var = lv.variable;
        if (var->init && var->init->isConst) {
            return var->init->constValue;
        }
    }

    // ─── Anything else: not a compile-time constant ─────────────────────
    //
    // A parameter is bound at call time, not at compile time. A table
    // name is a sheet, not a value. An import alias is a module, not a
    // value. None of these can appear in a `const_expr` as a value.
    //
    //   - `Kind::Param`   — a parameter; runtime value.
    //   - `Kind::Table`   — a table name; a sheet, not a value.
    //   - `Kind::None`    — already handled above by `found()`.
    return ConstantValue::unknown();
}

// ═════════════════════════════════════════════════════════════════════════════
// evaluateFieldAccess
// ═════════════════════════════════════════════════════════════════════════════
//
// A field access is a compile-time constant in two shapes:
//
//   ─── Fixed-row sugar: `T.Member` ───────────────────────────────────────
//   On a `@fixed`/`@readonly` table, `T.Member` resolves at compile time
//   to a specific row of `T`. The constant value is the row's index.
//   The field-access resolver sets `isFixedRowSugar` and
//   `hasResolvedFixedRow`; the evaluator reads them.
//
//   ─── Cross-module member: `module.NAME` ────────────────────────────────
//   A reference to a top-level `let`/`const` binding in an imported
//   module, or to a top-level `FN` in an imported module. The
//   field-access resolver classifies the access as `isModuleAccess`
//   and stores the member's declaration on `resolvedDecl`; the
//   evaluator reads it.
//
//   - If the member is a `const` binding whose initializer has already
//     been folded, the constant is the binding's cached value.
//   - If the member is a `FN` declaration, the constant is a `Function`
//     holding the `FnDeclAST*` — the same `ConstantValue` a same-module
//     `FN` reference produces.
//   - If the member is a `let` binding whose initializer has already
//     been folded, the constant is the binding's *initial value*. A
//     later reassignment by user code does not change what the fold
//     sees (grammar §4.1.1c).
//   - Anything else — an unfolder binding, a table name, a `by<Column>`
//     lookup, a module member that is not a `let`/`const`/`FN` — is not
//     a compile-time constant.
//
// Everything else that reaches this function is not a constant. A cell
// read `row.name` is a runtime operation. A column view `Person.age` is
// a live view, not a value. A `by<Column>` lookup is a function value
// synthesized at each use — it is not one of the two constant shapes
// above.

ConstantValue evaluateFieldAccess(FieldAccessExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    // ─── Fixed-row sugar: the value is the row's index ──────────────────
    if (expr->isFixedRowSugar) {
        if (!expr->hasResolvedFixedRow) return ConstantValue::unknown();
        return ConstantValue(static_cast<int64_t>(expr->resolvedFixedRowIndex));
    }

    // ─── Cross-module member: `module.NAME` ─────────────────────────────
    if (expr->isModuleAccess) {
        DeclAST* member = expr->resolvedDecl;

        // Defensive: a caller that evaluates a node in isolation (a
        // hover tooltip, a diagnostic renderer) may see a field
        // access that the resolver has not classified yet. Resolve
        // the member here rather than returning Unknown for a shape
        // that is actually foldable.
        if (!member && expr->object &&
            expr->object->isa<IdentifierExprAST>()) {
            IdentifierExprAST* objId =
                expr->object->as<IdentifierExprAST>();
            ModuleAST* module = ctx.lookupImport(objId->name);
            if (module) {
                // Value member first (variables, then functions).
                member = ctx.lookupModuleValueMember(module, expr->fieldName);
                if (!member) {
                    // A table member is not a `ValueDeclAST`; look it up
                    // through the table-specific path. It is not a
                    // `const_expr` value today, but resolving it keeps
                    // the diagnostic ("a table name is not a
                    // compile-time constant") anchored on the right
                    // declaration rather than on an unresolved member.
                    member = ctx.lookupModuleTableMember(module,
                                                         expr->fieldName);
                }
            }
        }

        if (!member) return ConstantValue::unknown();

        // A `const` or `let` binding: the cached value of its
        // initializer, provided it has already been folded. The two
        // are treated identically here because the fold is over the
        // *initial value*; `let`'s mutability affects what the user
        // may do afterwards, not what the fold reads.
        if (member->isa<VarDeclAST>()) {
            VarDeclAST* var = member->as<VarDeclAST>();
            if (var->init && var->init->isConst) {
                return var->init->constValue;
            }
            return ConstantValue::unknown();
        }

        // A `FN` name: a compile-time-known code address. Same
        // representation as a same-module `FN` reference.
        if (member->isa<FnDeclAST>()) {
            return ConstantValue(member->as<FnDeclAST>());
        }

        // A `TABLE` name: the sheet itself, not a `const_expr` value
        // in any supported position. Unknown.
        return ConstantValue::unknown();
    }

    return ConstantValue::unknown();
}

// ═════════════════════════════════════════════════════════════════════════════
// evaluateArrayLiteral
// ═════════════════════════════════════════════════════════════════════════════
//
// An array literal is a compile-time constant iff every element is. The
// elements are folded in order; the first non-constant element makes
// the whole literal non-constant. An error in any element propagates
// as an error.
//
// The result is a `ConstantValue` of `Kind::Array`, holding the folded
// element values. The array's length is implicit in the vector's size.

ConstantValue evaluateArrayLiteral(ArrayLiteralExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    std::vector<ConstantValue> elements;
    elements.reserve(expr->elements.size());

    for (ExprAST* element : expr->elements) {
        ConstantValue folded = evaluate(element, ctx);
        if (folded.isError()) {
            // A real fold error — propagate it. The element's own
            // evaluator already emitted the diagnostic.
            return folded;
        }
        if (!folded.isEvaluated()) {
            // An element is not a constant. The whole literal is not a
            // constant. Return Unknown, not Error — "this array is not
            // a compile-time constant" is not an error on its own; the
            // caller decides.
            return ConstantValue::unknown();
        }
        elements.push_back(folded);
    }

    ConstantValue result;
    result.kind  = ConstantValue::Kind::Array;
    result.value = std::move(elements);
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// evaluateUnaryExpr / evaluateBinaryExpr
// ═════════════════════════════════════════════════════════════════════════════
//
// These two helpers fold the sub-expressions and hand the results to
// the operator-specific folders in `ConstEvalUnary.cpp` and
// `ConstEvalBinary.cpp`. They are the only place the `Operand not
// folded` propagation lives for unary and binary nodes.

ConstantValue evaluateUnaryExpr(UnaryExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    ConstantValue operand = evaluate(expr->operand, ctx);
    if (operand.isError())   return operand;
    if (!operand.isEvaluated()) return ConstantValue::unknown();

    return foldUnary(expr->op, operand, ctx);
}

ConstantValue evaluateBinaryExpr(BinaryExprAST* expr, SemaContext& ctx) {
    if (!expr) return ConstantValue::unknown();

    ConstantValue left = evaluate(expr->left, ctx);
    if (left.isError())   return left;
    if (!left.isEvaluated()) return ConstantValue::unknown();

    ConstantValue right = evaluate(expr->right, ctx);
    if (right.isError())   return right;
    if (!right.isEvaluated()) return ConstantValue::unknown();

    return foldBinary(expr->op, left, right, ctx);
}

} // namespace lucid::sema