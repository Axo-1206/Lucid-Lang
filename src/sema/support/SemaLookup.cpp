/// @file SemaLookup.cpp
/// @brief Implements all name lookup with proper priority and diagnostics.

#include "SemaLookup.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// Generic Parameter Lookup
// =============================================================================

bool isGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupGenericParam(name) != nullptr;
}

const GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupGenericParam(name);
}

// =============================================================================
// Value Lookup (variables, functions, parameters, fields, enum variants)
// =============================================================================

const ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupValue(name);
}

const ValueDeclAST* resolveValueOrError(const IdentifierExprAST* expr, SemaContext& ctx) {
    if (const ValueDeclAST* decl = lookupValue(expr->name, ctx)) {
        return decl;
    }
    ctx.error(expr, DiagCode::E2001, 
               "undefined value '", ctx.pool().lookup(expr->name), "'");
    return nullptr;
}

const FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx) {
    const ValueDeclAST* value = lookupValue(name, ctx);
    return (value && value->isa<FuncDeclAST>()) ? value->as<FuncDeclAST>() : nullptr;
}

// =============================================================================
// Type Lookup (structs, enums, traits)
// =============================================================================

const TypeDeclAST* lookupType(InternedString name, SemaContext& ctx) {
    return ctx.symbols.lookupType(name);
}

const TypeDeclAST* resolveTypeOrError(const NamedTypeAST* type, SemaContext& ctx) {
    // Generic parameters have highest priority - they shadow type names
    if (isGenericParam(type->name, ctx)) {
        return nullptr; // Valid, just not a TypeDeclAST
    }

    // Look up as concrete type (local → module)
    if (const TypeDeclAST* decl = lookupType(type->name, ctx)) {
        return decl;
    }

    // Not found - report error
    ctx.error(type, DiagCode::E2002, 
               "undefined type '", ctx.pool().lookup(type->name), "'");
    return nullptr;
}

const TypeDeclAST* resolveTypeNameOrError(const NamedTypeAST* type, SemaContext& ctx) {
    return resolveTypeOrError(type, ctx);
}

// =============================================================================
// Trait Reference Resolution
// =============================================================================

const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    // Look up the type declaration by name
    const TypeDeclAST* typeDecl = lookupType(ref->name, ctx);
    if (!typeDecl) {
        ctx.error(ref, DiagCode::E2002,
                  "undefined trait '", ctx.pool().lookup(ref->name), "'");
        return nullptr;
    }

    // Verify it's a trait (not a struct or enum)
    if (!typeDecl->isa<TraitDeclAST>()) {
        ctx.error(ref, DiagCode::E2002,
                  "'", ctx.pool().lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    const TraitDeclAST* traitDecl = typeDecl->as<TraitDeclAST>();

    // Check generic arguments if present
    if (!ref->genericArgs.empty()) {
        // TODO: Check generic argument arity against the trait's generic parameters
        // TODO: Check constraints are satisfied
        // For now, just resolve each generic argument type
        for (const TypePtr arg : ref->genericArgs) {
            resolveType(arg, ctx);
        }
    }

    return traitDecl;
}

// =============================================================================
// Module Member Lookup (module:member)
// =============================================================================

const ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx) {
    if (!module) return nullptr;
    
    ModuleTable* table = ctx.symbols.findModuleTable(module);
    if (!table) return nullptr;
    
    auto it = table->values.find(memberName);
    return it != table->values.end() ? it->second : nullptr;
}

const ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx) {
    ModuleAST* mod = ctx.symbols.lookupImport(access->moduleName);
    if (!mod) {
        ctx.error(access, DiagCode::E2001,
                   "undefined module alias '", ctx.pool().lookup(access->moduleName), "'");
        return nullptr;
    }
    
    const ValueDeclAST* member = lookupModuleMember(mod, access->memberName, ctx);
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

const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx) {
    // Plain call: `foo(...)`
    if (callee->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        const ValueDeclAST* value = lookupValue(id->name, ctx);
        
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
        const ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        const ValueDeclAST* value = resolveModuleMemberOrError(const_cast<ModuleAccessExprAST*>(access), ctx);
        
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
// REDECLARATION HELPERS - Check only the current tier (not outer scopes)
// =============================================================================

bool isValueRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.symbols.isAtModuleLevel()) {
        ModuleTable* table = ctx.symbols.currentModuleTable();
        return table && table->values.find(name) != table->values.end();
    } else {
        const Scope& current = ctx.symbols.currentScope();
        return current.values.find(name) != current.values.end();
    }
}

bool isTypeRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.symbols.isAtModuleLevel()) {
        ModuleTable* table = ctx.symbols.currentModuleTable();
        return table && table->types.find(name) != table->types.end();
    } else {
        const Scope& current = ctx.symbols.currentScope();
        return current.types.find(name) != current.types.end();
    }
}

bool isGenericParamRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.symbols.isAtModuleLevel()) {
        return false; // Generic params are never at module level
    }
    const Scope& current = ctx.symbols.currentScope();
    return current.genericParams.find(name) != current.genericParams.end();
}

bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx) {
    ModuleTable* table = ctx.symbols.currentModuleTable();
    if (!table) return false;
    return table->importAliases.find(alias) != table->importAliases.end();
}

// ─── Value Redeclaration ──────────────────────────────────────────────────

bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isValueRedeclared(node->name, ctx)) {
        ctx.error(node, DiagCode::E2101,
                  "redeclaration of '", ctx.pool().lookup(node->name), "' in the same scope");
        return true;
    }
    return false;
}

// ─── Type Redeclaration ──────────────────────────────────────────────────

bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isTypeRedeclared(node->name, ctx)) {
        ctx.error(node, DiagCode::E2101,
                  "redeclaration of '", ctx.pool().lookup(node->name), "' in the same scope");
        return true;
    }
    return false;
}

// ─── Generic Param Redeclaration ─────────────────────────────────────────

bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isGenericParamRedeclared(node->name, ctx)) {
        ctx.error(node, DiagCode::E2101,
                  "redeclaration of generic parameter '", ctx.pool().lookup(node->name), "' in the same scope");
        return true;
    }
    return false;
}

// ─── Import Alias Redeclaration ──────────────────────────────────────────

bool reportImportAliasRedeclaration(InternedString alias, const BaseAST* node, SemaContext& ctx) {
    if (isImportAliasRedeclared(alias, ctx)) {
        ctx.error(node, DiagCode::E2101,
                  "redeclaration of import alias '", ctx.pool().lookup(alias), "'");
        return true;
    }
    return false;
}

} // namespace sema