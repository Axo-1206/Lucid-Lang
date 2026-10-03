/// @file registry/ArgTypeValidators.hpp
/// @brief Shape checks for attribute arguments.
///
/// ─── Role ─────────────────────────────────────────────────────────────────
/// An attribute's arguments are literals by the grammar (§9's
/// `attr_arg` production: `STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT`).
/// This file provides the two checks the language's attributes need:
///
///   - `validateIntArg` — is this an integer literal? If so, what is
///     its value?
///   - `validateStringArg` — is this a string literal? If so, what is
///     its interned lexeme?
///
/// Both return `std::optional<T>`. A `nullopt` means the argument did
/// not pass its check; a diagnostic has already been emitted and the
/// caller should skip further work. A non-empty `optional` carries the
/// value, so the caller does not have to re-read the literal.
///
/// ─── Relationship to ConstEvaluator ───────────────────────────────────────
/// This file and `ConstEvaluator` answer different questions. The
/// evaluator asks "does this expression have a compile-time value?" and
/// can fold `1 + 2` to `Int(3)`. This file asks "does this expression
/// have the *shape* an attribute argument requires?" and rejects `1 + 2`
/// because the grammar forbids a non-literal attribute argument.
///
/// `validateIntArg` uses `ConstEvaluator::evaluateLiteral` as the
/// numeric-value reader — one step of its work, not the whole thing.
/// `validateStringArg` does not use the evaluator at all: a string
/// literal's value is the interned lexeme the parser already stored.
///
/// A caller that wants a *foldable expression's* value (a `const`
/// binding's initializer, a `switch` case value, a fixed-table cell)
/// calls `ConstEvaluator::evaluate` directly. This file is specifically
/// for the attribute-argument positions, where the argument's *shape*
/// is a grammar-level constraint.

#pragma once

#include "../context/SemaContext.hpp"

#include "core/ast/ExprAST.hpp"
#include "core/memory/InternedString.hpp"

#include <optional>
#include <string>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Shape checks
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Confirm `arg` is an integer literal and return its value.
///
/// Checks, in order:
///   1. `arg` is not null.
///   2. `arg` is a `LiteralExprAST`.
///   3. The literal's kind is an integer kind — `Int`, `Hex`, `Binary`,
///      or `Octal`.
///   4. The literal's lexeme is parseable as an integer (via
///      `ConstEvaluator::evaluateLiteral`).
///
/// On success, returns the integer value as an `int64_t`. The literal's
/// concrete numeric type is not considered here — attribute arguments
/// are untyped integers, and the caller (e.g. `@reserve`'s validator)
/// decides what range is meaningful.
///
/// On failure, emits `Attr_InvalidArgValue` with a message naming the
/// argument (`argName`) and returns `std::nullopt`.
///
/// @param arg      The argument expression.
/// @param argName  A short noun phrase ("reserve count") used in the
///                 diagnostic.
/// @param ctx      The session context.
///
/// @return the value on success; `std::nullopt` on failure.
std::optional<int64_t> validateIntArg(ExprAST* arg, const std::string& argName,
                                      SemaContext& ctx);

/// @brief Confirm `arg` is a string literal and return its interned
///        lexeme.
///
/// Checks, in order:
///   1. `arg` is not null.
///   2. `arg` is a `LiteralExprAST`.
///   3. The literal's kind is `String` or `RawString`.
///
/// On success, returns the literal's interned lexeme — the same
/// `InternedString` the parser stored on the node. The caller (e.g.
/// `@deprecated`'s validator) can look it up through the pool when it
/// needs to emit it.
///
/// On failure, emits `Attr_InvalidArgValue` with a message naming the
/// argument and returns `std::nullopt`.
///
/// @param arg      The argument expression.
/// @param argName  A short noun phrase ("deprecation message") used in
///                 the diagnostic.
/// @param ctx      The session context.
///
/// @return the interned lexeme on success; `std::nullopt` on failure.
std::optional<InternedString> validateStringArg(ExprAST* arg,
                                                const std::string& argName,
                                                SemaContext& ctx);

} // namespace lucid::sema