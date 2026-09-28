/**
 * @file core/ASTStrings.hpp
 * @brief String conversion helpers for AST enums and nodes.
 *
 * These utilities convert AST enum values (kinds, operators, primitive
 * types, etc.) and AST nodes to human-readable strings. Used throughout
 * the compiler for debug output, serialization, and diagnostics.
 *
 * All functions are inline and header-only for easy inclusion without
 * creating unnecessary compilation dependencies.
 *
 * ─── No LLVM Dependency ─────────────────────────────────────────────────
 * This file lives in core/ and must not depend on LLVM. Token string
 * conversion lives in `core/Tokens.hpp` next to `tokenTypeName`. LLVM
 * type stringification, if needed, lives in the codegen layer.
 *
 * ─── Design: one stringifier per enum, one per node family ──────────────
 * Enum stringifiers are simple: a switch over the enum's values,
 * returning the canonical spelling. Node stringifiers are recursive:
 * `typeToString` recurses into a type's children, `declToString` recurses
 * into a declaration's name and type, and so on. The recursion terminates
 * because every node family's tree is finite.
 *
 * ─── Design: unmatched values return a marker ───────────────────────────
 * An enum value with no case returns `"Unknown(N)"`, where N is the
 * numeric value. This makes a missing case visible in the output rather
 * than silently wrong. A node kind with no case returns its `ASTKind`
 * spelling via `astKindToString`.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/StringPool.hpp"

#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string astKindToString(ASTKind kind) {
    switch (kind) {
        // ─── Family bases ──────────────────────────────────────────────
        case ASTKind::ValueDecl:        return "ValueDecl";
        case ASTKind::TypeDecl:         return "TypeDecl";

        // ─── Type nodes ────────────────────────────────────────────────
        case ASTKind::PrimitiveType:    return "PrimitiveType";
        case ASTKind::NamedType:        return "NamedType";
        case ASTKind::ArrayType:        return "ArrayType";
        case ASTKind::RowRefType:       return "RowRefType";
        case ASTKind::FunctionType:     return "FunctionType";
        case ASTKind::NullableType:     return "NullableType";

        // ─── Declaration nodes ─────────────────────────────────────────
        case ASTKind::ImportDecl:       return "ImportDecl";
        case ASTKind::TableDecl:        return "TableDecl";
        case ASTKind::ColumnDecl:       return "ColumnDecl";
        case ASTKind::FnDecl:           return "FnDecl";
        case ASTKind::VarDecl:          return "VarDecl";
        case ASTKind::Param:            return "Param";

        // ─── Expression nodes ──────────────────────────────────────────
        case ASTKind::LiteralExpr:      return "LiteralExpr";
        case ASTKind::IdentifierExpr:   return "IdentifierExpr";
        case ASTKind::ArrayLiteralExpr: return "ArrayLiteralExpr";
        case ASTKind::FieldAccessExpr:  return "FieldAccessExpr";
        case ASTKind::IndexExpr:        return "IndexExpr";
        case ASTKind::CallExpr:         return "CallExpr";
        case ASTKind::LambdaExpr:       return "LambdaExpr";
        case ASTKind::StartExpr:        return "StartExpr";
        case ASTKind::UnaryExpr:        return "UnaryExpr";
        case ASTKind::BinaryExpr:       return "BinaryExpr";
        case ASTKind::AssignExpr:       return "AssignExpr";
        case ASTKind::ParenExpr:        return "ParenExpr";
        case ASTKind::RangeExpr:        return "RangeExpr";

        // ─── Statement nodes ───────────────────────────────────────────
        case ASTKind::BlockStmt:        return "BlockStmt";
        case ASTKind::VarDeclStmt:      return "VarDeclStmt";
        case ASTKind::AssignStmt:       return "AssignStmt";
        case ASTKind::ExprStmt:         return "ExprStmt";
        case ASTKind::ReturnStmt:       return "ReturnStmt";
        case ASTKind::BreakStmt:        return "BreakStmt";
        case ASTKind::ContinueStmt:     return "ContinueStmt";
        case ASTKind::IfStmt:           return "IfStmt";
        case ASTKind::SwitchStmt:       return "SwitchStmt";
        case ASTKind::SwitchCase:       return "SwitchCase";
        case ASTKind::WhileStmt:        return "WhileStmt";
        case ASTKind::ForStmt:          return "ForStmt";

        // ─── Sequence suspend points ───────────────────────────────────
        case ASTKind::WaitStmt:            return "WaitStmt";
        case ASTKind::WaitFramesStmt:      return "WaitFramesStmt";
        case ASTKind::WaitUntilStmt:       return "WaitUntilStmt";
        case ASTKind::WaitForEventStmt:    return "WaitForEventStmt";
        case ASTKind::WaitForRequestStmt:  return "WaitForRequestStmt";

        // ─── Root and shared ───────────────────────────────────────────
        case ASTKind::Module:           return "Module";
        case ASTKind::Attribute:        return "Attribute";

        // ─── Error recovery ────────────────────────────────────────────
        case ASTKind::Unknown:          return "Unknown";
        case ASTKind::UnknownDecl:      return "UnknownDecl";
        case ASTKind::UnknownExpr:      return "UnknownExpr";
        case ASTKind::UnknownStmt:      return "UnknownStmt";
        case ASTKind::UnknownType:      return "UnknownType";

        // ─── Forward-compatibility: any enum value not listed above ────
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LiteralKind to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string literalKindToString(LiteralKind kind) {
    switch (kind) {
        case LiteralKind::Int:       return "Int";
        case LiteralKind::Float:     return "Float";
        case LiteralKind::String:    return "String";
        case LiteralKind::RawString: return "RawString";
        case LiteralKind::Char:      return "Char";
        case LiteralKind::Hex:       return "Hex";
        case LiteralKind::Binary:    return "Binary";
        case LiteralKind::Octal:     return "Octal";
        case LiteralKind::True:      return "True";
        case LiteralKind::False:     return "False";
        case LiteralKind::Nil:       return "Nil";
        case LiteralKind::Unknown:   return "Unknown";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOp to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string binaryOpToString(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:          return "+";
        case BinaryOp::Sub:          return "-";
        case BinaryOp::Mul:          return "*";
        case BinaryOp::Div:          return "/";
        case BinaryOp::Mod:          return "%";
        case BinaryOp::Pow:          return "**";
        case BinaryOp::Eq:           return "==";
        case BinaryOp::Ne:           return "!=";
        case BinaryOp::Lt:           return "<";
        case BinaryOp::Le:           return "<=";
        case BinaryOp::Gt:           return ">";
        case BinaryOp::Ge:           return ">=";
        case BinaryOp::And:          return "and";
        case BinaryOp::Or:           return "or";
        case BinaryOp::BitAnd:       return "&";
        case BinaryOp::BitOr:        return "|";
        case BinaryOp::BitXor:       return "^";
        case BinaryOp::Shl:          return "<<";
        case BinaryOp::Shr:          return ">>";
        case BinaryOp::NullCoalesce: return "??";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(op)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOp to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string unaryOpToString(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg:    return "-";
        case UnaryOp::Not:    return "not";
        case UnaryOp::BitNot: return "~";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(op)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AssignOp to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string assignOpToString(AssignOp op) {
    switch (op) {
        case AssignOp::Assign:       return "=";
        case AssignOp::AddAssign:    return "+=";
        case AssignOp::SubAssign:    return "-=";
        case AssignOp::MulAssign:    return "*=";
        case AssignOp::DivAssign:    return "/=";
        case AssignOp::ModAssign:    return "%=";
        case AssignOp::BitAndAssign: return "&=";
        case AssignOp::BitOrAssign:  return "|=";
        case AssignOp::BitXorAssign: return "^=";
        case AssignOp::ShlAssign:    return "<<=";
        case AssignOp::ShrAssign:    return ">>=";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(op)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string primitiveKindToString(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:    return "bool";
        case PrimitiveKind::Char:    return "char";
        case PrimitiveKind::String:  return "string";
        case PrimitiveKind::Unit:    return "unit";

        case PrimitiveKind::Int8:    return "int8";
        case PrimitiveKind::Int16:   return "int16";
        case PrimitiveKind::Int32:   return "int32";
        case PrimitiveKind::Int64:   return "int64";

        case PrimitiveKind::Uint8:   return "uint8";
        case PrimitiveKind::Uint16:  return "uint16";
        case PrimitiveKind::Uint32:  return "uint32";
        case PrimitiveKind::Uint64:  return "uint64";

        case PrimitiveKind::Float32: return "float32";
        case PrimitiveKind::Float64: return "float64";

        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrayKind to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string arrayKindToString(ArrayKind kind) {
    switch (kind) {
        case ArrayKind::Dynamic: return "Dynamic";
        case ArrayKind::Fixed:   return "Fixed";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ResourceKind to String
// ─────────────────────────────────────────────────────────────────────────────
//
// ResourceKind lives in core/ast/ResourceKind.hpp. It is stringified here
// alongside the other AST enums because it appears on ValueDeclAST and is
// useful in debug output.

inline std::string resourceKindToString(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::None:         return "None";
        case ResourceKind::Refcounted:   return "Refcounted";
        case ResourceKind::OwnedBuffer:  return "OwnedBuffer";
        case ResourceKind::Aggregate:    return "Aggregate";
        default:
            return "Unknown(" +
                   std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type to String — full type signature
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a TypeAST to a human-readable string representation.
 *
 * Supports every type form in the new grammar:
 *   - Primitive:  `int`, `float`, `string`, ...
 *   - Named:      `Person`, `weapons.Item`
 *   - Row ref:    `&Person`, `&weapons.Item`
 *   - Array:      `[int]` (dynamic), `[5, int]` (fixed)
 *   - Function:   `(int, string) -> bool`
 *   - Nullable:   `int?`, `[int]?`, `[5, int]?`
 *   - Unknown:    the AST kind's spelling
 *
 * The function recurses into children. It never returns null; a null
 * input produces `"<null>"`.
 */
inline std::string typeToString(TypeAST* type, StringPool& pool) {
    if (type == nullptr) return "<null>";

    // ─── PrimitiveType ─────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        auto* prim = type->as<PrimitiveTypeAST>();
        return primitiveKindToString(prim->primitiveKind);
    }

    // ─── NamedType ─────────────────────────────────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        std::string result;
        if (named->isQualified()) {
            result += std::string(pool.lookupView(named->qualifier));
            result += ".";
        }
        result += std::string(pool.lookupView(named->name));
        return result;
    }

    // ─── ArrayType ─────────────────────────────────────────────────────────
    //
    // The new syntax puts everything inside the brackets. Dynamic:
    // `[T]`. Fixed: `[N, T]`.
    if (type->isa<ArrayTypeAST>()) {
        auto* arr = type->as<ArrayTypeAST>();
        std::string result = "[";
        if (arr->isFixed()) {
            result += std::to_string(arr->fixedSize);
            result += ", ";
            result += typeToString(arr->element, pool);
        } else {
            result += typeToString(arr->element, pool);
        }
        result += "]";
        return result;
    }

    // ─── NullableType ──────────────────────────────────────────────────────
    if (type->isa<NullableTypeAST>()) {
        auto* nullable = type->as<NullableTypeAST>();
        return typeToString(nullable->inner, pool) + "?";
    }

    // ─── RowRefType ────────────────────────────────────────────────────────
    if (type->isa<RowRefTypeAST>()) {
        auto* ref = type->as<RowRefTypeAST>();
        return "&" + typeToString(ref->inner, pool);
    }

    // ─── FunctionType ──────────────────────────────────────────────────────
    if (type->isa<FunctionTypeAST>()) {
        auto* func = type->as<FunctionTypeAST>();
        std::string result = "(";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) result += ", ";
            result += typeToString(func->params[i], pool);
        }
        result += ")";

        if (func->returnType != nullptr) {
            result += " -> ";
            result += typeToString(func->returnType, pool);
        } else {
            result += " -> unit";
        }
        return result;
    }

    // ─── UnknownType or any unrecognized kind ──────────────────────────────
    return astKindToString(type->kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Declaration to String
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a DeclAST to a human-readable string representation.
 *
 * Supports every declaration form in the new grammar:
 *   - Import:  `import a.b.c as d`
 *   - Table:   `TABLE Person { ... }`
 *   - Fn:      `FN name(params) -> Ret { ... }`
 *   - Var:     `let x: T = e` / `const x: T = e`
 *   - Param:   `name: T`
 *   - Column:  `name: T`
 *   - Unknown: the AST kind's spelling
 *
 * The function recurses into a declaration's type and children. It never
 * returns null; a null input produces `"<null>"`.
 */
inline std::string declToString(DeclAST* decl, StringPool& pool) {
    if (decl == nullptr) return "<null>";

    // ─── ImportDecl ────────────────────────────────────────────────────────
    if (decl->isa<ImportDeclAST>()) {
        auto* import = decl->as<ImportDeclAST>();
        std::string result = "import ";
        result += std::string(pool.lookupView(import->path));
        if (import->alias.isValid() &&
            import->alias != import->path) {
            result += " as ";
            result += std::string(pool.lookupView(import->alias));
        }
        return result;
    }

    // ─── TableDecl ─────────────────────────────────────────────────────────
    if (decl->isa<TableDeclAST>()) {
        auto* table = decl->as<TableDeclAST>();
        std::string result;
        if (table->isFixed) result += "FIXED ";
        result += "TABLE ";
        result += std::string(pool.lookupView(table->name));

        if (table->isHostBacked) {
            result += " = host(\"";
            result += std::string(pool.lookupView(table->hostName));
            result += "\")";
            return result;
        }

        result += " {";
        for (size_t i = 0; i < table->columns.size(); ++i) {
            if (i > 0) result += ",";
            result += " ";
            result += std::string(
                pool.lookupView(table->columns[i]->name));
            result += ": ";
            result += typeToString(table->columns[i]->type, pool);
        }
        result += " }";

        if (!table->rows.empty()) {
            result += " = [";
            for (size_t i = 0; i < table->rows.size(); ++i) {
                if (i > 0) result += ",";
                result += " { ... }";   // cells omitted for brevity
            }
            result += " ]";
        }
        return result;
    }

    // ─── ColumnDecl ────────────────────────────────────────────────────────
    if (decl->isa<ColumnDeclAST>()) {
        auto* column = decl->as<ColumnDeclAST>();
        std::string result = std::string(pool.lookupView(column->name));
        result += ": ";
        result += typeToString(column->type, pool);
        return result;
    }

    // ─── FnDecl ────────────────────────────────────────────────────────────
    if (decl->isa<FnDeclAST>()) {
        auto* fn = decl->as<FnDeclAST>();
        std::string result = "FN ";
        result += std::string(pool.lookupView(fn->name));
        result += "(";
        for (size_t i = 0; i < fn->params.size(); ++i) {
            if (i > 0) result += ", ";
            if (fn->params[i]->isConst) result += "const ";
            result += std::string(pool.lookupView(fn->params[i]->name));
            result += ": ";
            if (fn->params[i]->isVariadic) result += "...";
            result += typeToString(fn->params[i]->type, pool);
        }
        result += ")";

        if (fn->returnType != nullptr) {
            result += " -> ";
            result += typeToString(fn->returnType, pool);
        }

        if (fn->isHostBound) {
            result += " = host(\"";
            result += std::string(pool.lookupView(fn->hostName));
            result += "\")";
        } else if (fn->body != nullptr) {
            result += " { ... }";
        } else {
            result += " { }";
        }
        return result;
    }

    // ─── VarDecl ───────────────────────────────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        auto* var = decl->as<VarDeclAST>();
        std::string result = var->isConst ? "const " : "let ";
        result += std::string(pool.lookupView(var->name));
        result += ": ";
        result += typeToString(var->type, pool);
        if (var->init != nullptr) {
            result += " = ...";   // init expression omitted for brevity
        }
        result += ";";
        return result;
    }

    // ─── Param ─────────────────────────────────────────────────────────────
    if (decl->isa<ParamAST>()) {
        auto* param = decl->as<ParamAST>();
        std::string result;
        if (param->isConst) result += "const ";
        result += std::string(pool.lookupView(param->name));
        result += ": ";
        if (param->isVariadic) result += "...";
        result += typeToString(param->type, pool);
        return result;
    }

    // ─── UnknownDecl or any unrecognized kind ──────────────────────────────
    return astKindToString(decl->kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement to String (compact)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a StmtAST to a short tag indicating its kind.
 *
 * Statements are structural; their bodies can be large. This function
 * returns just the statement's kind and a name if it has one. A full
 * recursive dump of a statement tree belongs in a dedicated AST dumper,
 * not in a single inline function.
 */
inline std::string stmtToString(StmtAST* stmt, StringPool& /*pool*/) {
    if (stmt == nullptr) return "<null>";
    return astKindToString(stmt->kind);
}