/**
 * @file core/ASTStrings.hpp
 * @brief String conversion helpers for AST enums and nodes.
 *
 * These utilities convert AST enum values (kinds, operators, primitive types,
 * etc.) and AST nodes to human-readable strings. Used throughout the compiler
 * for debug output, serialization, and diagnostics.
 *
 * All functions are inline and header-only for easy inclusion without
 * creating unnecessary compilation dependencies.
 *
 * ─── No LLVM Dependency ─────────────────────────────────────────────────
 * This file lives in core/ and must not depend on LLVM. Token string
 * conversion lives in `core/Tokens.hpp` next to `token_type_name`. LLVM
 * type stringification, if needed, lives in the codegen layer.
 */

#pragma once

#include "ast/StmtAST.hpp"
#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/StringPool.hpp"

#include <string>
#include <cstdint>

// ─── ASTKind to String ─────────────────────────────────────────────────────

inline std::string astKindToString(ASTKind kind) {
    switch (kind) {
        // Unknown
        case ASTKind::Unknown:          return "Unknown";
        case ASTKind::UnknownDecl:      return "UnknownDecl";
        case ASTKind::UnknownExpr:      return "UnknownExpr";
        case ASTKind::UnknownStmt:      return "UnknownStmt";
        case ASTKind::UnknownType:      return "UnknownType";

        // Special
        case ASTKind::ValueDecl:        return "ValueDecl";
        case ASTKind::TypeDecl:         return "TypeDecl";

        // Type nodes
        case ASTKind::PrimitiveType:    return "PrimitiveType";
        case ASTKind::NamedType:        return "NamedType";
        case ASTKind::ArrayType:        return "ArrayType";
        case ASTKind::NullableType:     return "NullableType";
        case ASTKind::FallibleType:     return "FallibleType";
        case ASTKind::CombinedType:     return "CombinedType";
        case ASTKind::RefType:          return "RefType";
        case ASTKind::FuncType:         return "FuncType";

        // Declarations
        case ASTKind::ImportDecl:       return "ImportDecl";
        case ASTKind::VarDecl:          return "VarDecl";
        case ASTKind::Param:            return "Param";
        case ASTKind::GenericParamDecl: return "GenericParamDecl";
        case ASTKind::FuncDecl:         return "FuncDecl";
        case ASTKind::FieldDecl:        return "FieldDecl";
        case ASTKind::StructDecl:       return "StructDecl";
        case ASTKind::EnumVariant:      return "EnumVariant";
        case ASTKind::EnumDecl:         return "EnumDecl";
        case ASTKind::TraitFieldDecl:   return "TraitFieldDecl";
        case ASTKind::TraitDecl:        return "TraitDecl";
        case ASTKind::TraitRequireDecl: return "TraitRequireDecl";
        case ASTKind::SatisfyDecl:      return "SatisfyDecl";
        case ASTKind::DefDecl:          return "DefDecl";
        case ASTKind::HostTypeDecl:     return "HostTypeDecl";
        case ASTKind::TypeAliasDecl:    return "TypeAliasDecl";
        case ASTKind::StaticFnDecl:     return "StaticFnDecl";

        // Expressions
        case ASTKind::LiteralExpr:         return "LiteralExpr";
        case ASTKind::IdentifierExpr:      return "IdentifierExpr";
        case ASTKind::ArrayLiteralExpr:    return "ArrayLiteralExpr";
        case ASTKind::StructLiteralExpr:   return "StructLiteralExpr";
        case ASTKind::FieldInit:           return "FieldInit";
        case ASTKind::BinaryExpr:          return "BinaryExpr";
        case ASTKind::UnaryExpr:           return "UnaryExpr";
        case ASTKind::CallExpr:            return "CallExpr";
        case ASTKind::IndexExpr:           return "IndexExpr";
        case ASTKind::SliceExpr:           return "SliceExpr";
        case ASTKind::FieldAccessExpr:     return "FieldAccessExpr";
        case ASTKind::ModuleAccessExpr:    return "ModuleAccessExpr";
        case ASTKind::AssignExpr:          return "AssignExpr";
        case ASTKind::NullCoalesceExpr:    return "NullCoalesceExpr";
        case ASTKind::PipelineExpr:        return "PipelineExpr";
        case ASTKind::PipelineStep:        return "PipelineStep";
        case ASTKind::AnonFuncExpr:        return "AnonFuncExpr";
        case ASTKind::IfExpr:              return "IfExpr";
        case ASTKind::RangeExpr:           return "RangeExpr";
        case ASTKind::CaseValue:           return "CaseValue";

        // Concurrency
        case ASTKind::AwaitStmt:           return "AwaitStmt";
        case ASTKind::SpawnStmt:           return "SpawnStmt";
        case ASTKind::StartStmt:           return "StartStmt";

        // Statements
        case ASTKind::BlockStmt:           return "BlockStmt";
        case ASTKind::ExprStmt:            return "ExprStmt";
        case ASTKind::DeclStmt:            return "DeclStmt";
        case ASTKind::IfStmt:              return "IfStmt";
        case ASTKind::SwitchStmt:          return "SwitchStmt";
        case ASTKind::SwitchCase:          return "SwitchCase";
        case ASTKind::ForStmt:             return "ForStmt";
        case ASTKind::WhileStmt:           return "WhileStmt";
        case ASTKind::DoWhileStmt:         return "DoWhileStmt";
        case ASTKind::ReturnStmt:          return "ReturnStmt";
        case ASTKind::BreakStmt:           return "BreakStmt";
        case ASTKind::ContinueStmt:        return "ContinueStmt";

        // Root
        case ASTKind::Program:             return "Program";

        // Compiler directives
        case ASTKind::Attribute:           return "Attribute";

        default: return "Unknown(" + std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─── LiteralKind to String ─────────────────────────────────────────────────

inline std::string literalKindToString(LiteralKind kind) {
    switch (kind) {
        case LiteralKind::Int:       return "Int";
        case LiteralKind::Float:     return "Float";
        case LiteralKind::String:    return "String";
        case LiteralKind::RawString: return "RawString";
        case LiteralKind::Char:      return "Char";
        case LiteralKind::Hex:       return "Hex";
        case LiteralKind::Binary:    return "Binary";
        case LiteralKind::True:      return "True";
        case LiteralKind::False:     return "False";
        case LiteralKind::Nil:       return "Nil";
        case LiteralKind::Err:       return "Err";
        default: return "Unknown";
    }
}

// ─── BinaryOp to String ────────────────────────────────────────────────────

inline std::string binaryOpToString(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:    return "+";
        case BinaryOp::Sub:    return "-";
        case BinaryOp::Mul:    return "*";
        case BinaryOp::Div:    return "/";
        case BinaryOp::Pow:    return "**";
        case BinaryOp::Mod:    return "%";
        case BinaryOp::Eq:     return "==";
        case BinaryOp::Ne:     return "!=";
        case BinaryOp::Lt:     return "<";
        case BinaryOp::Gt:     return ">";
        case BinaryOp::Le:     return "<=";
        case BinaryOp::Ge:     return ">=";
        case BinaryOp::And:    return "and";
        case BinaryOp::Or:     return "or";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr:  return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::Shl:    return "<<";
        case BinaryOp::Shr:    return ">>";
        default: return "Unknown";
    }
}

// ─── UnaryOp to String ─────────────────────────────────────────────────────

inline std::string unaryOpToString(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg:    return "-";
        case UnaryOp::Not:    return "not";
        case UnaryOp::BitNot: return "~";
        default: return "Unknown";
    }
}

// ─── AssignOp to String ────────────────────────────────────────────────────

inline std::string assignOpToString(AssignOp op) {
    switch (op) {
        case AssignOp::Assign:       return "=";
        case AssignOp::AddAssign:    return "+=";
        case AssignOp::SubAssign:    return "-=";
        case AssignOp::MulAssign:    return "*=";
        case AssignOp::DivAssign:    return "/=";
        case AssignOp::PowAssign:    return "**=";
        case AssignOp::ModAssign:    return "%=";
        case AssignOp::BitAndAssign: return "&=";
        case AssignOp::BitOrAssign:  return "|=";
        case AssignOp::BitXorAssign: return "^=";
        case AssignOp::ShlAssign:    return "<<=";
        case AssignOp::ShrAssign:    return ">>=";
        default: return "Unknown";
    }
}

// ─── PrimitiveKind to String ───────────────────────────────────────────────

inline std::string primitiveKindToString(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:    return "bool";
        case PrimitiveKind::Byte:    return "byte";
        case PrimitiveKind::Short:   return "short";
        case PrimitiveKind::Int:     return "int";
        case PrimitiveKind::Long:    return "long";
        case PrimitiveKind::Ubyte:   return "ubyte";
        case PrimitiveKind::Ushort:  return "ushort";
        case PrimitiveKind::Uint:    return "uint";
        case PrimitiveKind::Ulong:   return "ulong";
        case PrimitiveKind::Int8:    return "int8";
        case PrimitiveKind::Int16:   return "int16";
        case PrimitiveKind::Int32:   return "int32";
        case PrimitiveKind::Int64:   return "int64";
        case PrimitiveKind::Uint8:   return "uint8";
        case PrimitiveKind::Uint16:  return "uint16";
        case PrimitiveKind::Uint32:  return "uint32";
        case PrimitiveKind::Uint64:  return "uint64";
        case PrimitiveKind::Float:   return "float";
        case PrimitiveKind::Double:  return "double";
        case PrimitiveKind::Decimal: return "decimal";
        case PrimitiveKind::String:  return "string";
        case PrimitiveKind::Char:    return "char";
        default: return "Unknown";
    }
}

// ─── ArrayKind to String ───────────────────────────────────────────────────

inline std::string arrayKindToString(ArrayKind kind) {
    switch (kind) {
        case ArrayKind::Slice:   return "Slice";
        case ArrayKind::Dynamic: return "Dynamic";
        case ArrayKind::Fixed:   return "Fixed";
        default: return "Unknown";
    }
}

// ─── DeclKeyword to String ─────────────────────────────────────────────────

inline std::string declKeywordToString(DeclKeyword keyword) {
    switch (keyword) {
        case DeclKeyword::Let:   return "let";
        case DeclKeyword::Const: return "const";
        default: return "Unknown";
    }
}

// ─── ValueState to String ──────────────────────────────────────────────────

inline std::string valueStateToString(ValueState state) {
    switch (state) {
        case ValueState::None:     return "None";
        case ValueState::Definite: return "Definite";
        case ValueState::Nil:      return "Nil";
        case ValueState::Err:      return "Err";
        case ValueState::Unknown:  return "Unknown";
        default: return "Unknown";
    }
}

// ─── AwaitKind to String ───────────────────────────────────────────────────

inline std::string awaitKindToString(AwaitKind kind) {
    switch (kind) {
        case AwaitKind::Single: return "Single";
        case AwaitKind::All:    return "All";
        case AwaitKind::Any:    return "Any";
        default: return "Unknown";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type to String — full type signature
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a TypeAST to a human-readable string representation.
 *
 * Supports:
 *   - Primitive types: `int`, `float`, `string`, ...
 *   - Named types: `Vec2`, `Buffer<int>`, `Map<string, Vec2>`
 *   - Array types: `[*]int`, `[_]float`, `[4]Vec2`
 *   - Nullable / Fallible / Combined: `T?`, `T!`, `T?!`
 *   - Reference: `&T`
 *   - Function types: `fn (int, string) -> bool`, `cls (a) -> cls (b) -> int`
 */
inline std::string typeToString(TypeAST* type, StringPool& pool) {
    if (!type) return "<null>";

    // ─── PrimitiveType ─────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        auto* prim = type->as<PrimitiveTypeAST>();
        return primitiveKindToString(prim->primitiveKind);
    }

    // ─── NamedType ─────────────────────────────────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        std::string result = std::string(pool.lookup(named->name));
        if (!named->genericArgs.empty()) {
            result += "<";
            for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                if (i > 0) result += ", ";
                result += typeToString(named->genericArgs[i], pool);
            }
            result += ">";
        }
        return result;
    }

    // ─── ArrayType ─────────────────────────────────────────────────────────
    if (type->isa<ArrayTypeAST>()) {
        auto* arr = type->as<ArrayTypeAST>();
        std::string result = "[";
        if (arr->isFixed()) {
            result += std::to_string(arr->size);
        } else if (arr->isSlice()) {
            result += "_";
        } else {
            result += "*";
        }
        result += "]";
        result += typeToString(arr->element, pool);
        return result;
    }

    // ─── NullableType ──────────────────────────────────────────────────────
    if (type->isa<NullableTypeAST>()) {
        auto* nullable = type->as<NullableTypeAST>();
        return typeToString(nullable->inner, pool) + "?";
    }

    // ─── FallibleType ──────────────────────────────────────────────────────
    if (type->isa<FallibleTypeAST>()) {
        auto* fallible = type->as<FallibleTypeAST>();
        return typeToString(fallible->inner, pool) + "!";
    }

    // ─── CombinedType ──────────────────────────────────────────────────────
    if (type->isa<CombinedTypeAST>()) {
        auto* combined = type->as<CombinedTypeAST>();
        return typeToString(combined->inner, pool) + "?!";
    }

    // ─── RefType ───────────────────────────────────────────────────────────
    if (type->isa<RefTypeAST>()) {
        auto* ref = type->as<RefTypeAST>();
        return "&" + typeToString(ref->inner, pool);
    }

    // ─── FuncType ──────────────────────────────────────────────────────────
    if (type->isa<FuncTypeAST>()) {
        auto* func = type->as<FuncTypeAST>();
        std::string result = "fn(";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) result += ", ";
            ParamAST* param = func->params[i];
            if (param->isConstParam) result += "const ";
            result += std::string(pool.lookup(param->name)) + " ";
            result += typeToString(param->type, pool);
            if (param->isVariadic) result += "...";
        }
        result += ")";

        if (func->returnType) {
            result += " -> ";
            result += typeToString(func->returnType, pool);
        }
        return result;
    }

    // ─── Unknown ───────────────────────────────────────────────────────────
    return astKindToString(type->kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// TypeDecl to String — full declaration with fields / variants
// ─────────────────────────────────────────────────────────────────────────────

inline std::string typeDeclToString(TypeDeclAST* decl, StringPool& pool) {
    if (!decl) return "<null>";

    // ─── Struct ────────────────────────────────────────────────────────────
    if (decl->isa<StructDeclAST>()) {
        auto* structDecl = decl->as<StructDeclAST>();
        std::string result = "struct " + std::string(pool.lookup(structDecl->name));

        if (!structDecl->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(structDecl->genericParams[i]->name));
            }
            result += ">";
        }

        if (!structDecl->traitRefs.empty()) {
            result += " : ";
            for (size_t i = 0; i < structDecl->traitRefs.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(structDecl->traitRefs[i]->name));
            }
        }

        result += " { ";
        for (size_t i = 0; i < structDecl->fields.size(); ++i) {
            if (i > 0) result += ", ";
            FieldDeclAST* field = structDecl->fields[i];
            if (field->isConst()) result += "const ";
            if (field->isOpaque) result += "@[opaque] ";
            result += std::string(pool.lookup(field->name)) + " ";
            result += typeToString(field->type, pool);
        }
        result += " }";
        return result;
    }

    // ─── Enum ──────────────────────────────────────────────────────────────
    if (decl->isa<EnumDeclAST>()) {
        auto* enumDecl = decl->as<EnumDeclAST>();
        std::string result = "enum " + std::string(pool.lookup(enumDecl->name));
        if (enumDecl->backingType) {
            result += " : " + typeToString(enumDecl->backingType, pool);
        }
        result += " { ";
        for (size_t i = 0; i < enumDecl->variants.size(); ++i) {
            if (i > 0) result += ", ";
            EnumVariantAST* variant = enumDecl->variants[i];
            result += std::string(pool.lookup(variant->name));
            if (variant->hasValue) {
                result += " = " + std::to_string(variant->value);
            } else if (variant->payloadType) {
                result += "(" + typeToString(variant->payloadType, pool) + ")";
            }
        }
        result += " }";
        return result;
    }

    // ─── Trait ─────────────────────────────────────────────────────────────
    if (decl->isa<TraitDeclAST>()) {
        auto* traitDecl = decl->as<TraitDeclAST>();
        std::string result = "trait " + std::string(pool.lookup(traitDecl->name));

        if (!traitDecl->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < traitDecl->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(traitDecl->genericParams[i]->name));
            }
            result += ">";
        }

        if (!traitDecl->parentTraits.empty()) {
            result += " : ";
            for (size_t i = 0; i < traitDecl->parentTraits.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(traitDecl->parentTraits[i]->name));
            }
        }

        result += " { ";
        bool first = true;
        for (TraitFieldDeclAST* field : traitDecl->fields) {
            if (!first) result += ", ";
            first = false;
            if (field->isConst()) result += "const ";
            result += std::string(pool.lookup(field->name)) + " ";
            result += typeToString(field->type, pool);
        }
        for (TraitRequireDeclAST* req : traitDecl->requires) {
            if (!first) result += ", ";
            first = false;
            result += "REQUIRE ";
            result += std::string(pool.lookup(req->opKindName)) + " ";
            result += std::string(pool.lookup(req->symbol));
        }
        result += " }";
        return result;
    }

    // ─── Host type ─────────────────────────────────────────────────────────
    if (decl->isa<HostTypeDeclAST>()) {
        auto* host = decl->as<HostTypeDeclAST>();
        std::string result = "TYPE " + std::string(pool.lookup(host->name));
        if (!host->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < host->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(host->genericParams[i]->name));
            }
            result += ">";
        }
        result += " = ";
        switch (host->kind) {
            case HostTypeKind::Host:    result += "#host(";    break;
            case HostTypeKind::Native:  result += "#native(";  break;
            case HostTypeKind::Builtin: result += "#builtin("; break;
        }
        result += std::string(pool.lookup(host->targetName)) + ")";
        return result;
    }

    // ─── Type alias ────────────────────────────────────────────────────────
    if (decl->isa<TypeAliasDeclAST>()) {
        auto* alias = decl->as<TypeAliasDeclAST>();
        std::string result = "TYPE " + std::string(pool.lookup(alias->name));
        if (!alias->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < alias->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(alias->genericParams[i]->name));
            }
            result += ">";
        }
        result += " = " + typeToString(alias->targetType, pool);
        return result;
    }

    return "UnknownTypeDecl(" + astKindToString(decl->kind) + ")";
}