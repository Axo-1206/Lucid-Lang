/**
 * @file ResolverHelpers.hpp
 * @brief Static helper functions for type resolution.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include <string>

namespace ResolverHelpers {

    inline bool isPrimitiveType(TypeAST* type) {
        return type && type->isa<PrimitiveTypeAST>();
    }

    inline bool isStructType(TypeAST* type, const SemanticContext& ctx) {
        if (!type || !type->isa<NamedTypeAST>()) return false;
        auto* named = type->as<NamedTypeAST>();
        Symbol* sym = ctx.symbols->lookup(named->name);
        return sym && sym->kind == SymbolKind::Struct;
    }

    inline bool isEnumType(TypeAST* type, const SemanticContext& ctx) {
        if (!type || !type->isa<NamedTypeAST>()) return false;
        auto* named = type->as<NamedTypeAST>();
        Symbol* sym = ctx.symbols->lookup(named->name);
        return sym && sym->kind == SymbolKind::Enum;
    }

    inline bool isFunctionType(TypeAST* type) {
        return type && type->isa<FuncTypeAST>();
    }

    inline bool isReferenceType(TypeAST* type) {
        return type && type->isa<RefTypeAST>();
    }

    inline bool isPointerType(TypeAST* type) {
        return type && type->isa<PtrTypeAST>();
    }

    inline bool isArrayType(TypeAST* type) {
        return type && type->isa<ArrayTypeAST>();
    }

    inline std::string getTypeName(TypeAST* type, const SemanticContext& ctx) {
        if (!type) return "";
        if (type->isa<PrimitiveTypeAST>()) {
            auto pt = type->as<PrimitiveTypeAST>();
            switch (pt->primitiveKind) {
                case PrimitiveKind::Int:     return "int";
                case PrimitiveKind::Float:   return "float";
                case PrimitiveKind::Double:  return "double";
                case PrimitiveKind::String:  return "string";
                case PrimitiveKind::Bool:    return "bool";
                case PrimitiveKind::Char:    return "char";
                case PrimitiveKind::Byte:    return "byte";
                case PrimitiveKind::Short:   return "short";
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
                case PrimitiveKind::Decimal: return "decimal";
                case PrimitiveKind::Any:     return "any";
                default:                     return "primitive";
            }
        }
        if (type->isa<NamedTypeAST>()) {
            return std::string(ctx.pool.lookup(type->as<NamedTypeAST>()->name));
        }
        return "";
    }

    inline bool hasGenericParams(TypeAST* type) {
        if (type && type->isa<NamedTypeAST>()) {
            return !type->as<NamedTypeAST>()->genericArgs.empty();
        }
        return false;
    }

    inline size_t getGenericArity(TypeAST* type) {
        if (type && type->isa<NamedTypeAST>()) {
            return type->as<NamedTypeAST>()->genericArgs.size();
        }
        return 0;
    }

} // namespace ResolverHelpers