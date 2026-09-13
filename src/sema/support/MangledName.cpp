/// @file sema/support/MangledName.cpp
/// @brief Implementation of mangled name generation.

#include "MangledName.hpp"
#include "../context/Generic.hpp"
#include "core/ASTStrings.hpp"

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

InternedString generateMangledName(EnumDeclAST* decl, SemaContext& ctx) {
    if (!decl) return InternedString(0);
    
    std::string result;
    
    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";
    
    // ─── 2. Enum name ──────────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(decl->name));
    
    // ─── 3. Backing type (for disambiguation) ──────────────────────────────
    // This helps distinguish enums with different backing types
    // but the same name (unlikely, but safe for consistency).
    if (decl->backingType) {
        result += "_B" + typeToMangleString(decl->backingType, ctx);
    } else {
        // Default backing type is int32
        result += "_B" + typeToMangleString(ctx.getIntType(), ctx);
    }
    
    // ─── 4. Variant count (for disambiguation) ─────────────────────────────
    // Two enums with the same name and same backing type but different
    // variants would be different types. This ensures uniqueness.
    result += "_V" + std::to_string(decl->variants.size());
    
    return buildMangledName(result, ctx);
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

// ─── Substituting Type Encoding ─────────────────────────────────────────
//
// Like typeToMangleString, but consults a GenericSubstitution while
// walking: when a NamedTypeAST is a generic parameter, its concrete
// type is mangled in its place.
//
// This deliberately does NOT call substituteType. Mangling only needs
// to *read* the substitution map to decide what string to emit — it
// never needs the rewritten AST that substituteType produces. Building
// the rewritten tree here would allocate nodes the caller immediately
// discards, bloating the arena for every generic instantiation. Walking
// the original tree and substituting on the fly produces the same
// string with zero allocations.
//
// Recursion terminates because GenericSubstitution maps a parameter to
// a concrete type that does not itself reference the same parameter
// (Sema rejects self-referential instantiations before reaching
// mangling).

static std::string typeToMangleStringSubstituted(
    TypeAST* type,
    const GenericSubstitution& subst,
    SemaContext& ctx)
{
    if (!type) return "V";

    // ─── Generic parameter: mangle its concrete type in its place ────
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (subst.isParam(named->name)) {
            TypeAST* concrete = subst.lookup(named->name);
            if (concrete) {
                // Recurse with the same substitution: the concrete type
                // may itself contain parameters (e.g., a nested generic
                // instantiation `Pair<U, int>` where U is also bound).
                return typeToMangleStringSubstituted(concrete, subst, ctx);
            }
            // Fallthrough: parameter with no binding. Emit the name.
        }
    }

    // ─── Structural walk: mirror typeToMangleString exactly ──────────
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

            if (!named->genericArgs.empty()) {
                name += "_G";
                for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                    if (i > 0) name += "_";
                    name += typeToMangleStringSubstituted(
                        named->genericArgs[i], subst, ctx);
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
            result += typeToMangleStringSubstituted(arr->element, subst, ctx);
            return result;
        }

        case ASTKind::PtrType: {
            const PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return "P" + typeToMangleStringSubstituted(ptr->inner, subst, ctx);
        }

        case ASTKind::RefType: {
            const RefTypeAST* ref = type->as<RefTypeAST>();
            return "R" + typeToMangleStringSubstituted(ref->inner, subst, ctx);
        }

        case ASTKind::NullableType: {
            const NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return "N" + typeToMangleStringSubstituted(nullable->inner, subst, ctx);
        }

        case ASTKind::FallibleType: {
            const FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return "F" + typeToMangleStringSubstituted(fallible->inner, subst, ctx);
        }

        case ASTKind::CombinedType: {
            const CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return "X" + typeToMangleStringSubstituted(combined->inner, subst, ctx);
        }

        case ASTKind::FuncType: {
            const FuncTypeAST* func = type->as<FuncTypeAST>();
            std::string result = "F";

            for (ParamAST* param : func->params) {
                result += typeToMangleStringSubstituted(param->type, subst, ctx);
            }
            result += "_";

            if (func->returnType) {
                result += typeToMangleStringSubstituted(func->returnType, subst, ctx);
            } else {
                result += "V";
            }
            return result;
        }

        case ASTKind::FutureType: {
            const FutureTypeAST* future = type->as<FutureTypeAST>();
            return "U" + typeToMangleStringSubstituted(future->inner, subst, ctx);
        }

        case ASTKind::ThreadType: {
            const ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return "H" + typeToMangleStringSubstituted(thread->inner, subst, ctx);
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

// ─── Generic Instantiation Mangling ──────────────────────────────────────

InternedString generateMangledNameForGeneric(
    DeclAST* decl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx)
{
    if (!decl || typeArgs.empty()) {
        return InternedString(0);
    }

    std::string result;

    // ─── 1. Module path ──────────────────────────────────────────────────
    result += getMangledModulePath(ctx) + "_";

    // ─── 2. Declaration name ──────────────────────────────────────────────
    result += sanitizeForMangledName(ctx.pool.lookup(decl->name));

    // ─── 3. Generic arguments (concrete types) ──────────────────────────
    result += "_G";
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) result += "_";
        result += typeToMangleString(typeArgs[i], ctx);
    }

    // ─── 4. For functions, also encode parameter and return types ──────
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        GenericSubstitution subst{funcDecl->genericParams, typeArgs};

        // Parameter types (substituted)
        result += "_P";
        FuncTypeAST* funcType = funcDecl->funcType;
        while (funcType) {
            for (ParamAST* param : funcType->params) {
                if (param->type) {
                    result += typeToMangleStringSubstituted(
                        param->type, subst, ctx);
                }
            }
            funcType = funcType->getNext();
        }

        // Return type (substituted)
        if (funcDecl->funcType->returnType) {
            result += "_R" + typeToMangleStringSubstituted(
                funcDecl->funcType->returnType, subst, ctx);
        } else {
            result += "_RV";
        }
    }

    // ─── 5. For structs, encode field types ──────────────────────────────
    if (decl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = decl->as<StructDeclAST>();
        GenericSubstitution subst{structDecl->genericParams, typeArgs};

        result += "_F";
        for (FieldDeclAST* field : structDecl->fields) {
            if (field->type) {
                result += typeToMangleStringSubstituted(
                    field->type, subst, ctx);
            }
        }
    }

    return ctx.pool.intern("_L" + result);
}

} // namespace sema