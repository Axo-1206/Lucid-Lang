/// @file LiteralHelpers.hpp
/// @brief Helpers for inspecting literal AST nodes.
/// 
/// These operate on AST nodes (ExprAST), not tokens.
/// They are used by both AttributeRegistry and IntrinsicRegistry.
/// 
/// @note This is distinct from Token-level helpers in Tokens.hpp,
///       which operate on lexed tokens before AST construction.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/StringPool.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <optional>
#include <string>

namespace sema {
namespace literal {

// ─────────────────────────────────────────────────────────────────────────────
// Literal Type Checks (AST level)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if an expression is a string literal.
inline bool isStringLiteral(const ExprAST* expr) {
    if (!expr || !expr->isa<LiteralExprAST>()) return false;
    const LiteralExprAST* literal = expr->as<LiteralExprAST>();
    return literal->kind == LiteralKind::String ||
           literal->kind == LiteralKind::RawString;
}

/// @brief Check if an expression is an integer literal.
inline bool isIntLiteral(const ExprAST* expr) {
    if (!expr || !expr->isa<LiteralExprAST>()) return false;
    const LiteralExprAST* literal = expr->as<LiteralExprAST>();
    return literal->kind == LiteralKind::Int ||
           literal->kind == LiteralKind::Hex ||
           literal->kind == LiteralKind::Binary ||
           literal->kind == LiteralKind::Char;
}

/// @brief Check if an expression is a float literal.
inline bool isFloatLiteral(const ExprAST* expr) {
    if (!expr || !expr->isa<LiteralExprAST>()) return false;
    const LiteralExprAST* literal = expr->as<LiteralExprAST>();
    return literal->kind == LiteralKind::Float;
}

/// @brief Check if an expression is a boolean literal.
inline bool isBoolLiteral(const ExprAST* expr) {
    if (!expr || !expr->isa<LiteralExprAST>()) return false;
    const LiteralExprAST* literal = expr->as<LiteralExprAST>();
    return literal->kind == LiteralKind::True ||
           literal->kind == LiteralKind::False;
}

/// @brief Check if an expression is any literal.
inline bool isLiteral(const ExprAST* expr) {
    return expr && expr->isa<LiteralExprAST>();
}

// ─────────────────────────────────────────────────────────────────────────────
// Literal Value Extraction (AST level)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Extract string value from a literal expression.
inline std::optional<std::string> extractString(const ExprAST* expr,
                                                  StringPool& pool) {
    if (!isStringLiteral(expr)) return std::nullopt;
    return std::string(pool.lookup(expr->as<LiteralExprAST>()->value));
}

/// @brief Extract integer value from a literal expression.
inline std::optional<int64_t> extractInt(const ExprAST* expr,
                                          StringPool& pool) {
    if (!isIntLiteral(expr)) return std::nullopt;
    const LiteralExprAST* literal = expr->as<LiteralExprAST>();
    std::string value(pool.lookup(literal->value));
    try {
        if (literal->kind == LiteralKind::Hex) {
            return std::stoll(value, nullptr, 16);
        } else if (literal->kind == LiteralKind::Binary) {
            return std::stoll(value, nullptr, 2);
        } else if (literal->kind == LiteralKind::Char) {
            return static_cast<int64_t>(value[0]);
        } else {
            return std::stoll(value);
        }
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Extract float value from a literal expression.
inline std::optional<double> extractFloat(const ExprAST* expr,
                                            StringPool& pool) {
    if (!isFloatLiteral(expr)) return std::nullopt;
    std::string value(pool.lookup(expr->as<LiteralExprAST>()->value));
    try {
        return std::stod(value);
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Extract boolean value from a literal expression.
inline std::optional<bool> extractBool(const ExprAST* expr,
                                        StringPool& pool) {
    if (!isBoolLiteral(expr)) return std::nullopt;
    return expr->as<LiteralExprAST>()->kind == LiteralKind::True;
}

// ─────────────────────────────────────────────────────────────────────────────
// Validation Functions (Convenience wrappers with diagnostics)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate that an argument is a string literal.
inline bool validateStringLiteral(const ExprAST* arg,
                                   SemaContext& ctx,
                                   const std::string& argName) {
    if (!arg || !isStringLiteral(arg)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects a string literal");
        return false;
    }
    return true;
}

/// @brief Validate that an argument is an integer literal.
inline bool validateIntLiteral(const ExprAST* arg,
                                SemaContext& ctx,
                                const std::string& argName) {
    if (!arg || !isIntLiteral(arg)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects an integer literal");
        return false;
    }
    return true;
}

/// @brief Validate that an argument is a float literal.
inline bool validateFloatLiteral(const ExprAST* arg,
                                  SemaContext& ctx,
                                  const std::string& argName) {
    if (!arg || !isFloatLiteral(arg)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects a float literal");
        return false;
    }
    return true;
}

/// @brief Validate that an argument is a boolean literal.
inline bool validateBoolLiteral(const ExprAST* arg,
                                 SemaContext& ctx,
                                 const std::string& argName) {
    if (!arg || !isBoolLiteral(arg)) {
        ctx.error(arg, DiagCode::E3003,
                  "argument '", argName, "' expects a boolean literal");
        return false;
    }
    return true;
}

} // namespace literal
} // namespace sema