/**
 * @file Lookup.cpp
 * @brief Implements all name lookup with proper priority and diagnostics.
 *
 * @architectural_note Lookup Priority (per LUCID_GRAMMAR.md)
 *   1. Generic parameters in current scope (highest priority, shadow everything)
 *   2. Value/Type declarations in local scopes (innermost to outermost)
 *   3. Value/Type declarations in module scope (global)
 *
 * @architectural_note Two namespaces
 *   - VALUE NAMESPACE: variables, functions, parameters, fields, enum variants
 *   - TYPE NAMESPACE: structs, enums, traits, generic params
 *
 * @architectural_note No creation here
 *   The parser already created all AST nodes. This file only LOOKS UP
 *   existing names and reports errors if not found.
 */

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// =============================================================================
// Generic Parameter Lookup
// =============================================================================

/**
 * @brief Check if a name is a generic parameter in the current scope.
 *
 * Generic parameters have the HIGHEST priority and shadow type names.
 */
bool isGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupGenericParam(name) != nullptr;
}

/**
 * @brief Look up a generic parameter by name.
 *
 * @return The GenericParamDeclAST if found, nullptr otherwise.
 */
GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupGenericParam(name);
}

// =============================================================================
// Value Lookup (variables, functions, parameters, fields, enum variants)
// =============================================================================

/**
 * @brief Look up a value declaration by name.
 *
 * Searches: generic params → local scopes → module scope
 * Generic params are NOT values, so they don't match here.
 *
 * @return The ValueDeclAST if found, nullptr otherwise.
 */
ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupValue(name);
}

/**
 * @brief Look up a value and report E2001 if not found.
 *
 * This is the main function for value resolution.
 */
ValueDeclAST* resolveValueOrError(IdentifierExprAST* expr, SemaContext& ctx) {
    if (ValueDeclAST* decl = lookupValue(expr->name, ctx)) {
        return decl;
    }
    ctx.error(expr, DiagCode::E2001, 
               "undefined value '", ctx.pool().lookup(expr->name), "'");
    return nullptr;
}

/**
 * @brief Look up a function by name.
 *
 * Convenience wrapper that checks the resolved value is a FuncDeclAST.
 */
FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx) {
    ValueDeclAST* value = lookupValue(name, ctx);
    return (value && value->isa<FuncDeclAST>()) ? value->as<FuncDeclAST>() : nullptr;
}

// =============================================================================
// Type Lookup (structs, enums, traits)
// =============================================================================

/**
 * @brief Look up a type declaration by name.
 *
 * Searches: local scopes → module scope
 * Generic parameters are NOT type declarations (they shadow, but are separate).
 *
 * @return The TypeDeclAST if found, nullptr otherwise.
 */
TypeDeclAST* lookupType(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupType(name);
}

/**
 * @brief Look up a type with proper priority (generic params shadow types).
 *
 * This is the main type resolution function. It handles:
 *   1. Check if it's a generic parameter (returns nullptr, no error)
 *   2. Look up as concrete type (returns TypeDeclAST*)
 *   3. Not found (reports E2002, returns nullptr)
 *
 * @note Callers must check isGenericParam() separately if they need to
 *       distinguish "generic parameter" from "not found".
 */
TypeDeclAST* resolveTypeOrError(NamedTypeAST* type, SemaContext& ctx) {
    // Generic parameters have highest priority - they shadow type names
    if (isGenericParam(type->name, ctx)) {
        return nullptr; // Valid, just not a TypeDeclAST
    }

    // Look up as concrete type (local → module)
    if (TypeDeclAST* decl = lookupType(type->name, ctx)) {
        return decl;
    }

    // Not found - report error
    ctx.error(type, DiagCode::E2002, 
               "undefined type '", ctx.pool().lookup(type->name), "'");
    return nullptr;
}

/**
 * @brief Resolve a named type reference, reporting E2002 on failure.
 *
 * Alias for resolveTypeOrError() for consistency with resolveValueOrError().
 */
TypeDeclAST* resolveTypeNameOrError(NamedTypeAST* type, SemaContext& ctx) {
    return resolveTypeOrError(type, ctx);
}

// =============================================================================
// Module Member Lookup (module:member)
// =============================================================================

/**
 * @brief Look up a member in a module's table.
 *
 * Used for module:member access. The module must already be resolved.
 */
ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx) {
    if (!module) return nullptr;
    
    ModuleTable* table = ctx.symbols.findModuleTable(module);
    if (!table) return nullptr;
    
    auto it = table->values.find(memberName);
    return it != table->values.end() ? it->second : nullptr;
}

/**
 * @brief Resolve a module alias and look up a member, with error reporting.
 */
ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx) {
    ModuleAST* mod = ctx.symbols.lookupImport(access->moduleName);
    if (!mod) {
        ctx.error(access, DiagCode::E2001,
                   "undefined module alias '", ctx.pool().lookup(access->moduleName), "'");
        return nullptr;
    }
    
    ValueDeclAST* member = lookupModuleMember(mod, access->memberName, ctx);
    if (!member) {
        ctx.error(access, DiagCode::E2001,
                   "undefined member '", ctx.pool().lookup(access->moduleName), ":",
                   ctx.pool().lookup(access->memberName), "'");
        return nullptr;
    }
    
    return member;
}

// =============================================================================
// Callee Resolution (for function calls)
// =============================================================================

/**
 * @brief Resolve a call expression's callee to the FuncDeclAST it names.
 *
 * Handles two callee shapes:
 *   - IdentifierExprAST: Look up in value namespace
 *   - ModuleAccessExprAST: Look up module alias, then member
 *
 * Any other callee shape (curried call, function literal) returns nullptr
 * silently - the caller must check the callee's resolved type instead.
 */
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx) {
    // Plain call: `foo(...)`
    if (callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        ValueDeclAST* value = lookupValue(id->name, ctx);
        
        if (!value) {
            ctx.error(callee, DiagCode::E2001,
                       "undefined value '", ctx.pool().lookup(id->name), "'");
            return nullptr;
        }
        
        if (!value->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003,
                       "'", ctx.pool().lookup(id->name), "' is not callable");
            return nullptr;
        }
        
        return value->as<FuncDeclAST>();
    }

    // Cross-module call: `module:member(...)`
    if (callee->isa<ModuleAccessExprAST>()) {
        ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        ValueDeclAST* value = resolveModuleMemberOrError(access, ctx);
        
        if (!value) {
            return nullptr; // Error already reported
        }
        
        if (!value->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003,
                       "'", ctx.pool().lookup(access->moduleName), ":",
                       ctx.pool().lookup(access->memberName), "' is not callable");
            return nullptr;
        }
        
        return value->as<FuncDeclAST>();
    }

    // Any other callee shape - doesn't name a declaration
    // Caller must check callee's resolved type
    return nullptr;
}

// =============================================================================
// Self-Type Cache
// =============================================================================

/**
 * @brief Get (creating if necessary) the cached self-type reference for decl.
 *
 * Lazy cache: TypeDeclAST::selfType starts nullptr and is populated on first access.
 * Used when a type name appears as a value (e.g., `int("42")`).
 */
NamedTypeAST* selfTypeOf(TypeDeclAST* decl, SemaContext& ctx) {
    if (decl->selfType) {
        return decl->selfType;
    }

    NamedTypeAST* self = ctx.arena().makeType<NamedTypeAST>(decl->name);
    self->loc = decl->loc;
    decl->selfType = self;
    return self;
}

} // namespace sema