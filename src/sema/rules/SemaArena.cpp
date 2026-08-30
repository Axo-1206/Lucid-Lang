/// @file SemaArena.cpp
/// @brief Implementation of Arena type validation.

#include "SemaArena.hpp"
#include "core/builtins/BuiltinTypes.hpp"
#include "core/ASTStrings.hpp"
#include "../types/SemaCompare.hpp"
#include "../types/SemaResolve.hpp"
#include "../types/SemaValidate.hpp"

namespace sema {

// ─── Arena Type Detection ──────────────────────────────────────────────────

bool isArenaType(TypeAST* type) {
    return builtins::isArenaType(const_cast<TypeAST*>(type));
}

bool isArenaDescriptorType(TypeAST* type) {
    return builtins::isArenaDescriptorType(const_cast<TypeAST*>(type));
}

bool isArenaNamedType(NamedTypeAST* named) {
    return builtins::isArenaNamedType(named);
}

bool isArenaDescriptorNamedType(NamedTypeAST* named) {
    return builtins::isArenaDescriptorNamedType(named);
}

bool isArenaBinding(VarDeclAST* decl) {
    if (!decl) return false;
    return isArenaType(decl->type);
}

// ─── Arena Type Resolution ─────────────────────────────────────────────────

TypeAST* getArenaType(SemaContext& ctx) {
    InternedString name = ctx.pool.intern("Arena");
    
    // Check cache first
    TypeCache::NamedTypeKey key{name, ctx.arena.makeBuilder<TypeAST*>().build()};
    auto it = ctx.typeCache.namedTypes.find(key);
    if (it != ctx.typeCache.namedTypes.end()) {
        return it->second;
    }
    
    // Create and cache the Arena type
    NamedTypeAST* type = builtins::createArenaType(ctx.pool, ctx.arena);
    ctx.typeCache.namedTypes[key] = type;
    return type;
}

TypeAST* getArenaDescriptorType(SemaContext& ctx) {
    InternedString name = ctx.pool.intern("ArenaDescriptor");
    
    // Check cache first
    TypeCache::NamedTypeKey key{name, ctx.arena.makeBuilder<TypeAST*>().build()};
    auto it = ctx.typeCache.namedTypes.find(key);
    if (it != ctx.typeCache.namedTypes.end()) {
        return it->second;
    }
    
    // Create and cache the ArenaDescriptor type
    NamedTypeAST* type = builtins::createArenaDescriptorType(ctx.pool, ctx.arena);
    ctx.typeCache.namedTypes[key] = type;
    return type;
}

// ─── Arena Declaration Resolution ─────────────────────────────────────────

bool resolveArenaVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl || !isArenaBinding(decl)) {
        return true;  // Not an Arena binding
    }
    
    // ─── Rule 1: Arena bindings must be declared with `const` ──────────────
    if (decl->keyword == DeclKeyword::Let) {
        ctx.diagnostics.error(DiagCode::Sem_ConstRequired, decl,
                              "Arena bindings must be declared with `const`");
        ctx.diagnostics.note(decl,
                              "Reassigning an Arena binding would orphan slices "
                              "into its backing region");
        return false;
    }
    
    // ─── Rule 2: Initializer must be valid ──────────────────────────────────
    if (!decl->init) {
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty()");
        return false;
    }
    
    return validateArenaInitializer(decl->init, ctx);
}

// ─── Arena Initializer Validation ─────────────────────────────────────────

bool validateArenaInitializer(ExprAST* init, SemaContext& ctx) {
    if (!init) {
        return false;
    }
    
    // Must be an ArenaAccessExprAST
    if (!init->isa<ArenaAccessExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty()");
        ctx.diagnostics.note(init,
                              "Found: ", init->resolvedType 
                              ? typeToString(init->resolvedType, ctx.pool) 
                              : "unknown");
        return false;
    }
    
    ArenaAccessExprAST* access = init->as<ArenaAccessExprAST>();
    
    // Must be static form (Arena::method)
    if (!access->isStatic) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty(), "
                              "not an existing arena");
        return false;
    }
    
    // Method must be "create" or "empty"
    std::string_view methodName = lookupStringView(access->methodName);
    if (methodName != "create" && methodName != "empty") {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArenaInit, init,
                              "Arena binding must be initialized with "
                              "Arena::create(size) or Arena::empty()");
        ctx.diagnostics.note(init,
                              "Found: Arena::", methodName, 
                              " - only create and empty are valid");
        return false;
    }
    
    // create(size) requires exactly one argument
    if (methodName == "create") {
        if (access->args.size() != 1) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaMethodArgCount, init,
                                  "Arena::create expects exactly 1 argument (size), got ",
                                  access->args.size());
            return false;
        }
    }
    
    // empty() requires no arguments
    if (methodName == "empty") {
        if (access->args.size() != 0) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaMethodArgCount, init,
                                  "Arena::empty takes no arguments");
            return false;
        }
    }
    
    return true;
}

// ─── Arena Access Resolution ──────────────────────────────────────────────

TypeAST* resolveArenaAccess(ArenaAccessExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;
    
    // ─── Step 1: Validate the access using the pure validator ──────────────
    auto methodOpt = builtins::validateArenaAccess(expr, ctx.pool, ctx.diagnostics);
    if (!methodOpt) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }
    builtins::ArenaMethodKind method = *methodOpt;
    
    // ─── Step 2: For instance methods, validate the LHS ────────────────────
    TypeAST* returnType = nullptr;
    ValueState state = ValueState::Definite;
    
    if (!expr->isStatic) {
        // Instance form: arena::method()
        
        // ─── 2a: Validate LHS is Arena type ──────────────────────────────
        if (!expr->arenaExpr) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaInvalidLHS, expr,
                                  "instance arena access requires an Arena expression");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
        
        TypeAST* arenaType = expr->arenaExpr->resolvedType;
        if (!arenaType || !isArenaType(arenaType)) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaInvalidLHS, expr,
                                  "arena:: access requires an Arena value, got ",
                                  arenaType ? typeToString(arenaType, ctx.pool) : "unknown");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
        
        // ─── 2b: LHS must be a binding ──────────────────────────────
        // Arena bindings must be (validated at declaration site)
        // But we also check here for safety (e.g., if the binding was mutated)
        if (expr->arenaExpr->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = expr->arenaExpr->as<IdentifierExprAST>();
            ValueDeclAST* decl = id->resolvedDecl;
            if (decl && decl->isa<VarDeclAST>()) {
                VarDeclAST* varDecl = decl->as<VarDeclAST>();
                if (varDecl->keyword == DeclKeyword::Let) {
                    ctx.diagnostics.error(DiagCode::Sem_ArenaNotConst, expr,
                                          "Arena access requires a binding");
                    ctx.diagnostics.note(expr,
                                          "Arena bindings must be declared with const");
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
            }
        }
    }
    
    // ─── Step 3: Build the return type ─────────────────────────────────────
    TypeAST* genericArg = nullptr;
    if (!expr->genericArgs.empty() && expr->genericArgs[0]) {
        genericArg = expr->genericArgs[0];
    }
    
    returnType = builtins::getArenaMethodReturnType(
        method, 
        genericArg,
        ctx.pool,
        ctx.arena
    );
    
    // ─── Step 4: Special handling for specific methods ─────────────────────
    switch (method) {
        case builtins::ArenaMethodKind::Create: {
            // Arena::create(size) -> Arena!
            // Wrap Arena in FallibleTypeAST
            TypeAST* arenaType = getArenaType(ctx);
            returnType = ctx.arena.make<FallibleTypeAST>(arenaType);
            state = ValueState::Err;  // Can fail (out of memory)
            break;
        }
        
        case builtins::ArenaMethodKind::Empty: {
            // Arena::empty() -> Arena
            returnType = getArenaType(ctx);
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::Alloc: {
            // arena::alloc<T>(count) -> [_]T
            // Return type is already ArrayTypeAST with Slice kind
            // The slice is a borrowed view - state is unknown (bounds check at runtime)
            state = ValueState::Unknown;
            break;
        }
        
        case builtins::ArenaMethodKind::Reset: {
            // arena::reset() -> ()
            returnType = nullptr;
            state = ValueState::None;
            break;
        }
        
        case builtins::ArenaMethodKind::Descriptor: {
            // arena::descriptor() -> ArenaDescriptor
            returnType = getArenaDescriptorType(ctx);
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::Capacity:
        case builtins::ArenaMethodKind::Remaining:
        case builtins::ArenaMethodKind::Space: {
            // capacity() -> uint64, remaining() -> uint64, space<T>() -> uint64
            // Return type is already PrimitiveTypeAST (Uint64)
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::IsEmpty:
        case builtins::ArenaMethodKind::CanFit: {
            // isEmpty() -> bool, canFit<T>() -> bool
            // Return type is already PrimitiveTypeAST (Bool)
            state = ValueState::Definite;
            break;
        }
    }
    
    // ─── Step 5: Store results ─────────────────────────────────────────────
    expr->resolvedType = returnType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->is= false;
    
    return returnType;
}

} // namespace sema