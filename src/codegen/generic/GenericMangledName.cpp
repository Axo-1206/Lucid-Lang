/// @file codegen/support/GenericMangledName.cpp
/// @brief Implementation of generic instantiation mangling.

#include "GenericMangledName.hpp"
#include "debug/DebugUtils.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace codegen {

// ─── Private Helper: Build Mangled String ──────────────────────────────────

static InternedString buildMangledName(const std::string& components, CodeGenContext& ctx) {
    std::string result = "_L";
    result += components;
    return ctx.pool.intern(result);
}

// ─── Public API ─────────────────────────────────────────────────────────────

InternedString generateMangledNameForGeneric(
    DeclAST* baseDecl,
    const std::vector<TypeAST*>& typeArgs,
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
        result += typeToMangleString(typeArgs[i], ctx.pool);
    }
    
    // ─── 4. For functions, also encode parameter and return types ──────
    //
    // NOTE: `as<T>()` is an UNCHECKED cast (assert-only in debug, UB in
    // release on a kind mismatch) - it is NOT like dyn_cast and never
    // returns nullptr. `if (X* x = ptr->as<T>())` is therefore always
    // truthy regardless of the actual kind. The correct pattern (already
    // used elsewhere in this codebase, e.g. CodeGenGeneric.cpp's
    // shouldSpecialize()) is to guard with isa<T>() first.
    if (baseDecl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = baseDecl->as<FuncDeclAST>();
        GenericSubstitution subst{funcDecl->genericParams, typeArgs};

        // Parameter types (substituted at any depth - not just bare `T`)
        result += "_P";
        FuncTypeAST* funcType = funcDecl->funcType;
        while (funcType) {
            for (ParamAST* param : funcType->params) {
                result += typeToMangleString(param->type, ctx.pool, &subst);
            }
            funcType = funcType->getNext();
        }

        // Return type (substituted at any depth)
        if (funcDecl->funcType->returnType) {
            result += "_R" + typeToMangleString(funcDecl->funcType->returnType, ctx.pool, &subst);
        } else {
            result += "_RV";
        }
    }

    // ─── 5. For structs, encode field types ──────────────────────────────
    if (baseDecl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = baseDecl->as<StructDeclAST>();
        GenericSubstitution subst{structDecl->genericParams, typeArgs};

        result += "_F";
        for (FieldDeclAST* field : structDecl->fields) {
            result += typeToMangleString(field->type, ctx.pool, &subst);
        }
    }
    
    return buildMangledName(result, ctx);
}

// ─── Core Encoding Functions ──────────────────────────────────────────────

std::string typeToMangleString(TypeAST* type, StringPool& pool, const GenericSubstitution* subst) {
    if (!type) return "V";
    
    switch (type->kind) {
        case ASTKind::PrimitiveType: {
            PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
            char code = encodePrimitiveKind(prim->primitiveKind);
            return std::string(1, code);
        }
        
        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();

            // Substitute BEFORE encoding, at whatever depth this NamedType
            // occurs - this is what makes e.g. `*T`, `T?`, and `Box<T>`
            // mangle differently per concrete instantiation instead of all
            // collapsing to the literal name "T".
            if (subst) {
                if (TypeAST* substituted = subst->lookup(named->name)) {
                    return typeToMangleString(substituted, pool, subst);
                }
            }

            std::string name = sanitizeForMangledName(pool.lookup(named->name));
            
            if (!named->genericArgs.empty()) {
                name += "_G";
                for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                    if (i > 0) name += "_";
                    name += typeToMangleString(named->genericArgs[i], pool, subst);
                }
            }
            return name;
        }
        
        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            std::string result = "A";
            if (arr->isFixed()) {
                result += std::to_string(arr->size);
            } else if (arr->isSlice()) {
                result += "_";
            } else {
                result += "*";
            }
            result += typeToMangleString(arr->element, pool, subst);
            return result;
        }
        
        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return "P" + typeToMangleString(ptr->inner, pool, subst);
        }
        
        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return "R" + typeToMangleString(ref->inner, pool, subst);
        }
        
        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return "N" + typeToMangleString(nullable->inner, pool, subst);
        }
        
        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return "F" + typeToMangleString(fallible->inner, pool, subst);
        }
        
        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return "X" + typeToMangleString(combined->inner, pool, subst);
        }
        
        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            std::string result = "F";
            
            // Parameter types
            for (ParamAST* param : func->params) {
                result += typeToMangleString(param->type, pool, subst);
            }
            result += "_";
            
            // Return type
            if (func->returnType) {
                result += typeToMangleString(func->returnType, pool, subst);
            } else {
                result += "V";
            }
            return result;
        }
        
        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return "U" + typeToMangleString(future->inner, pool, subst);
        }
        
        case ASTKind::ThreadType: {
            const ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return "H" + typeToMangleString(thread->inner, pool, subst);
        }

        case ASTKind::ModuleTypeAccess: {
            // Previously unhandled - fell through to the generic "?<kind>"
            // default below, which produced the SAME placeholder string
            // for every module-qualified type regardless of which module
            // or type it actually named, causing collisions whenever two
            // different cross-module types were used as generic arguments.
            ModuleTypeAccessAST* access = type->as<ModuleTypeAccessAST>();
            std::string name = "M" + sanitizeForMangledName(pool.lookup(access->moduleName)) +
                                "_" + sanitizeForMangledName(pool.lookup(access->typeName));
            if (!access->genericArgs.empty()) {
                name += "_G";
                for (size_t i = 0; i < access->genericArgs.size(); ++i) {
                    if (i > 0) name += "_";
                    name += typeToMangleString(access->genericArgs[i], pool, subst);
                }
            }
            return name;
        }
        
        default:
            return "?" + debug::kindToString(type->kind);
    }
}

// ─── Helper Functions ─────────────────────────────────────────────────────

std::string sanitizeForMangledName(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return result;
}

std::string getMangledModulePath(CodeGenContext& ctx) {
    if (!ctx.module) return "global";
    std::string path = ctx.module->getName().str();
    for (char& c : path) {
        if (c == '/' || c == '\\' || c == '.') c = '_';
    }
    return path.empty() ? "global" : path;
}

char encodePrimitiveKind(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:   return 'b';
        case PrimitiveKind::Int8:   return 'c';
        case PrimitiveKind::Int16:  return 's';
        case PrimitiveKind::Int32:  return 'i';
        case PrimitiveKind::Int64:  return 'l';
        case PrimitiveKind::Uint8:  return 'h';
        case PrimitiveKind::Uint16: return 't';
        case PrimitiveKind::Uint32: return 'u';
        case PrimitiveKind::Uint64: return 'm';
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

bool isPrimitiveType(TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

} // namespace codegen