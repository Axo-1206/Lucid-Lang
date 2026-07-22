/**
 * @file DebugUtils.hpp
 * @brief Helper utilities for debug logging.
 * 
 * These utilities convert AST nodes, tokens, and types to human-readable
 * strings for debug output. Only used in debug builds.
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/Tokens.hpp"
#include "core/memory/StringPool.hpp"
#include <string>
#include <sstream>

namespace debug {

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string kindToString(ASTKind kind) {
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

        // Types
        case ASTKind::PrimitiveType:    return "PrimitiveType";
        case ASTKind::NamedType:        return "NamedType";
        case ASTKind::ArrayType:        return "ArrayType";
        case ASTKind::NullableType:     return "NullableType";
        case ASTKind::FallibleType:     return "FallibleType";
        case ASTKind::CombinedType:     return "CombinedType";
        case ASTKind::RefType:          return "RefType";
        case ASTKind::PtrType:          return "PtrType";
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
        case ASTKind::TraitRef:         return "TraitRef";

        // Expressions
        case ASTKind::LiteralExpr:        return "LiteralExpr";
        case ASTKind::ArrayLiteralExpr:   return "ArrayLiteralExpr";
        case ASTKind::StructLiteralExpr:  return "StructLiteralExpr";
        case ASTKind::FieldInit:          return "FieldInit";
        case ASTKind::IdentifierExpr:     return "IdentifierExpr";
        case ASTKind::FieldAccessExpr:    return "FieldAccessExpr";
        case ASTKind::ModuleAccessExpr:   return "ModuleAccessExpr";
        case ASTKind::CallExpr:           return "CallExpr";
        case ASTKind::IndexExpr:          return "IndexExpr";
        case ASTKind::SliceExpr:          return "SliceExpr";
        case ASTKind::BinaryExpr:         return "BinaryExpr";
        case ASTKind::UnaryExpr:          return "UnaryExpr";
        case ASTKind::AssignExpr:         return "AssignExpr";
        case ASTKind::NullableChainExpr:  return "NullableChainExpr";
        case ASTKind::NullCoalesceExpr:   return "NullCoalesceExpr";
        case ASTKind::PipelineExpr:       return "PipelineExpr";
        case ASTKind::PipelineStep:       return "PipelineStep";
        case ASTKind::ComposeExpr:        return "ComposeExpr";
        case ASTKind::ComposeOperand:     return "ComposeOperand";
        case ASTKind::AnonFuncExpr:       return "AnonFuncExpr";
        case ASTKind::IfExpr:             return "IfExpr";
        case ASTKind::RangeExpr:          return "RangeExpr";

        // Concurrency
        case ASTKind::AsyncExpr:          return "AsyncExpr";
        case ASTKind::AwaitExpr:          return "AwaitExpr";
        case ASTKind::SpawnExpr:          return "SpawnExpr";
        case ASTKind::JoinExpr:           return "JoinExpr";

        // Statements
        case ASTKind::BlockStmt:          return "BlockStmt";
        case ASTKind::ExprStmt:           return "ExprStmt";
        case ASTKind::DeclStmt:           return "DeclStmt";
        case ASTKind::IfStmt:             return "IfStmt";
        case ASTKind::SwitchStmt:         return "SwitchStmt";
        case ASTKind::SwitchCase:         return "SwitchCase";
        case ASTKind::ForStmt:            return "ForStmt";
        case ASTKind::WhileStmt:          return "WhileStmt";
        case ASTKind::DoWhileStmt:        return "DoWhileStmt";
        case ASTKind::ReturnStmt:         return "ReturnStmt";
        case ASTKind::BreakStmt:          return "BreakStmt";
        case ASTKind::ContinueStmt:       return "ContinueStmt";
        case ASTKind::MultiVarDecl:       return "MultiVarDecl";
        case ASTKind::MultiAssignStmt:    return "MultiAssignStmt";

        // Root
        case ASTKind::Program:            return "Program";

        // Compiler directives
        case ASTKind::Attribute:          return "Attribute";
        case ASTKind::AttributeArg:       return "AttributeArg";
        case ASTKind::IntrinsicCallExpr:  return "IntrinsicCallExpr";

        default: return "Unknown(" + std::to_string(static_cast<int>(kind)) + ")";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Token to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::EOF_TOKEN:      return "EOF";
        case TokenType::IDENTIFIER:     return "IDENTIFIER";
        case TokenType::INT_LITERAL:    return "INT_LITERAL";
        case TokenType::FLOAT_LITERAL:  return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL";
        case TokenType::RAW_STRING_LITERAL: return "RAW_STRING_LITERAL";
        case TokenType::CHAR_LITERAL:   return "CHAR_LITERAL";
        case TokenType::HEX_LITERAL:    return "HEX_LITERAL";
        case TokenType::BINARY_LITERAL: return "BINARY_LITERAL";
        case TokenType::TRUE:           return "TRUE";
        case TokenType::FALSE:          return "FALSE";
        case TokenType::NIL:            return "NIL";
        case TokenType::ERR:            return "ERR";
        case TokenType::UNDERSCORE:     return "_";
        
        // Keywords
        case TokenType::IMPORT:         return "import";
        case TokenType::AS:             return "as";
        case TokenType::STRUCT:         return "struct";
        case TokenType::ENUM:           return "enum";
        case TokenType::TRAIT:          return "trait";
        case TokenType::LET:            return "let";
        case TokenType::CONST:          return "const";
        case TokenType::IF:             return "if";
        case TokenType::ELSE:           return "else";
        case TokenType::SWITCH:         return "switch";
        case TokenType::CASE:           return "case";
        case TokenType::DEFAULT:        return "default";
        case TokenType::WHILE:          return "while";
        case TokenType::FOR:            return "for";
        case TokenType::IN:             return "in";
        case TokenType::DO:             return "do";
        case TokenType::RETURN:         return "return";
        case TokenType::BREAK:          return "break";
        case TokenType::CONTINUE:       return "continue";
        case TokenType::SPAWN:          return "spawn";
        case TokenType::JOIN:           return "join";
        case TokenType::ASYNC:          return "async";
        case TokenType::AWAIT:          return "await";
        case TokenType::AND:            return "and";
        case TokenType::OR:             return "or";
        case TokenType::NOT:            return "not";
        
        // Types
        case TokenType::TYPE_BOOL:      return "bool";
        case TokenType::TYPE_INT8:      return "int8";
        case TokenType::TYPE_INT16:     return "int16";
        case TokenType::TYPE_INT32:     return "int32";
        case TokenType::TYPE_INT64:     return "int64";
        case TokenType::TYPE_UINT8:     return "uint8";
        case TokenType::TYPE_UINT16:    return "uint16";
        case TokenType::TYPE_UINT32:    return "uint32";
        case TokenType::TYPE_UINT64:    return "uint64";
        case TokenType::TYPE_BYTE:      return "byte";
        case TokenType::TYPE_SHORT:     return "short";
        case TokenType::TYPE_INT:       return "int";
        case TokenType::TYPE_LONG:      return "long";
        case TokenType::TYPE_UBYTE:     return "ubyte";
        case TokenType::TYPE_USHORT:    return "ushort";
        case TokenType::TYPE_UINT:      return "uint";
        case TokenType::TYPE_ULONG:     return "ulong";
        case TokenType::TYPE_FLOAT:     return "float";
        case TokenType::TYPE_DOUBLE:    return "double";
        case TokenType::TYPE_DECIMAL:   return "decimal";
        case TokenType::TYPE_STRING:    return "string";
        case TokenType::TYPE_CHAR:      return "char";
        case TokenType::TYPE_FUTURE:    return "Future";
        
        // Operators
        case TokenType::PLUS:           return "+";
        case TokenType::MINUS:          return "-";
        case TokenType::MUL:            return "*";
        case TokenType::DIV:            return "/";
        case TokenType::MOD:            return "%";
        case TokenType::POW:            return "**";
        case TokenType::BIT_AND:        return "&";
        case TokenType::BIT_OR:         return "|";
        case TokenType::BIT_XOR:        return "^";
        case TokenType::BIT_NOT:        return "~";
        case TokenType::SHL:            return "<<";
        case TokenType::SHR:            return ">>";
        case TokenType::EQUAL_EQUAL:    return "==";
        case TokenType::NOT_EQUAL:      return "!=";
        case TokenType::LESS:           return "<";
        case TokenType::LESS_EQUAL:     return "<=";
        case TokenType::GREATER:        return ">";
        case TokenType::GREATER_EQUAL:  return ">=";
        case TokenType::ASSIGN:         return "=";
        case TokenType::PLUS_ASSIGN:    return "+=";
        case TokenType::MINUS_ASSIGN:   return "-=";
        case TokenType::MUL_ASSIGN:     return "*=";
        case TokenType::DIV_ASSIGN:     return "/=";
        case TokenType::MOD_ASSIGN:     return "%=";
        case TokenType::POW_ASSIGN:     return "**=";
        case TokenType::BIT_AND_ASSIGN: return "&=";
        case TokenType::BIT_OR_ASSIGN:  return "|=";
        case TokenType::BIT_XOR_ASSIGN: return "^=";
        case TokenType::SHL_ASSIGN:     return "<<=";
        case TokenType::SHR_ASSIGN:     return ">>=";
        case TokenType::ARROW:          return "->";
        case TokenType::COMPOSE:        return "+>";
        case TokenType::PIPELINE:       return "|>";
        case TokenType::RANGE:          return "..";
        case TokenType::RANGE_EXCLUSIVE:return "..<";
        case TokenType::BANG:           return "!";
        case TokenType::QUESTION:       return "?";
        case TokenType::QUESTION_DOT:   return "?.";
        case TokenType::QUESTION_QUESTION: return "??";
        case TokenType::VARIADIC:       return "...";
        case TokenType::DOT:            return ".";
        case TokenType::COLON:          return ":";
        case TokenType::COMMA:          return ",";
        case TokenType::SEMICOLON:      return ";";
        case TokenType::LPAREN:         return "(";
        case TokenType::RPAREN:         return ")";
        case TokenType::LBRACE:         return "{";
        case TokenType::RBRACE:         return "}";
        case TokenType::LBRACKET:       return "[";
        case TokenType::RBRACKET:       return "]";
        case TokenType::AT_SIGN:        return "@";
        case TokenType::HASH:           return "#";
        case TokenType::AMPERSAND:      return "&";
        case TokenType::ARRAY_STAR:     return "[*]";
        case TokenType::ARRAY_UNDER:    return "[_]";
        case TokenType::DOC_COMMENT:    return "/-- ... --/";
        case TokenType::LINE_COMMENT:   return "-- ...";
        case TokenType::UNKNOWN:        return "UNKNOWN";
        default: return "Token(" + std::to_string(static_cast<int>(type)) + ")";
    }
}

inline std::string tokenToString(const Token& token) {
    std::string result = tokenTypeToString(token.type);
    if (!token.value.empty()) {
        result += "('" + token.value + "')";
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind to String
// ─────────────────────────────────────────────────────────────────────────────

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
        default: return "UnknownPrimitive";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type to String - Full Type Signature
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a TypeAST to a human-readable string representation.
 * 
 * Supports full type signatures including:
 *   - Primitive types: int, float, string, etc.
 *   - Named types: Vec2, Buffer<int>, etc.
 *   - Array types: [*]int, [_]float, [4]Vec2
 *   - Nullable/Fallible/Combined: T?, T!, T?!
 *   - Reference: &T
 *   - Pointer: *T (shown as ptr<T>)
 *   - Function types: (int, string) -> bool
 *   - Struct and enum types with their fields
 */
inline std::string typeToString(TypeAST* type, const StringPool& pool) {
    if (!type) return "<null>";

    // ─── PrimitiveType ──────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        auto* prim = type->as<PrimitiveTypeAST>();
        return primitiveKindToString(prim->primitiveKind);
    }

    // ─── NamedType ──────────────────────────────────────────────────────────
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

    // ─── ArrayType ──────────────────────────────────────────────────────────
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

    // ─── RefType ────────────────────────────────────────────────────────────
    if (type->isa<RefTypeAST>()) {
        auto* ref = type->as<RefTypeAST>();
        return "&" + typeToString(ref->inner, pool);
    }

    // ─── PtrType ────────────────────────────────────────────────────────────
    if (type->isa<PtrTypeAST>()) {
        auto* ptr = type->as<PtrTypeAST>();
        return "*" + typeToString(ptr->inner, pool);
    }

    // ─── FuncType ───────────────────────────────────────────────────────────
    if (type->isa<FuncTypeAST>()) {
        auto* func = type->as<FuncTypeAST>();
        std::string result = "(";
        
        // Parameters
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) result += ", ";
            ParamAST* param = func->params[i];
            if (param->isVariadic) result += "...";
            if (param->isConst) result += "const ";
            result += typeToString(param->type, pool);
        }
        result += ")";
        
        // Only add "->" if hasArrow is true
        // This correctly handles both forms:
        //   - Form 1: (a int) -> int           → hasArrow = true  → "(a int) -> int"
        //   - Form 2: (a int)(b int) -> int     → first group hasArrow = false, second hasArrow = true
        //     → "(a int)(b int) -> int"  (the first group shows no arrow)
        //   - Void: (a int)                    → hasArrow = false → "(a int)"
        if (func->hasArrow) {
            result += " -> ";
            
            // Return types
            if (func->returnTypes.empty()) {
                result += "void";
            } else if (func->isCurried()) {
                // For curried functions, show the inner function type
                // The inner function's hasArrow determines its own arrow display
                result += typeToString(func->returnTypes[0], pool);
            } else if (func->returnTypes.size() == 1) {
                result += typeToString(func->returnTypes[0], pool);
            } else {
                // Multiple return values
                result += "(";
                for (size_t i = 0; i < func->returnTypes.size(); ++i) {
                    if (i > 0) result += ", ";
                    result += typeToString(func->returnTypes[i], pool);
                }
                result += ")";
            }
        }
        return result;
    }

    // ─── Unknown ────────────────────────────────────────────────────────────
    return kindToString(type->kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Decl to String - Includes field/param names
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Convert a TypeDeclAST to a human-readable string with full structure.
 * 
 * Shows struct/enum/trait definitions with their fields/variants.
 */
inline std::string typeDeclToString(TypeDeclAST* decl, const StringPool& pool) {
    if (!decl) return "<null>";

    if (decl->isa<StructDeclAST>()) {
        auto* structDecl = decl->as<StructDeclAST>();
        std::string result = "struct " + std::string(pool.lookup(structDecl->name));
        
        // Generic parameters
        if (!structDecl->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(structDecl->genericParams[i]->name));
            }
            result += ">";
        }
        
        // Trait implementations
        if (!structDecl->traitRefs.empty()) {
            result += " : ";
            for (size_t i = 0; i < structDecl->traitRefs.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(structDecl->traitRefs[i]->name));
            }
        }
        
        // Fields
        result += " { ";
        for (size_t i = 0; i < structDecl->fields.size(); ++i) {
            if (i > 0) result += ", ";
            FieldDeclAST* field = structDecl->fields[i];
            if (field->isConst) result += "const ";
            result += std::string(pool.lookup(field->name)) + " ";
            result += typeToString(field->type, pool);
        }
        result += " }";
        
        return result;
    }

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
            result += std::string(pool.lookup(variant->name)) + " = " + std::to_string(variant->value);
        }
        result += " }";
        return result;
    }

    if (decl->isa<TraitDeclAST>()) {
        auto* traitDecl = decl->as<TraitDeclAST>();
        std::string result = "trait " + std::string(pool.lookup(traitDecl->name));
        
        // Generic parameters
        if (!traitDecl->genericParams.empty()) {
            result += "<";
            for (size_t i = 0; i < traitDecl->genericParams.size(); ++i) {
                if (i > 0) result += ", ";
                result += std::string(pool.lookup(traitDecl->genericParams[i]->name));
            }
            result += ">";
        }
        
        // Fields
        result += " { ";
        for (size_t i = 0; i < traitDecl->fields.size(); ++i) {
            if (i > 0) result += ", ";
            TraitFieldDeclAST* field = traitDecl->fields[i];
            if (field->isConst) result += "const ";
            result += std::string(pool.lookup(field->name)) + " ";
            result += typeToString(field->type, pool);
        }
        result += " }";
        
        return result;
    }

    return "UnknownTypeDecl(" + kindToString(decl->kind) + ")";
}

// ─────────────────────────────────────────────────────────────────────────────
// InternedString to String
// ─────────────────────────────────────────────────────────────────────────────

inline std::string internedToString(const StringPool& pool, const InternedString& s) {
    if (!s.isValid()) return "";
    return std::string(pool.lookup(s));
}

} // namespace debug