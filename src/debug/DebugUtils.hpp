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
            case ASTKind::ArrayType:         return "ArrayType";
            case ASTKind::GenericArrayType:  return "GenericArrayType";
            case ASTKind::RefType:           return "RefType";
            case ASTKind::PtrType:           return "PtrType";
            case ASTKind::FuncType:          return "FuncType";

            // Declaration nodes
            case ASTKind::PackageDecl:       return "PackageDecl";
            case ASTKind::UseDecl:           return "UseDecl";
            case ASTKind::VarDecl:           return "VarDecl";
            case ASTKind::Param:             return "Param";
            case ASTKind::GenericParamDecl:  return "GenericParamDecl";
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
            case ASTKind::SliceExpr:          return "SliceExpr";
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

    // Helper to print token type (for parser debug)
    inline std::string tokenToString(const Token& token) {
        switch (token.type) {
            case TokenType::IDENTIFIER:     return "IDENTIFIER('" + token.value + "')";
            case TokenType::INT_LITERAL:    return "INT_LITERAL(" + token.value + ")";
            case TokenType::FLOAT_LITERAL:  return "FLOAT_LITERAL(" + token.value + ")";
            case TokenType::STRING_LITERAL: return "STRING_LITERAL(\"" + token.value + "\")";
            case TokenType::CHAR_LITERAL:   return "CHAR_LITERAL('" + token.value + "')";
            case TokenType::HEX_LITERAL:    return "HEX_LITERAL(" + token.value + ")";
            case TokenType::BINARY_LITERAL: return "BINARY_LITERAL(" + token.value + ")";
            case TokenType::RAW_STRING_LITERAL: return "RAW_STRING_LITERAL(\"\"\"" + token.value + "\"\"\")";
            case TokenType::LINE_COMMENT:   return "LINE_COMMENT(--" + token.value + ")";
            case TokenType::DOC_COMMENT:    return "DOC_COMMENT(/--" + token.value + "--/)";
            case TokenType::EOF_TOKEN:      return "EOF";
            case TokenType::UNKNOWN:        return "UNKNOWN('" + token.value + "')";
            
            // Keywords
            case TokenType::PUB:            return "PUB(pub)";
            case TokenType::EXPORT:         return "EXPORT(export)";
            case TokenType::PACKAGE:        return "PACKAGE(package)";
            case TokenType::USE:            return "USE(use)";
            case TokenType::AS:             return "AS(as)";
            case TokenType::IMPL:           return "IMPL(impl)";
            case TokenType::TYPE:           return "TYPE(type)";
            case TokenType::STRUCT:         return "STRUCT(struct)";
            case TokenType::ENUM:           return "ENUM(enum)";
            case TokenType::TRAIT:          return "TRAIT(trait)";
            case TokenType::FROM:           return "FROM(from)";
            case TokenType::LET:            return "LET(let)";
            case TokenType::CONST:          return "CONST(const)";
            case TokenType::AWAIT:          return "AWAIT(await)";
            case TokenType::IF:             return "IF(if)";
            case TokenType::ELSE:           return "ELSE(else)";
            case TokenType::MATCH:          return "MATCH(match)";
            case TokenType::SWITCH:         return "SWITCH(switch)";
            case TokenType::CASE:           return "CASE(case)";
            case TokenType::DEFAULT:        return "DEFAULT(default)";
            case TokenType::IS:             return "IS(is)";
            case TokenType::WHILE:          return "WHILE(while)";
            case TokenType::FOR:            return "FOR(for)";
            case TokenType::IN:             return "IN(in)";
            case TokenType::DO:             return "DO(do)";
            case TokenType::RETURN:         return "RETURN(return)";
            case TokenType::BREAK:          return "BREAK(break)";
            case TokenType::CONTINUE:       return "CONTINUE(continue)";
            case TokenType::AND:            return "AND(and)";
            case TokenType::OR:             return "OR(or)";
            case TokenType::NOT:            return "NOT(not)";
            case TokenType::TRUE:           return "TRUE(true)";
            case TokenType::FALSE:          return "FALSE(false)";
            case TokenType::NIL:            return "NIL(nil)";
            case TokenType::RESOLVE:        return "RESOLVE(resolve)";
            case TokenType::OK:             return "OK(ok)";
            case TokenType::ERR:            return "ERR(err)";
            
            // Type keywords
            case TokenType::TYPE_BOOL:      return "TYPE_BOOL(bool)";
            case TokenType::TYPE_BYTE:      return "TYPE_BYTE(byte)";
            case TokenType::TYPE_SHORT:     return "TYPE_SHORT(short)";
            case TokenType::TYPE_INT:       return "TYPE_INT(int)";
            case TokenType::TYPE_LONG:      return "TYPE_LONG(long)";
            case TokenType::TYPE_UBYTE:     return "TYPE_UBYTE(ubyte)";
            case TokenType::TYPE_USHORT:    return "TYPE_USHORT(ushort)";
            case TokenType::TYPE_UINT:      return "TYPE_UINT(uint)";
            case TokenType::TYPE_ULONG:     return "TYPE_ULONG(ulong)";
            case TokenType::TYPE_INT8:      return "TYPE_INT8(int8)";
            case TokenType::TYPE_INT16:     return "TYPE_INT16(int16)";
            case TokenType::TYPE_INT32:     return "TYPE_INT32(int32)";
            case TokenType::TYPE_INT64:     return "TYPE_INT64(int64)";
            case TokenType::TYPE_UINT8:     return "TYPE_UINT8(uint8)";
            case TokenType::TYPE_UINT16:    return "TYPE_UINT16(uint16)";
            case TokenType::TYPE_UINT32:    return "TYPE_UINT32(uint32)";
            case TokenType::TYPE_UINT64:    return "TYPE_UINT64(uint64)";
            case TokenType::TYPE_FLOAT:     return "TYPE_FLOAT(float)";
            case TokenType::TYPE_DOUBLE:    return "TYPE_DOUBLE(double)";
            case TokenType::TYPE_DECIMAL:   return "TYPE_DECIMAL(decimal)";
            case TokenType::TYPE_STRING:    return "TYPE_STRING(string)";
            case TokenType::TYPE_CHAR:      return "TYPE_CHAR(char)";
            case TokenType::TYPE_ANY:       return "TYPE_ANY(any)";
            
            // Single character tokens
            case TokenType::DOT:            return "DOT(.)";
            case TokenType::COLON:          return "COLON(:)";
            case TokenType::COMMA:          return "COMMA(,)";
            case TokenType::SEMICOLON:      return "SEMICOLON(;)";
            case TokenType::LPAREN:         return "LPAREN(()";
            case TokenType::RPAREN:         return "RPAREN())";
            case TokenType::LBRACE:         return "LBRACE({)";
            case TokenType::RBRACE:         return "RBRACE(})";
            case TokenType::LBRACKET:       return "LBRACKET([)";
            case TokenType::RBRACKET:       return "RBRACKET(])";
            case TokenType::PLUS:           return "PLUS(+)";
            case TokenType::MINUS:          return "MINUS(-)";
            case TokenType::MUL:            return "MUL(*)";
            case TokenType::DIV:            return "DIV(/)";
            case TokenType::MOD:            return "MOD(%)";
            case TokenType::POW:            return "POW(^)";
            case TokenType::ASSIGN:         return "ASSIGN(=)";
            case TokenType::BANG:           return "BANG(!)";
            case TokenType::QUESTION:       return "QUESTION(?)";
            case TokenType::TILDE:          return "TILDE(~)";
            case TokenType::AT_SIGN:        return "AT_SIGN(@)";
            case TokenType::HASH:           return "HASH(#)";
            case TokenType::AMPERSAND:      return "AMPERSAND(&)";
            case TokenType::PIPE:           return "PIPE(|)";
            case TokenType::WILDCARD:       return "WILDCARD(_)";
            
            // Two+ character tokens
            case TokenType::RANGE:          return "RANGE(..)";
            case TokenType::VARIADIC:       return "VARIADIC(...)";
            case TokenType::QUESTION_DOT:   return "QUESTION_DOT(?.)";
            case TokenType::QUESTION_QUESTION: return "QUESTION_QUESTION(?\?)";
            case TokenType::EQUAL_EQUAL:    return "EQUAL_EQUAL(==)";
            case TokenType::EQUAL_EQUAL_EQUAL: return "EQUAL_EQUAL_EQUAL(===)";
            case TokenType::NOT_EQUAL:      return "NOT_EQUAL(!=)";
            case TokenType::LESS_EQUAL:     return "LESS_EQUAL(<=)";
            case TokenType::GREATER_EQUAL:  return "GREATER_EQUAL(>=)";
            case TokenType::SHL:            return "SHL(<<)";
            case TokenType::SHR:            return "SHR(>>)";
            case TokenType::ARROW:          return "ARROW(->)";
            case TokenType::FAT_ARROW:      return "FAT_ARROW(=>)";
            case TokenType::COMPOSE:        return "COMPOSE(+>)";
            case TokenType::PIPELINE:       return "PIPELINE(|>)";
            case TokenType::BIT_AND:        return "BIT_AND(&&)";
            case TokenType::BIT_OR:         return "BIT_OR(||)";
            case TokenType::BIT_NOT:        return "BIT_NOT(~~)";
            case TokenType::BIT_XOR:        return "BIT_XOR(~^)";
            case TokenType::PLUS_ASSIGN:    return "PLUS_ASSIGN(+=)";
            case TokenType::MINUS_ASSIGN:   return "MINUS_ASSIGN(-=)";
            case TokenType::MUL_ASSIGN:     return "MUL_ASSIGN(*=)";
            case TokenType::DIV_ASSIGN:     return "DIV_ASSIGN(/=)";
            case TokenType::MOD_ASSIGN:     return "MOD_ASSIGN(%=)";
            case TokenType::POW_ASSIGN:     return "POW_ASSIGN(^=)";
            case TokenType::SHL_ASSIGN:     return "SHL_ASSIGN(<<=)";
            case TokenType::SHR_ASSIGN:     return "SHR_ASSIGN(>>=)";
            
            default: return "UNKNOWN_TOKEN(" + std::to_string(static_cast<int>(token.type)) + ")";
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

    // Helper to convert a TypeAST to a readable string for diagnostics
    inline std::string formatType(TypeAST* type, const StringPool& pool) {
        if (!type) return "<null>";
        
        if (type->isa<PrimitiveTypeAST>()) {
            auto* prim = type->as<PrimitiveTypeAST>();
            return primitiveKindToString(prim->primitiveKind);
        }
        
        if (type->isa<NamedTypeAST>()) {
            auto* named = type->as<NamedTypeAST>();
            std::string result = std::string(pool.lookup(named->name));
            if (!named->genericArgs.empty()) {
                result += "<";
                for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                    if (i > 0) result += ", ";
                    result += formatType(named->genericArgs[i], pool);
                }
                result += ">";
            }
            return result;
        }
        
        return LucDebug::kindToString(type->kind);
    }
}