/// @file registry/ArgTypeValidators.cpp
/// @brief Implementation of attribute-argument shape checks.

#include "ArgTypeValidators.hpp"

#include "sema/const_eval/ConstEvaluator.hpp"

#include "core/ASTStrings.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// validateIntArg
// ═════════════════════════════════════════════════════════════════════════════

std::optional<int64_t> validateIntArg(ExprAST* arg, const std::string& argName,
                                      SemaContext& ctx) {
    // ─── Step 1: non-null ───────────────────────────────────────────────
    if (!arg) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, nullptr,
                              "argument '", argName, "' is missing");
        return std::nullopt;
    }

    // ─── Step 2: shape — must be a literal ──────────────────────────────
    //
    // The grammar restricts an attribute argument to a literal
    // (`attr_arg ::= STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT`). The
    // parser produces a well-formed argument by construction, but a
    // caller that receives an `ExprAST*` from anywhere else (a future
    // LSP path that resolves a synthesized node) needs the check.
    if (!arg->isa<LiteralExprAST>()) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, arg,
                              "argument '", argName,
                              "' must be an integer literal");
        return std::nullopt;
    }

    // ─── Step 3: kind — must be an integer literal ──────────────────────
    LiteralExprAST* lit = arg->as<LiteralExprAST>();
    switch (lit->kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
        case LiteralKind::Octal:
            break;
        default:
            ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, arg,
                                  "argument '", argName,
                                  "' must be an integer literal, got ",
                                  literalKindToString(lit->kind));
            return std::nullopt;
    }

    // ─── Step 4: value — read through the const evaluator ───────────────
    //
    // The evaluator's `evaluateLiteral` handles the lexeme parsing: the
    // radix detection (`0x`, `0b`, `0o`), the digit reading, and the
    // `int64_t` range check. A failure here means the lexeme was
    // malformed — a parser bug, since the parser rejects a malformed
    // numeric token at parse time.
    ConstantValue val = evaluateLiteral(lit, ctx);
    if (!val.isInt()) {
        // The evaluator emitted its own diagnostic (Value_IntegerOverflow
        // or similar). Return nullopt; the caller stops.
        return std::nullopt;
    }

    return val.asInt();
}

// ═════════════════════════════════════════════════════════════════════════════
// validateStringArg
// ═════════════════════════════════════════════════════════════════════════════

std::optional<InternedString> validateStringArg(ExprAST* arg,
                                                const std::string& argName,
                                                SemaContext& ctx) {
    // ─── Step 1: non-null ───────────────────────────────────────────────
    if (!arg) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, nullptr,
                              "argument '", argName, "' is missing");
        return std::nullopt;
    }

    // ─── Step 2: shape — must be a literal ──────────────────────────────
    if (!arg->isa<LiteralExprAST>()) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, arg,
                              "argument '", argName,
                              "' must be a string literal");
        return std::nullopt;
    }

    // ─── Step 3: kind — must be a string literal ────────────────────────
    LiteralExprAST* lit = arg->as<LiteralExprAST>();
    if (lit->kind != LiteralKind::String && lit->kind != LiteralKind::RawString) {
        ctx.diagnostics.error(DiagCode::Attr_InvalidArgValue, arg,
                              "argument '", argName,
                              "' must be a string literal, got ",
                              literalKindToString(lit->kind));
        return std::nullopt;
    }

    // ─── Step 4: value — the parser already stored it ───────────────────
    //
    // No evaluation needed. A string literal's value is fully determined
    // by its text; the parser stripped the quotes, processed escapes,
    // and interned the result. The `InternedString` on the node is the
    // value the caller wants.
    return lit->value;
}

} // namespace lucid::sema