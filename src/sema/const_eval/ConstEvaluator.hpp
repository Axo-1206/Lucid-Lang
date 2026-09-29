/**
 * @file ConstEvaluator.hpp
 *
 * @responsibility Compile-time constant folding: the public entry
 *                 points, the internal per-form folders, and the
 *                 operator-specific helpers the evaluator is split
 *                 across.
 *
 * ─── What the evaluator does ──────────────────────────────────────────────
 * A `const_expr` is a syntactic class: literals, arithmetic on
 * literals, and the fixed-table sugar `T.Member`. The grammar puts a
 * `const_expr` in three positions:
 *
 *   - a cell of a `@fixed`/`@readonly` table's inline `= [ ... ]`
 *     initializer (§4.1.1c);
 *   - a top-level `const` binding's initializer (§4.3);
 *   - a `switch` case value (§12.2).
 *
 * The evaluator folds a constant expression to a `ConstantValue`. The
 * folding happens once, during compilation; the result is cached on
 * the expression via `expr->isConst` / `expr->constValue` and read by
 * every later pass.
 *
 * ─── Design: three return states, one entry point ─────────────────────────
 * `evaluate` returns a `ConstantValue` in one of three states:
 *
 *   - **Evaluated** — the expression is a compile-time constant, and
 *     its value is in the returned `ConstantValue`.
 *   - **Unknown** — the expression is not a compile-time constant. No
 *     diagnostic. The caller decides whether that is acceptable.
 *   - **Error** — the expression *is* a constant-shaped expression
 *     whose fold failed (division by zero, integer overflow, ...). A
 *     diagnostic has already been emitted.
 *
 * The three states are distinguished by `isEvaluated()`, `isUnknown()`,
 * and `isError()` on the returned value. A caller that requires a
 * constant checks `isEvaluated()`; a caller that wants "fold if
 * possible, else leave it" checks `isEvaluated()` and ignores the
 * `Unknown` case.
 *
 * ─── Design: no diagnostic for Unknown ───────────────────────────────────
 * "This expression is not a compile-time constant" is not an error in
 * general — it is only an error in the positions that require one. The
 * evaluator returns `Unknown` and lets the caller decide. A `switch`
 * case that is not constant emits `Type_Mismatch` at the case's
 * location; an ordinary expression that happens not to be constant
 * emits nothing.
 *
 * The evaluator does emit diagnostics for actual fold failures —
 * division by zero, shift by a negative, integer overflow. Those go
 * through `ctx.diagnostics.error` with a code from the value band.
 *
 * ─── Design: the evaluator returns; the caller caches ─────────────────────
 * The evaluator never writes to the expression tree. It returns a
 * `ConstantValue` and the caller decides whether to cache it. In
 * practice every caller caches, but the separation makes the evaluator
 * a pure function of `(expr, ctx)` — testable without an AST to
 * mutate.
 *
 * ─── Design: the file split ───────────────────────────────────────────────
 * The evaluator is split across four `.cpp` files, mirroring the
 * operator categories the grammar defines:
 *
 *   - `ConstEvaluator.cpp`   — the dispatcher, plus the identifier,
 *                              field-access, and array-literal cases.
 *   - `ConstEvalLiteral.cpp` — `evaluateLiteral` and the numeric-lexeme
 *                              parsers.
 *   - `ConstEvalUnary.cpp`   — `foldUnary`.
 *   - `ConstEvalBinary.cpp`  — `foldBinary` and its category helpers.
 *
 * The split is historical: the old design had the same four files, and
 * keeping the split lets each operator category stay in one place as
 * the evaluator grows. A single `ConstEvaluator.cpp` would be about
 * 600 lines and fits in one read; the four-file split is about 700
 * lines with comments and is more searchable.
 */

#pragma once

#include "core/ast/BaseAST.hpp"    // for ConstantValue
#include "core/ast/ExprAST.hpp"    // for ExprAST, LiteralExprAST

#include "sema/context/SemaContext.hpp"

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// Public entry points
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Fold an expression to a compile-time constant.
///
/// Returns:
///   - a real `ConstantValue` if the expression is a compile-time
///     constant;
///   - `ConstantValue::unknown()` if the expression is not a
///     compile-time constant (no diagnostic; the caller decides);
///   - `ConstantValue::error()` if the expression *is* a constant-shaped
///     expression whose fold failed (a diagnostic has been emitted).
///
/// The three states are distinguished by `isEvaluated()`, `isUnknown()`,
/// and `isError()`. A caller that requires a constant checks
/// `isEvaluated()`; a caller that wants "fold if possible, else leave
/// it" checks `isEvaluated()` and ignores `Unknown`.
///
/// The evaluator is idempotent: if the expression has already been
/// folded (its `isConst` flag is set), the cached value is returned
/// without re-walking the tree.
ConstantValue evaluate(ExprAST* expr, SemaContext& ctx);

/// @brief Read a single literal node's value.
///
/// The parser stores a numeric literal's raw lexeme — `"42"`, `"0xFF"`,
/// `"3.14"`. This function parses it and returns the typed value. For a
/// string or char literal, the value is the interned lexeme.
///
/// Returns `ConstantValue::unknown()` if `lit` is null or its kind is
/// not one of the eight literal kinds (a compiler bug, since the parser
/// only produces those eight).
///
/// Returns `ConstantValue::error()` if a numeric lexeme cannot be
/// parsed — also a compiler bug, since the parser rejects a malformed
/// numeric token at parse time. The `Error` state lets the caller
/// propagate the failure instead of silently misreading the literal.
ConstantValue evaluateLiteral(LiteralExprAST* lit, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Internal per-form folders
// ═════════════════════════════════════════════════════════════════════════════
//
// These are the dispatch cases of `evaluate`. They are declared here,
// not `static` in `ConstEvaluator.cpp`, because a future caller may
// want to fold a single node of a known kind without going through the
// general dispatcher. Today the only callers are inside the evaluator
// itself.

/// @brief Fold a bare identifier.
///
/// A bare identifier in a `const_expr` position is one of two things:
///
///   - a top-level `FN` name, which produces a `Function` constant;
///   - a reference to a `const` binding whose initializer has already
///     been folded, which produces the binding's cached value.
///
/// Everything else — a `let`, an unfolder `const`, a parameter, a table
/// name, a module alias — is not a compile-time constant, and this
/// function returns `Unknown`.
ConstantValue evaluateIdentifier(IdentifierExprAST* expr, SemaContext& ctx);

/// @brief Fold a field access.
///
/// The only field access that is a compile-time constant is the
/// fixed-table sugar `Direction.North`. The resolver in
/// `resolveTableMemberAccess` sets `isFixedRowSugar` and
/// `hasResolvedFixedRow` when it recognizes the shape and resolves it
/// to a specific row of a `@fixed` or `@readonly` table. The constant
/// value is that row's index, as an `Int`.
///
/// Everything else — a cell access `row.name`, a column view
/// `Person.age`, a module member `math.sqrt`, a `by<Column>` lookup —
/// is either a runtime operation or a function value, and this
/// function returns `Unknown`.
ConstantValue evaluateFieldAccess(FieldAccessExprAST* expr, SemaContext& ctx);

/// @brief Fold an array literal.
///
/// An array literal is a compile-time constant iff every element is.
/// The elements are folded in order; the first non-constant element
/// makes the whole literal non-constant. An error in any element
/// propagates as an error.
///
/// The result is a `ConstantValue` of `Kind::Array`, holding the folded
/// element values.
ConstantValue evaluateArrayLiteral(ArrayLiteralExprAST* expr, SemaContext& ctx);

/// @brief Fold a unary expression.
///
/// Folds the operand, then applies `foldUnary`. If the operand is not a
/// constant, returns `Unknown`; if the operand's fold errored, returns
/// the error.
ConstantValue evaluateUnaryExpr(UnaryExprAST* expr, SemaContext& ctx);

/// @brief Fold a binary expression.
///
/// Folds both operands, then applies `foldBinary`. If either operand is
/// not a constant, returns `Unknown`; if either operand's fold errored,
/// returns the error.
///
/// The evaluator folds *both* sides of `and` / `or` before applying the
/// operator. This differs from the runtime's short-circuit semantics,
/// but a constant expression has no side effects, and folding both
/// sides lets the evaluator catch a fold error in the right operand
/// even when the left would short-circuit.
ConstantValue evaluateBinaryExpr(BinaryExprAST* expr, SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Operator folders
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Apply a unary operator to a folded operand.
///
/// Returns `Unknown` if the operand's kind does not match the operator
/// (e.g. `not 42`, `~3.14`). A kind mismatch is not an error — the type
/// checker, not the evaluator, is responsible for reporting it.
///
/// No diagnostics are emitted by this function. The three unary
/// operators (`-`, `not`, `~`) have no runtime failure modes on a
/// well-typed operand.
ConstantValue foldUnary(UnaryOp op, const ConstantValue& operand,
                        SemaContext& ctx);

/// @brief Apply a binary operator to two folded operands.
///
/// Returns `Unknown` if the operands' kinds do not match the operator
/// (e.g. `1 + "a"`). Returns `Error` (after emitting a diagnostic) for
/// a real fold failure: division by zero, modulo by zero, integer
/// overflow, or a shift amount out of range.
///
/// `+` is special: it is numeric addition on numeric operands and string
/// concatenation on two-string operands. The other arithmetic operators
/// are numeric-only.
ConstantValue foldBinary(BinaryOp op,
                         const ConstantValue& left,
                         const ConstantValue& right,
                         SemaContext& ctx);

// ═════════════════════════════════════════════════════════════════════════════
// Numeric-lexeme parsers
// ═════════════════════════════════════════════════════════════════════════════
//
// The parser stores a numeric literal's raw lexeme. These functions
// read the lexeme and return the value. They are declared here because
// they are defined in `ConstEvalLiteral.cpp` and called from that file;
// keeping them here rather than in a separate `ConstEvalHelpers.hpp`
// avoids an extra header for two functions.
//
// Both return `false` on a malformed lexeme — a compiler bug, since the
// parser rejects a malformed numeric token at parse time. The caller
// decides how to report it.

/// @brief Parse a decimal, hex, binary, or octal integer lexeme.
///
/// Recognizes the three prefix forms (`0x` / `0X`, `0b` / `0B`,
/// `0o` / `0O`) and decimal. A leading `0` without a letter prefix is
/// decimal — the language has no C-style leading-zero octal.
///
/// Returns true and writes `out` on success, false on failure.
bool parseIntLexeme(std::string_view lexeme, int64_t& out);

/// @brief Parse a decimal float lexeme.
///
/// Accepts the lexical form `d+.d+([eE][-+]?d+)?` that the grammar
/// defines for `FLOAT_LIT`.
///
/// Returns true and writes `out` on success, false on failure.
bool parseFloatLexeme(std::string_view lexeme, double& out);

} // namespace lucid::sema