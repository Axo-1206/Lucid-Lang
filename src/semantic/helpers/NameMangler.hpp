/**
 * @file NameMangler.hpp
 * @brief Name mangling for types, functions, and methods.
 * 
 * ============================================================================
 * DESIGN NOTES
 * ============================================================================
 * 
 * Name mangling produces canonical string keys for:
 *   - Type identities (for trait conformance lookup)
 *   - Function/method symbols (for code generation)
 *   - From conversion entries (for implicit conversion lookup)
 * 
 * Key changes from previous design:
 *   - Removed dependency on SymbolTable (now uses direct AST node access)
 *   - Type aliases are resolved eagerly during type resolution, so mangleType
 *     receives already-resolved types (no need for symbol table lookup)
 *   - Simplified function signatures (no SymbolTable parameter)
 * 
 * Mangled name format (simplified):
 *   - Primitive:     P{type}      e.g., Pint
 *   - Named type:    N{name}      e.g., NVec2
 *   - Nullable:      O{inner}     e.g., OPint
 *   - Result:        R{inner}E{error} or R{inner}N
 *   - Array slice:   A{element}   e.g., APint
 *   - Array dynamic: D{element}
 *   - Array fixed:   F{size}{element}
 *   - Reference:     Rf{inner}
 *   - Pointer:       Pp{inner}
 *   - Function:      Fn{qualifiers}({params}){returns}
 * 
 * @see TypeResolver for alias resolution
 */

#pragma once

#include "ast/support/StringPool.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include <string>
#include <string_view>

namespace NameMangler {

// ============================================================================
// Type Mangling (AST-node-only, no symbol table)
// ============================================================================

/**
 * @brief Converts a PrimitiveKind to its mangled string representation.
 */
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

/**
 * @brief Recursively mangles a type AST node.
 * 
 * IMPORTANT: This function assumes the type has already been resolved
 * (type aliases unwrapped). The caller is responsible for passing the
 * resolved type, not the original alias.
 * 
 * @param type The resolved type AST node
 * @param pool String pool for name lookups (for NamedTypeAST)
 * @return std::string Mangled type key
 */
inline std::string mangleType(const TypeAST* type, const StringPool& pool) {
    if (!type) return "void";
    
    switch (type->kind) {
        case ASTKind::PrimitiveType: {
            const auto* pt = static_cast<const PrimitiveTypeAST*>(type);
            return "P" + primitiveKindToString(pt->primitiveKind);
        }

        case ASTKind::NamedType: {
            const auto* nt = static_cast<const NamedTypeAST*>(type);
            std::string res = "N" + std::string(pool.lookup(nt->name));
            
            // Handle generic arguments
            if (!nt->genericArgs.empty()) {
                res += "<";
                for (size_t i = 0; i < nt->genericArgs.size(); ++i) {
                    if (i > 0) res += ",";
                    res += mangleType(nt->genericArgs[i], pool);
                }
                res += ">";
            }
            return res;
        }

        case ASTKind::NullableType: {
            const auto* nt = static_cast<const NullableTypeAST*>(type);
            return "O" + mangleType(nt->inner, pool);
        }

        case ASTKind::ResultType: {
            const auto* rt = static_cast<const ResultTypeAST*>(type);
            std::string res = "R" + mangleType(rt->inner, pool);
            if (rt->errorType) {
                res += "E" + mangleType(rt->errorType, pool);
            } else {
                res += "N";  // nil error (bare '!')
            }
            return res;
        }

        case ASTKind::ArrayType: {
            const auto* at = static_cast<const ArrayTypeAST*>(type);
            std::string prefix;
            switch (at->arrayKind) {
                case ArrayKind::Slice:   prefix = "A"; break;
                case ArrayKind::Dynamic: prefix = "D"; break;
                case ArrayKind::Fixed:   prefix = "F" + std::to_string(at->size); break;
                default:                 prefix = "U"; break;
            }
            return prefix + mangleType(at->element, pool);
        }

        case ASTKind::RefType: {
            const auto* rt = static_cast<const RefTypeAST*>(type);
            return "Rf" + mangleType(rt->inner, pool);
        }

        case ASTKind::PtrType: {
            const auto* pt = static_cast<const PtrTypeAST*>(type);
            return "Pp" + mangleType(pt->inner, pool);
        }

        case ASTKind::FuncType: {
            const auto* ft = static_cast<const FuncTypeAST*>(type);
            std::string res = "Fn";
            
            // Add qualifiers
            if (ft->isAsync()) res += "A";
            if (ft->isNullable()) res += "N";
            if (ft->isParallel()) res += "P";
            
            // Parameters (single group in new design)
            res += "(";
            for (const auto* param : ft->params) {
                if (param && param->type) {
                    res += mangleType(param->type, pool);
                } else {
                    res += "void";
                }
            }
            res += ")";
            
            // Return types
            if (ft->returnTypes.empty()) {
                res += "V";  // void
            } else if (ft->returnTypes.size() == 1) {
                res += mangleType(ft->returnTypes[0], pool);
            } else {
                res += "M";  // multiple returns
                for (const auto* ret : ft->returnTypes) {
                    res += mangleType(ret, pool);
                }
            }
            return res;
        }

        default:
            return "Unknown";
    }
}

// ============================================================================
// Function and Method Mangling
// ============================================================================

/**
 * @brief Mangles a function name with its parameter types.
 * 
 * @param funcName The function name
 * @param funcType The function type (for parameter mangling)
 * @param pool String pool for type mangling
 * @return std::string Mangled function symbol
 */
inline std::string mangleFunction(std::string_view funcName, const FuncTypeAST* funcType, const StringPool& pool) {
    std::string result;
    result.reserve(funcName.size() + 64);
    result.append(funcName);
    
    if (funcType) {
        result.append("_");
        for (const auto* param : funcType->params) {
            if (param && param->type) {
                result.append(mangleType(param->type, pool));
            }
        }
    }
    return result;
}

/**
 * @brief Mangles a method name with its parent type.
 * 
 * Format: ParentType::methodName
 * 
 * @param parentType The parent type (struct/enum/trait)
 * @param methodName The method name
 * @param pool String pool for type mangling
 * @return std::string Mangled method symbol
 */
inline std::string mangleMethod(const TypeAST* parentType, std::string_view methodName, const StringPool& pool) {
    std::string result;
    result.reserve(64);
    result.append(mangleType(parentType, pool));
    result.append("::");
    result.append(methodName);
    return result;
}

/**
 * @brief Mangles a method name with its parent type name (string version).
 * 
 * @param parentTypeName The parent type name
 * @param methodName The method name
 * @return std::string Mangled method symbol
 */
inline std::string mangleMethod(std::string_view parentTypeName, std::string_view methodName) {
    std::string result;
    result.reserve(parentTypeName.size() + methodName.size() + 2);
    result.append(parentTypeName);
    result.append("::");
    result.append(methodName);
    return result;
}

// ============================================================================
// Enum and From Mangling
// ============================================================================

/**
 * @brief Mangles an enum variant name.
 * 
 * Format: EnumName::variantName
 * 
 * @param enumName The enum type name
 * @param variant The variant name
 * @return std::string Mangled variant symbol
 */
inline std::string mangleEnumVariant(std::string_view enumName, std::string_view variant) {
    std::string result;
    result.reserve(enumName.size() + variant.size() + 2);
    result.append(enumName);
    result.append("::");
    result.append(variant);
    return result;
}

/**
 * @brief Mangles a from conversion entry.
 * 
 * Format: TargetType::from::SourceType
 * 
 * @param targetType The target type (what we convert to)
 * @param sourceType The source type parameter type
 * @param pool String pool for type mangling
 * @return std::string Mangled from entry key
 */
inline std::string mangleFrom(const TypeAST* targetType, const TypeAST* sourceType, const StringPool& pool) {
    std::string result;
    result.reserve(64);
    result.append(mangleType(targetType, pool));
    result.append("::from::");
    if (sourceType) {
        result.append(mangleType(sourceType, pool));
    } else {
        result.append("void");
    }
    return result;
}

/**
 * @brief Gets the prefix for from lookup queries.
 * 
 * @param targetType The target type
 * @param pool String pool for type mangling
 * @return std::string Prefix string (e.g., "Pint::from::")
 */
inline std::string getFromPrefix(const TypeAST* targetType, const StringPool& pool) {
    return mangleType(targetType, pool) + "::from::";
}

// ============================================================================
// Trait Conformance Key
// ============================================================================

/**
 * @brief Mangles a type for trait conformance lookup.
 * 
 * This is an alias for mangleType – types are the canonical keys for
 * trait conformance mapping.
 * 
 * @param type The resolved type
 * @param pool String pool for type mangling
 * @return std::string Mangled type key
 */
inline std::string mangleTraitKey(const TypeAST* type, const StringPool& pool) {
    return mangleType(type, pool);
}

} // namespace NameMangler