/// @file BuiltinTypes.cpp
/// @brief Implementation of built-in type helpers.

#include "BuiltinTypes.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include <string>

namespace builtins {

// ─── Arena Method Name Parsing ──────────────────────────────────────────

std::optional<ArenaMethodKind> parseArenaMethod(InternedString name, StringPool& pool) {
    std::string_view sv = lookupStringView(name);
    
    if (sv == "create")    return ArenaMethodKind::Create;
    if (sv == "empty")     return ArenaMethodKind::Empty;
    if (sv == "alloc")     return ArenaMethodKind::Alloc;
    if (sv == "reset")     return ArenaMethodKind::Reset;
    if (sv == "descriptor") return ArenaMethodKind::Descriptor;
    if (sv == "capacity")  return ArenaMethodKind::Capacity;
    if (sv == "remaining") return ArenaMethodKind::Remaining;
    if (sv == "isEmpty")   return ArenaMethodKind::IsEmpty;
    if (sv == "space")     return ArenaMethodKind::Space;
    if (sv == "canFit")    return ArenaMethodKind::CanFit;
    
    return std::nullopt;
}

// ─── Type Detection ──────────────────────────────────────────────────────

bool isArenaType(TypeAST* type) {
    if (!type) return false;
    
    // Check NamedTypeAST directly
    if (auto* named = type->as<NamedTypeAST>()) {
        return isArenaNamedType(named);
    }
    
    // Arena! (fallible) - unwrap and check
    if (auto* fallible = type->as<FallibleTypeAST>()) {
        return isArenaType(fallible->inner);
    }
    
    return false;
}

bool isArenaDescriptorType(TypeAST* type) {
    if (!type) return false;
    
    if (auto* named = type->as<NamedTypeAST>()) {
        return isArenaDescriptorNamedType(named);
    }
    
    return false;
}

bool isArenaNamedType(NamedTypeAST* named) {
    if (!named) return false;
    if (!named->genericArgs.empty()) return false;
    return lookupStringView(named->name) == "Arena";
}

bool isArenaDescriptorNamedType(NamedTypeAST* named) {
    if (!named) return false;
    if (!named->genericArgs.empty()) return false;
    return lookupStringView(named->name) == "ArenaDescriptor";
}

// ─── Type Validation ─────────────────────────────────────────────────────

bool validateArenaType(TypeAST* type, 
                       BaseAST* node,
                       StringPool& pool,
                       DiagnosticEngine& diag) {
    if (isArenaType(type)) {
        return true;
    }
    
    diag.error(DiagCode::Sem_BuiltinTypeMismatch, node,
               "expected Arena type, got ", 
               type ? typeToString(type, pool) : "unknown");
    return false;
}

bool validateArenaDescriptorType(TypeAST* type,
                                  BaseAST* node,
                                  StringPool& pool,
                                  DiagnosticEngine& diag) {
    if (isArenaDescriptorType(type)) {
        return true;
    }
    
    diag.error(DiagCode::Sem_BuiltinTypeMismatch, node,
               "expected ArenaDescriptor type, got ",
               type ? typeToString(type, pool) : "unknown");
    return false;
}

bool validateNotArenaDescriptorLiteral(TypeAST* type,
                                        BaseAST* node,
                                        StringPool& pool,
                                        DiagnosticEngine& diag) {
    if (!type) return true;
    
    if (isArenaDescriptorType(type)) {
        diag.error(DiagCode::Sem_ArenaDescriptorLiteral, node,
                   "ArenaDescriptor is a built-in type and cannot be constructed "
                   "via struct literal syntax");
        diag.note(node,
                  "ArenaDescriptor can only be obtained via arena::descriptor()");
        return false;
    }
    
    return true;
}

// ─── Arena Declaration Validation ──────────────────────────────────────

bool validateArenaBindingConst(VarDeclAST* decl,
                                DiagnosticEngine& diag) {
    if (decl->keyword == DeclKeyword::Let) {
        diag.error(DiagCode::Sem_ConstRequired, decl,
                   "Arena bindings must be declared with `const`");
        diag.note(decl,
                  "Reassigning an Arena binding would orphan slices "
                  "into its backing region");
        return false;
    }
    return true;
}

bool validateArenaInitializer(ExprAST* init,
                               StringPool& pool,
                               DiagnosticEngine& diag) {
    if (!init) {
        diag.error(DiagCode::Sem_MissingInitializer, init,
                   "Arena binding must be initialized with Arena::create(size) "
                   "or Arena::empty()");
        return false;
    }
    
    // Check if init is an ArenaAccessExprAST
    if (!init->isa<ArenaAccessExprAST>()) {
        diag.error(DiagCode::Sem_InvalidArenaInit, init,
                   "Arena binding must be initialized with Arena::create(size) "
                   "or Arena::empty()");
        diag.note(init,
                  "Found: ", init->resolvedType ? typeToString(init->resolvedType, pool) : "unknown");
        return false;
    }
    
    ArenaAccessExprAST* access = init->as<ArenaAccessExprAST>();
    
    // Must be static form (Arena::method)
    if (!access->isStatic) {
        diag.error(DiagCode::Sem_InvalidArenaInit, init,
                   "Arena binding must be initialized with Arena::create(size) "
                   "or Arena::empty(), not an existing arena");
        return false;
    }
    
    // Method must be "create" or "empty"
    std::string_view methodName = lookupStringView(access->methodName);
    if (methodName != "create" && methodName != "empty") {
        diag.error(DiagCode::Sem_InvalidArenaInit, init,
                   "Arena binding must be initialized with Arena::create(size) "
                   "or Arena::empty()");
        diag.note(init,
                  "Found: Arena::", methodName, " - only create and empty are valid");
        return false;
    }
    
    // create(size) requires exactly one argument
    if (methodName == "create") {
        if (access->args.size() != 1) {
            diag.error(DiagCode::Sem_ArenaMethodArgCount, init,
                       "Arena::create expects exactly 1 argument (size), got ",
                       access->args.size());
            return false;
        }
    }
    
    // empty() requires no arguments
    if (methodName == "empty") {
        if (access->args.size() != 0) {
            diag.error(DiagCode::Sem_ArenaMethodArgCount, init,
                       "Arena::empty takes no arguments");
            return false;
        }
    }
    
    return true;
}

bool validateArenaVarDecl(VarDeclAST* decl, 
                          StringPool& pool,
                          DiagnosticEngine& diag) {
    if (!decl || !isArenaType(decl->type)) {
        return true;  // Not an Arena binding
    }
    
    // Rule 1: Arena bindings must be declared with `const`
    if (!validateArenaBindingConst(decl, diag)) {
        return false;
    }
    
    // Rule 2: Initializer must be Arena::create(size) or Arena::empty()
    if (!validateArenaInitializer(decl->init, pool, diag)) {
        return false;
    }
    
    return true;
}

// ─── Arena Access Validation ────────────────────────────────────────────

std::optional<ArenaMethodKind> validateArenaAccess(ArenaAccessExprAST* expr,
                                                    StringPool& pool,
                                                    DiagnosticEngine& diag) {
    if (!expr) return std::nullopt;
    
    // ─── Step 1: Parse the method name ─────────────────────────────────
    auto methodOpt = parseArenaMethod(expr->methodName, pool);
    if (!methodOpt) {
        diag.error(DiagCode::Sem_UnknownMethod, expr,
                   "unknown arena method '", lookupStringView(expr->methodName), "'");
        diag.note(expr,
                  "Available arena methods: create, empty, alloc, reset, "
                  "descriptor, capacity, remaining, isEmpty, space, canFit");
        return std::nullopt;
    }
    ArenaMethodKind method = *methodOpt;
    
    // ─── Step 2: Validate static vs instance ──────────────────────────
    if (expr->isStatic && !isArenaMethodStatic(method)) {
        diag.error(DiagCode::Sem_ArenaMethodStatic, expr,
                   "Arena::", lookupStringView(expr->methodName),
                   " is not a static method");
        diag.note(expr,
                  "Only Arena::create and Arena::empty are static methods");
        return std::nullopt;
    }
    
    if (!expr->isStatic && isArenaMethodStatic(method)) {
        diag.error(DiagCode::Sem_ArenaMethodStatic, expr,
                   "arena::", lookupStringView(expr->methodName),
                   " is a static method (use Arena::", 
                   lookupStringView(expr->methodName), " instead)");
        return std::nullopt;
    }
    
    // ─── Step 3: Validate generic arguments ────────────────────────────
    bool hasGenericArgs = !expr->genericArgs.empty();
    bool requiresGenericArg = arenaMethodRequiresGenericArg(method);
    
    if (requiresGenericArg && !hasGenericArgs) {
        diag.error(DiagCode::Sem_ArenaMethodGenericArg, expr,
                   "arena::", lookupStringView(expr->methodName),
                   "<T> requires a type argument");
        return std::nullopt;
    }
    
    if (!requiresGenericArg && hasGenericArgs) {
        diag.error(DiagCode::Sem_ArenaMethodGenericArg, expr,
                   "arena::", lookupStringView(expr->methodName),
                   " does not take generic arguments");
        return std::nullopt;
    }
    
    if (requiresGenericArg && expr->genericArgs.size() != 1) {
        diag.error(DiagCode::Sem_GenericArityMismatch, expr,
                   "arena::", lookupStringView(expr->methodName),
                   "<T> expects exactly 1 generic argument, got ",
                   expr->genericArgs.size());
        return std::nullopt;
    }
    
    // ─── Step 4: Validate argument count ──────────────────────────────
    if (!validateArenaMethodArgCount(method, expr->args.size(), expr, diag)) {
        return std::nullopt;
    }
    
    return method;
}

bool validateArenaMethodArgCount(ArenaMethodKind method,
                                  size_t argCount,
                                  BaseAST* node,
                                  DiagnosticEngine& diag) {
    bool valid = true;
    
    switch (method) {
        case ArenaMethodKind::Create:
            if (argCount != 1) {
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "Arena::create expects exactly 1 argument (size), got ", argCount);
                valid = false;
            }
            break;
            
        case ArenaMethodKind::Empty:
            if (argCount != 0) {
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "Arena::empty takes no arguments");
                valid = false;
            }
            break;
            
        case ArenaMethodKind::Alloc:
            if (argCount != 1) {
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "arena::alloc<T> expects exactly 1 argument (count), got ", argCount);
                valid = false;
            }
            break;
            
        case ArenaMethodKind::Reset:
        case ArenaMethodKind::Descriptor:
        case ArenaMethodKind::Capacity:
        case ArenaMethodKind::Remaining:
        case ArenaMethodKind::IsEmpty:
            if (argCount != 0) {
                std::string name = "";
                switch (method) {
                    case ArenaMethodKind::Reset:      name = "reset"; break;
                    case ArenaMethodKind::Descriptor: name = "descriptor"; break;
                    case ArenaMethodKind::Capacity:   name = "capacity"; break;
                    case ArenaMethodKind::Remaining:  name = "remaining"; break;
                    case ArenaMethodKind::IsEmpty:    name = "isEmpty"; break;
                    default: break;
                }
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "arena::", name, " takes no arguments");
                valid = false;
            }
            break;
            
        case ArenaMethodKind::Space:
            if (argCount != 0) {
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "arena::space<T> takes no arguments (type argument only)");
                valid = false;
            }
            break;
            
        case ArenaMethodKind::CanFit:
            if (argCount != 1) {
                diag.error(DiagCode::Sem_ArenaMethodArgCount, node,
                           "arena::canFit<T> expects exactly 1 argument (count), got ", argCount);
                valid = false;
            }
            break;
    }
    
    return valid;
}

bool arenaMethodRequiresGenericArg(ArenaMethodKind method) {
    return method == ArenaMethodKind::Alloc ||
           method == ArenaMethodKind::Space ||
           method == ArenaMethodKind::CanFit;
}

bool isArenaMethodStatic(ArenaMethodKind method) {
    return method == ArenaMethodKind::Create ||
           method == ArenaMethodKind::Empty;
}

bool isArenaMethodVoid(ArenaMethodKind method) {
    return method == ArenaMethodKind::Reset;
}

TypeAST* getArenaMethodReturnType(ArenaMethodKind method,
                                   TypeAST* genericArg,
                                   StringPool& pool,
                                   ASTArena& arena) {
    switch (method) {
        case ArenaMethodKind::Create:
            // Arena::create(size) -> Arena!
            // Return type is Arena! (fallible)
            // Caller must wrap in FallibleTypeAST
            return createArenaType(pool, arena);
            
        case ArenaMethodKind::Empty:
            // Arena::empty() -> Arena
            return createArenaType(pool, arena);
            
        case ArenaMethodKind::Alloc:
            // arena::alloc<T>(count) -> [_]T
            // Return slice of T (ArrayKind::Slice)
            if (genericArg) {
                return arena.make<ArrayTypeAST>(ArrayKind::Slice, 0, genericArg);
            }
            return nullptr;
            
        case ArenaMethodKind::Reset:
            // arena::reset() -> ()
            return nullptr;  // Void
            
        case ArenaMethodKind::Descriptor:
            // arena::descriptor() -> ArenaDescriptor
            return createArenaDescriptorType(pool, arena);
            
        case ArenaMethodKind::Capacity:
        case ArenaMethodKind::Remaining:
        case ArenaMethodKind::Space:
            // capacity() -> uint64, remaining() -> uint64, space<T>() -> uint64
            return arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint64);
            
        case ArenaMethodKind::IsEmpty:
        case ArenaMethodKind::CanFit:
            // isEmpty() -> bool, canFit<T>() -> bool
            return arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
    }
    
    return nullptr;
}

// ─── ArenaDescriptor Type Construction ─────────────────────────────────

NamedTypeAST* createArenaDescriptorType(StringPool& pool, ASTArena& arena) {
    InternedString name = pool.intern("ArenaDescriptor");
    NamedTypeAST* type = arena.make<NamedTypeAST>(name);
    type->genericArgs = arena.makeBuilder<TypeAST*>().build();
    // resolvedDecl remains nullptr - it's a built-in type
    return type;
}

NamedTypeAST* createArenaType(StringPool& pool, ASTArena& arena) {
    InternedString name = pool.intern("Arena");
    NamedTypeAST* type = arena.make<NamedTypeAST>(name);
    type->genericArgs = arena.makeBuilder<TypeAST*>().build();
    // resolvedDecl remains nullptr - it's a built-in type
    return type;
}

// ─── ArenaDescriptor Layout Information ────────────────────────────────

InternedString getArenaDescriptorFieldName(size_t index, StringPool& pool) {
    switch (index) {
        case 0: return pool.intern("base");
        case 1: return pool.intern("size");
        default: return InternedString();
    }
}

TypeAST* getArenaDescriptorFieldType(size_t index, StringPool& pool, ASTArena& arena) {
    switch (index) {
        case 0: {
            // base: *uint8
            TypeAST* uint8Type = arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint8);
            return arena.make<PtrTypeAST>(uint8Type);
        }
        case 1: {
            // size: uint64
            return arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint64);
        }
        default:
            return nullptr;
    }
}

} // namespace builtins