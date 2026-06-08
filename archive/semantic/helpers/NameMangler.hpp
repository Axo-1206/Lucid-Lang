/**
 * @file NameMangler.hpp
 * @brief Name mangling for symbols, types, and methods.
 */

#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/SymbolTable.hpp"
#include <string>
#include <string_view>

namespace NameMangler {

    // Forward declaration with const-correctness
    inline std::string mangleType(const TypeAST* type, const StringPool& pool, const SymbolTable* symbols);

    // Helper to unwrap type aliases (const version)
    inline const TypeAST* unwrapAliases(const TypeAST* type, const SymbolTable* symbols) {
        if (!type || !symbols) return type;

        while (type && type->isa<NamedTypeAST>()) {
            const auto* named = static_cast<const NamedTypeAST*>(type);
            const Symbol* sym = symbols->lookup(named->name);
            if (!sym || sym->kind != SymbolKind::TypeAlias) break;
            if (!sym->type) break;
            type = sym->type;
        }
        return type;
    }

    // Get primitive kind string
    inline std::string primitiveKindToString(PrimitiveKind kind) {
        switch (kind) {
            case PrimitiveKind::Bool:     return "bool";
            case PrimitiveKind::Byte:     return "byte";
            case PrimitiveKind::Short:    return "short";
            case PrimitiveKind::Int:      return "int";
            case PrimitiveKind::Long:     return "long";
            case PrimitiveKind::Ubyte:    return "ubyte";
            case PrimitiveKind::Ushort:   return "ushort";
            case PrimitiveKind::Uint:     return "uint";
            case PrimitiveKind::Ulong:    return "ulong";
            case PrimitiveKind::Int8:     return "int8";
            case PrimitiveKind::Int16:    return "int16";
            case PrimitiveKind::Int32:    return "int32";
            case PrimitiveKind::Int64:    return "int64";
            case PrimitiveKind::Uint8:    return "uint8";
            case PrimitiveKind::Uint16:   return "uint16";
            case PrimitiveKind::Uint32:   return "uint32";
            case PrimitiveKind::Uint64:   return "uint64";
            case PrimitiveKind::Float:    return "float";
            case PrimitiveKind::Double:   return "double";
            case PrimitiveKind::Decimal:  return "decimal";
            case PrimitiveKind::String:   return "string";
            case PrimitiveKind::Char:     return "char";
            case PrimitiveKind::Any:      return "any";
            default:                      return "unknown";
        }
    }

    // Main type mangling function (const-correct)
    inline std::string mangleType(const TypeAST* type, const StringPool& pool, const SymbolTable* symbols) {
        if (!type) return "unknown";

        const TypeAST* underlying = type;
        if (symbols) {
            underlying = unwrapAliases(type, symbols);
        }

        switch (underlying->kind) {
            case ASTKind::PrimitiveType: {
                const auto* pt = static_cast<const PrimitiveTypeAST*>(underlying);
                return "P" + primitiveKindToString(pt->primitiveKind);
            }

            case ASTKind::NamedType: {
                const auto* nt = static_cast<const NamedTypeAST*>(underlying);
                std::string res = "N" + std::string(pool.lookup(nt->name));
                if (!nt->genericArgs.empty()) {
                    res += "<";
                    for (size_t i = 0; i < nt->genericArgs.size(); ++i) {
                        if (i > 0) res += ",";
                        res += mangleType(nt->genericArgs[i].get(), pool, symbols);
                    }
                    res += ">";
                }
                return res;
            }

            case ASTKind::NullableType: {
                const auto* nt = static_cast<const NullableTypeAST*>(underlying);
                return "O" + mangleType(nt->inner.get(), pool, symbols);
            }

            case ASTKind::ResultType: {
                const auto* rt = static_cast<const ResultTypeAST*>(underlying);
                std::string res = "R" + mangleType(rt->inner.get(), pool, symbols);
                if (rt->errorType) {
                    res += "E" + mangleType(rt->errorType.get(), pool, symbols);
                } else {
                    res += "N";  // nil error
                }
                return res;
            }

            case ASTKind::ArrayType: {
                const auto* at = static_cast<const ArrayTypeAST*>(underlying);
                std::string prefix;
                switch (at->arrayKind) {
                    case ArrayKind::Slice:   prefix = "A"; break;
                    case ArrayKind::Dynamic: prefix = "D"; break;
                    case ArrayKind::Fixed:   prefix = "F" + std::to_string(at->size); break;
                }
                return prefix + mangleType(at->element.get(), pool, symbols);
            }

            case ASTKind::RefType: {
                const auto* rt = static_cast<const RefTypeAST*>(underlying);
                return "Rf" + mangleType(rt->inner.get(), pool, symbols);
            }

            case ASTKind::PtrType: {
                const auto* pt = static_cast<const PtrTypeAST*>(underlying);
                return "Pp" + mangleType(pt->inner.get(), pool, symbols);
            }

            case ASTKind::FuncType: {
                const auto* ft = static_cast<const FuncTypeAST*>(underlying);
                std::string res = "Fn";
                
                // Add qualifiers
                if (ft->isAsync()) res += "A";
                if (ft->isNullable()) res += "N";
                if (ft->isParallel()) res += "P";
                
                res += "(";
                // Parameters - flatten all groups
                for (const auto& param : ft->sig.allParams) {
                    if (param && param->type) {
                        res += mangleType(param->type.get(), pool, symbols);
                    } else {
                        res += "void";
                    }
                }
                res += ")";
                
                // Return types
                if (ft->sig.returnTypes.empty()) {
                    res += "V";
                } else if (ft->sig.returnTypes.size() == 1) {
                    res += mangleType(ft->sig.returnTypes[0].get(), pool, symbols);
                } else {
                    res += "M";
                    for (const auto& ret : ft->sig.returnTypes) {
                        res += mangleType(ret.get(), pool, symbols);
                    }
                }
                return res;
            }

            default:
                return "Unknown";
        }
    }

    // Overload without symbol table (for contexts where aliases are already resolved)
    inline std::string mangleType(const TypeAST* type, const StringPool& pool) {
        return mangleType(type, pool, nullptr);
    }

    // Method mangling: Type::method
    inline std::string mangleMethod(std::string_view parent, std::string_view method) {
        std::string result;
        result.reserve(parent.size() + method.size() + 2);
        result.append(parent);
        result.append("::");
        result.append(method);
        return result;
    }

    // Enum variant mangling: EnumName::variantName
    inline std::string mangleEnumVariant(std::string_view enumName, std::string_view variant) {
        std::string result;
        result.reserve(enumName.size() + variant.size() + 2);
        result.append(enumName);
        result.append("::");
        result.append(variant);
        return result;
    }

    // From entry mangling: TargetType::from::SourceType
    inline std::string mangleFrom(std::string_view target, const TypeAST* paramType, const StringPool& pool) {
        std::string result;
        result.reserve(target.size() + 16);
        result.append(target);
        result.append("::from::");
        if (paramType) {
            result.append(mangleType(paramType, pool));
        } else {
            result.append("void");
        }
        return result;
    }

    // Prefix for from lookup
    inline std::string getFromPrefix(std::string_view target) {
        return std::string(target) + "::from::";
    }

} // namespace NameMangler