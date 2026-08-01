/// @file SemaLookup.cpp
/// @brief Implementation of pure name lookup functions.

#include "SemaLookup.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace sema {

// ─── Module Lookup ───────────────────────────────────────────────────────

const ValueDeclAST* lookupModuleMember(ModuleAST* module, 
                                        InternedString memberName, 
                                        SemaContext& ctx) {
    if (!module) return nullptr;
    
    ModuleTable* table = ctx.findModuleTable(module);
    if (!table) return nullptr;
    
    auto it = table->values.find(memberName);
    return it != table->values.end() ? it->second : nullptr;
}

// ─── Redeclaration Checks ────────────────────────────────────────────────

bool isValueRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.isAtModuleLevel()) {
        ModuleTable* table = ctx.currentModuleTable;
        return table && table->values.find(name) != table->values.end();
    } else {
        const Scope& current = ctx.currentScope();
        return current.values.find(name) != current.values.end();
    }
}

bool isTypeRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.isAtModuleLevel()) {
        ModuleTable* table = ctx.currentModuleTable;
        return table && table->types.find(name) != table->types.end();
    } else {
        const Scope& current = ctx.currentScope();
        return current.types.find(name) != current.types.end();
    }
}

bool isGenericParamRedeclared(InternedString name, SemaContext& ctx) {
    if (ctx.isAtModuleLevel()) {
        return false; // Generic params are never at module level
    }
    const Scope& current = ctx.currentScope();
    return current.genericParams.find(name) != current.genericParams.end();
}

bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx) {
    ModuleTable* table = ctx.currentModuleTable;
    if (!table) return false;
    return table->importAliases.find(alias) != table->importAliases.end();
}

// ─── Redeclaration Reporting ─────────────────────────────────────────────

bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isValueRedeclared(node->name, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_Redeclaration, node,
                              "redeclaration of '", ctx.pool.lookup(node->name), 
                              "' in the same scope");
        return true;
    }
    return false;
}

bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isTypeRedeclared(node->name, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_Redeclaration, node,
                              "redeclaration of '", ctx.pool.lookup(node->name), 
                              "' in the same scope");
        return true;
    }
    return false;
}

bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx) {
    if (isGenericParamRedeclared(node->name, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_GenericParamRedeclaration, node,
                              "redeclaration of generic parameter '", 
                              ctx.pool.lookup(node->name), "' in the same scope");
        return true;
    }
    return false;
}

bool reportImportAliasRedeclaration(InternedString alias, 
                                     const BaseAST* node, 
                                     SemaContext& ctx) {
    if (isImportAliasRedeclared(alias, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_ImportAliasRedeclaration, node,
                              "redeclaration of import alias '", 
                              ctx.pool.lookup(alias), "'");
        return true;
    }
    return false;
}

} // namespace sema