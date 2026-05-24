#pragma once

#include "ast/BaseAST.hpp"
#include "ast/TypeAST.hpp"

#include "Tokens.hpp"
#include <string>

namespace LucDebug {
    // Convert ASTKind enum to human-readable string
    inline std::string kindToString(ASTKind kind) {
        switch (kind) {
            // Unknown / Recovery nodes
            case ASTKind::Unknown:           return "Unknown";
            case ASTKind::UnknownDecl:       return "UnknownDecl";
            case ASTKind::UnknownExpr:       return "UnknownExpr";
            case ASTKind::UnknownStmt:       return "UnknownStmt";
            case ASTKind::UnknownType:       return "UnknownType";

            // Type nodes
            case ASTKind::PrimitiveType:     return "PrimitiveType";
            case ASTKind::NamedType:         return "NamedType";
            case ASTKind::NullableType:      return "NullableType";
            case ASTKind::ResultType:        return "ResultType";
            case ASTKind::FixedArrayType:    return "FixedArrayType";
            case ASTKind::SliceType:         return "SliceType";
            case ASTKind::DynamicArrayType:  return "DynamicArrayType";
            case ASTKind::RefType:           return "RefType";
            case ASTKind::PtrType:           return "PtrType";
            case ASTKind::FuncType:          return "FuncType";

            // Declaration nodes
            case ASTKind::PackageDecl:       return "PackageDecl";
            case ASTKind::UseDecl:           return "UseDecl";
            case ASTKind::VarDecl:           return "VarDecl";
            case ASTKind::Param:             return "Param";
            case ASTKind::GenericParam:      return "GenericParam";
            case ASTKind::FuncDecl:          return "FuncDecl";
            case ASTKind::FieldDecl:         return "FieldDecl";
            case ASTKind::StructDecl:        return "StructDecl";
            case ASTKind::EnumVariant:       return "EnumVariant";
            case ASTKind::EnumDecl:          return "EnumDecl";
            case ASTKind::TraitMethod:       return "TraitMethod";
            case ASTKind::TraitDecl:         return "TraitDecl";
            case ASTKind::TraitRef:          return "TraitRef";
            case ASTKind::MethodDecl:        return "MethodDecl";
            case ASTKind::FromDecl:          return "FromDecl";
            case ASTKind::FromEntry:         return "FromEntry";
            case ASTKind::ImplDecl:          return "ImplDecl";
            case ASTKind::TypeAliasDecl:     return "TypeAliasDecl";

            // Expression nodes
            case ASTKind::LiteralExpr:        return "LiteralExpr";
            case ASTKind::ArrayLiteralExpr:   return "ArrayLiteralExpr";
            case ASTKind::StructLiteralExpr:  return "StructLiteralExpr";
            case ASTKind::FieldInit:          return "FieldInit";
            case ASTKind::IdentifierExpr:     return "IdentifierExpr";
            case ASTKind::FieldAccessExpr:    return "FieldAccessExpr";
            case ASTKind::BehaviorAccessExpr: return "BehaviorAccessExpr";
            case ASTKind::CallExpr:           return "CallExpr";
            case ASTKind::IndexExpr:          return "IndexExpr";
            case ASTKind::BinaryExpr:         return "BinaryExpr";
            case ASTKind::UnaryExpr:          return "UnaryExpr";
            case ASTKind::AssignExpr:         return "AssignExpr";
            case ASTKind::IsExpr:             return "IsExpr";
            case ASTKind::NullableChainExpr:  return "NullableChainExpr";
            case ASTKind::NullCoalesceExpr:   return "NullCoalesceExpr";
            case ASTKind::PipelineExpr:       return "PipelineExpr";
            case ASTKind::PipelineStep:       return "PipelineStep";
            case ASTKind::ComposeExpr:        return "ComposeExpr";
            case ASTKind::ComposeOperand:     return "ComposeOperand";
            case ASTKind::AnonFuncExpr:       return "AnonFuncExpr";
            case ASTKind::AwaitExpr:          return "AwaitExpr";
            case ASTKind::MatchExpr:          return "MatchExpr";
            case ASTKind::IfExpr:             return "IfExpr";
            case ASTKind::RangeExpr:          return "RangeExpr";
            case ASTKind::TypeConvExpr:       return "TypeConvExpr";
            case ASTKind::ResolveExpr:        return "ResolveExpr";
            case ASTKind::OkArm:              return "OkArm";
            case ASTKind::ErrArm:             return "ErrArm";

            // Statement nodes
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

            // Pattern nodes
            case ASTKind::BindPattern:        return "BindPattern";
            case ASTKind::WildcardPattern:    return "WildcardPattern";
            case ASTKind::TypePattern:        return "TypePattern";
            case ASTKind::StructPattern:      return "StructPattern";
            case ASTKind::FieldPattern:       return "FieldPattern";
            case ASTKind::PatternExpr:        return "PatternExpr";
            case ASTKind::MatchArm:           return "MatchArm";
            case ASTKind::DefaultArm:         return "DefaultArm";

            // Root
            case ASTKind::Program:            return "Program";

            // Compiler directives
            case ASTKind::Attribute:          return "Attribute";
            case ASTKind::AttributeArg:       return "AttributeArg";
            case ASTKind::IntrinsicCallExpr:  return "IntrinsicCallExpr";

            default:                          return "Unknown";
        }
    }

    // Helper to print token type (for parser debug)
    inline std::string tokenTypeToString(TokenType type) {
        switch (type) {
            case TokenType::MUL:        return "MUL (*)";
            case TokenType::QUESTION:   return "QUESTION (?)";
            case TokenType::IDENTIFIER: return "IDENTIFIER";
            case TokenType::TYPE_UINT8: return "TYPE_UINT8";
            case TokenType::LPAREN:     return "LPAREN ((";
            case TokenType::RPAREN:     return "RPAREN )";
            case TokenType::ASSIGN:     return "ASSIGN (=)";
            default:                    return "Token(" + std::to_string(static_cast<int>(type)) + ")";
        }
    }

    // Helper: format primitive kind
    inline std::string primitiveKindToString(PrimitiveKind k) {
        switch (k) {
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
            case PrimitiveKind::Any:     return "any";
            default:                     return "primitive";
        }
    }

    // Helper: convert InternedString to display string
    inline std::string toStr(const StringPool* pool, const InternedString& s) {
        if (!s.isValid()) return "";
        return std::string(pool->lookup(s));
    }
}