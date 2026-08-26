/// @file sema/support/MangledName.cpp
/// @brief Implementation of mangled name generation.

#include "MangledName.hpp"
#include "core/ASTStrings.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace sema {

// ─── Private Helper: Build Mangled String ──────────────────────────────────

/// @brief Build a mangled name string from components.
/// @param components The components to join.
/// @param ctx The semantic context.
/// @return The full mangled name as an InternedString.
static InternedString buildMangledName(const std::string& components, SemaContext& ctx) {
    std::string result = "_L";
    result += components;
    return ctx.pool.intern(result);
}

// ─── Public API ─────────────────────────────────────────────────────────────

InternedString generateMangledName(FuncDeclAST* decl, SemaContext& ctx) {
    if (!decl) return InternedString(0);
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Function name ──────────────────────────────────────────────────
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
    
    // ─── 4. Parameter types ──────────────────────────────────────────────
    result += "_P";
    const FuncTypeAST* funcType = decl->funcType;
    while (funcType) {
        for (ParamAST* param : funcType->params) {
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

InternedString generateMangledName(VarDeclAST* decl, SemaContext& ctx) {
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
    InternedString baseName,
    const std::vector<TypeAST*>& typeArgs,
    SemaContext& ctx
) {
    if (typeArgs.empty() || !baseName.isValid()) {
        return baseName;
    }
    
    std::string result = ctx.pool.lookup(baseName);
    result += "_G";
    
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) result += "_";
        result += typeToMangleString(typeArgs[i], ctx);
    }
    
    return ctx.pool.intern(result);
}

InternedString generateMangledName(StructDeclAST* decl, SemaContext& ctx) {
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
    
    return buildMangledName(result, ctx);
}

// ─── Core Encoding Functions ──────────────────────────────────────────────

std::string typeToMangleString(TypeAST* type, SemaContext& ctx) {
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
                ctx.pool.lookup(named->name)
            );
            
            // Add generic arguments if present
            if (!named->genericArgs.empty()) {
                name += "_G";
                for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                    if (i > 0) name += "_";
                    name += typeToMangleString(named->genericArgs[i], ctx);
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
            result += typeToMangleString(arr->element, ctx);
            return result;
        }
        
        case ASTKind::PtrType: {
            const PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return "P" + typeToMangleString(ptr->inner, ctx);
        }
        
        case ASTKind::RefType: {
            const RefTypeAST* ref = type->as<RefTypeAST>();
            return "R" + typeToMangleString(ref->inner, ctx);
        }
        
        case ASTKind::NullableType: {
            const NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return "N" + typeToMangleString(nullable->inner, ctx);
        }
        
        case ASTKind::FallibleType: {
            const FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return "F" + typeToMangleString(fallible->inner, ctx);
        }
        
        case ASTKind::CombinedType: {
            const CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return "X" + typeToMangleString(combined->inner, ctx);
        }
        
        case ASTKind::FuncType: {
            const FuncTypeAST* func = type->as<FuncTypeAST>();
            std::string result = "F";
            
            // Parameter types
            for (ParamAST* param : func->params) {
                result += typeToMangleString(param->type, ctx);
            }
            result += "_";
            
            // Return type
            if (func->returnType) {
                result += typeToMangleString(func->returnType, ctx);
            } else {
                result += "V";
            }
            return result;
        }
        
        case ASTKind::FutureType: {
            const FutureTypeAST* future = type->as<FutureTypeAST>();
            return "U" + typeToMangleString(future->inner, ctx);
        }
        
        case ASTKind::ThreadType: {
            const ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return "H" + typeToMangleString(thread->inner, ctx);
        }
        
        default:
            return "?" + astKindToString(type->kind);
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

std::string getMangledModulePath(SemaContext& ctx) {
    if (!ctx.currentModule) {
        return "global";
    }
    
    std::string path = ctx.pool.lookup(ctx.currentModule->filePath);
    
    // Replace path separators and dots with underscores
    for (char& c : path) {
        if (c == '/' || c == '\\' || c == '.') {
            c = '_';
        }
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

} // namespace sema