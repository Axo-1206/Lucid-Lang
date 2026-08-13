/// @file codegen/support/MangledName.cpp
/// @brief Implementation of mangled name generation.

#include "MangledName.hpp"
#include "debug/DebugUtils.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace codegen {

// ─── Private Helper: Build Mangled String ──────────────────────────────────

/// @brief Build a mangled name string from components.
/// @param components The components to join.
/// @param ctx The code generation context.
/// @return The full mangled name as an InternedString.
static InternedString buildMangledName(const std::string& components, CodeGenContext& ctx) {
    std::string result = "_L";
    result += components;
    return ctx.pool.intern(result);
}

// ─── Public API ─────────────────────────────────────────────────────────────

InternedString generateMangledName(const FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return InternedString(0);
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Function name ──────────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(decl->name));
    
    // ─── 3. Generic parameters (if any) ──────────────────────────────────
    // Note: For the generic declaration itself, we encode the parameter NAMES
    // (e.g., "T"), not concrete types. Concrete types are encoded in the
    // generateMangledNameForGeneric function.
    if (!decl->genericParams.empty()) {
        result += "_G";
        for (size_t i = 0; i < decl->genericParams.size(); ++i) {
            if (i > 0) result += "_";
            result += sanitizeForMangledName(
                ctx.pool.lookup(decl->genericParams[i]->name)
            );
        }
    }
    
    // ─── 4. Parameter types ──────────────────────────────────────────────
    result += "_P";
    const FuncTypeAST* funcType = decl->funcType;
    while (funcType) {
        for (const ParamAST* param : funcType->params) {
            result += typeToMangleString(param->type, ctx);
        }
        funcType = funcType->getNext();
    }
    
    // ─── 5. Return type ──────────────────────────────────────────────────
    if (decl->funcType->returnType) {
        result += "_R" + typeToMangleString(decl->funcType->returnType, ctx);
    } else {
        result += "_RV";  // void
    }
    
    return buildMangledName(result, ctx);
}

InternedString generateMangledName(const VarDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return InternedString(0);
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Variable name ──────────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(decl->name));
    
    // ─── 3. Type ──────────────────────────────────────────────────────────
    if (decl->type) {
        result += "_T" + typeToMangleString(decl->type, ctx);
    } else {
        result += "_TV";  // void (should not happen for variables)
    }
    
    // ─── 4. Mutability ──────────────────────────────────────────────────
    result += decl->isConst() ? "_C" : "_M";  // Const or Mutable
    
    return buildMangledName(result, ctx);
}

InternedString generateMangledNameForGeneric(
    const DeclAST* baseDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!baseDecl || typeArgs.empty()) {
        return InternedString(0);
    }
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Declaration name ──────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(baseDecl->name));
    
    // ─── 3. Generic arguments (concrete types) ──────────────────────────
    result += "_G";
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) result += "_";
        result += typeToMangleString(typeArgs[i], ctx);
    }
    
    // ─── 4. For functions, also encode parameter and return types ──────
    if (const FuncDeclAST* funcDecl = baseDecl->as<FuncDeclAST>()) {
        // Parameter types (using substituted types)
        result += "_P";
        const FuncTypeAST* funcType = funcDecl->funcType;
        while (funcType) {
            for (const ParamAST* param : funcType->params) {
                // If this is a generic parameter, substitute it
                const TypeAST* paramType = param->type;
                // Check if param type is a generic parameter
                if (paramType->isa<NamedTypeAST>()) {
                    const NamedTypeAST* named = paramType->as<NamedTypeAST>();
                    // Find if this matches a generic param name
                    for (size_t j = 0; j < funcDecl->genericParams.size(); ++j) {
                        if (funcDecl->genericParams[j]->name == named->name) {
                            if (j < typeArgs.size()) {
                                paramType = typeArgs[j];
                            }
                            break;
                        }
                    }
                }
                result += typeToMangleString(paramType, ctx);
            }
            funcType = funcType->getNext();
        }
        
        // Return type (using substituted type)
        if (funcDecl->funcType->returnType) {
            const TypeAST* returnType = funcDecl->funcType->returnType;
            // Check if return type is a generic parameter
            if (returnType->isa<NamedTypeAST>()) {
                const NamedTypeAST* named = returnType->as<NamedTypeAST>();
                for (size_t j = 0; j < funcDecl->genericParams.size(); ++j) {
                    if (funcDecl->genericParams[j]->name == named->name) {
                        if (j < typeArgs.size()) {
                            returnType = typeArgs[j];
                        }
                        break;
                    }
                }
            }
            result += "_R" + typeToMangleString(returnType, ctx);
        } else {
            result += "_RV";
        }
    }
    
    // ─── 5. For structs, encode field types ──────────────────────────────
    if (const StructDeclAST* structDecl = baseDecl->as<StructDeclAST>()) {
        result += "_F";
        for (const FieldDeclAST* field : structDecl->fields) {
            const TypeAST* fieldType = field->type;
            // Check if field type is a generic parameter
            if (fieldType->isa<NamedTypeAST>()) {
                const NamedTypeAST* named = fieldType->as<NamedTypeAST>();
                for (size_t j = 0; j < structDecl->genericParams.size(); ++j) {
                    if (structDecl->genericParams[j]->name == named->name) {
                        if (j < typeArgs.size()) {
                            fieldType = typeArgs[j];
                        }
                        break;
                    }
                }
            }
            result += typeToMangleString(fieldType, ctx);
        }
    }
    
    return buildMangledName(result, ctx);
}

InternedString generateMangledName(const StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return InternedString(0);
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Struct name ──────────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(decl->name));
    
    // ─── 3. Generic parameters (if any) ──────────────────────────────────
    if (!decl->genericParams.empty()) {
        result += "_G";
        for (size_t i = 0; i < decl->genericParams.size(); ++i) {
            if (i > 0) result += "_";
            result += sanitizeForMangledName(
                ctx.pool.lookup(decl->genericParams[i]->name)
            );
        }
    }
    
    // ─── 4. Field types ──────────────────────────────────────────────────
    if (!decl->fields.empty()) {
        result += "_F";
        for (const FieldDeclAST* field : decl->fields) {
            result += typeToMangleString(field->type, ctx);
        }
    }
    
    return buildMangledName(result, ctx);
}

// ─── Core Encoding Functions ──────────────────────────────────────────────

std::string typeToMangleString(const TypeAST* type, StringPool& pool) {
    if (!type) return "V";  // void
    
    switch (type->kind) {
        case ASTKind::PrimitiveType: {
            const PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
            char code = encodePrimitiveKind(prim->primitiveKind);
            return std::string(1, code);
        }
        
        case ASTKind::NamedType: {
            const NamedTypeAST* named = type->as<NamedTypeAST>();
            std::string name = sanitizeForMangledName(
                pool.lookup(named->name)
            );
            
            // Add generic arguments if present
            if (!named->genericArgs.empty()) {
                name += "_G";
                for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                    if (i > 0) name += "_";
                    name += typeToMangleString(named->genericArgs[i], pool);
                }
            }
            return name;
        }
        
        case ASTKind::ArrayType: {
            const ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            std::string result = "A";
            if (arr->isFixed()) {
                result += std::to_string(arr->size);
            } else if (arr->isSlice()) {
                result += "_";
            } else {
                result += "*";
            }
            result += typeToMangleString(arr->element, pool);
            return result;
        }
        
        case ASTKind::PtrType: {
            const PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return "P" + typeToMangleString(ptr->inner, pool);
        }
        
        case ASTKind::RefType: {
            const RefTypeAST* ref = type->as<RefTypeAST>();
            return "R" + typeToMangleString(ref->inner, pool);
        }
        
        case ASTKind::NullableType: {
            const NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return "N" + typeToMangleString(nullable->inner, pool);
        }
        
        case ASTKind::FallibleType: {
            const FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return "F" + typeToMangleString(fallible->inner, pool);
        }
        
        case ASTKind::CombinedType: {
            const CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return "X" + typeToMangleString(combined->inner, pool);
        }
        
        case ASTKind::FuncType: {
            const FuncTypeAST* func = type->as<FuncTypeAST>();
            std::string result = "F";
            
            // Parameter types
            for (const ParamAST* param : func->params) {
                result += typeToMangleString(param->type, pool);
            }
            result += "_";
            
            // Return type
            if (func->returnType) {
                result += typeToMangleString(func->returnType, pool);
            } else {
                result += "V";
            }
            return result;
        }
        
        case ASTKind::FutureType: {
            const FutureTypeAST* future = type->as<FutureTypeAST>();
            return "U" + typeToMangleString(future->inner, pool);
        }
        
        case ASTKind::ThreadType: {
            const ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return "H" + typeToMangleString(thread->inner, pool);
        }
        
        default:
            return "?" + debug::kindToString(type->kind);
    }
}

std::string sanitizeForMangledName(const std::string& str) {
    std::string result = str;
    
    // Replace special characters with underscores
    for (char& c : result) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    
    return result;
}

std::string getMangledModulePath(CodeGenContext& ctx) {
    if (!ctx.module) {
        return "global";
    }
    
    // Get the module name (which is the file path)
    std::string path = ctx.module->getName().str();
    
    // Replace path separators and dots with underscores
    for (char& c : path) {
        if (c == '/' || c == '\\' || c == '.') {
            c = '_';
        }
    }
    
    // If the path is empty, use "global"
    if (path.empty()) {
        return "global";
    }
    
    return path;
}

// ─── Primitive Type Encoding ─────────────────────────────────────────────

char encodePrimitiveKind(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:   return 'b';
        case PrimitiveKind::Int8:   return 'c';  // char
        case PrimitiveKind::Int16:  return 's';
        case PrimitiveKind::Int32:  return 'i';
        case PrimitiveKind::Int64:  return 'l';
        case PrimitiveKind::Uint8:  return 'h';  // unsigned char
        case PrimitiveKind::Uint16: return 't';  // unsigned short
        case PrimitiveKind::Uint32: return 'u';
        case PrimitiveKind::Uint64: return 'm';  // unsigned long
        case PrimitiveKind::Byte:   return 'c';
        case PrimitiveKind::Short:  return 's';
        case PrimitiveKind::Int:    return 'i';
        case PrimitiveKind::Long:   return 'l';
        case PrimitiveKind::Ubyte:  return 'h';
        case PrimitiveKind::Ushort: return 't';
        case PrimitiveKind::Uint:   return 'u';
        case PrimitiveKind::Ulong:  return 'm';
        case PrimitiveKind::Float:  return 'f';
        case PrimitiveKind::Double: return 'd';
        case PrimitiveKind::Decimal:return 'D';
        case PrimitiveKind::String: return 'S';
        case PrimitiveKind::Char:   return 'C';
        default:                    return '?';
    }
}

bool isPrimitiveType(const TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

} // namespace codegen