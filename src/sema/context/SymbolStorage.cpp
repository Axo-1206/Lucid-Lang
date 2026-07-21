/**
 * @file SymbolStorage.cpp
 * @brief Implementation of SymbolStorage.
 */

#include "SymbolStorage.hpp"

namespace sema {

// ─── Module Management ──────────────────────────────────────────────────

void SymbolStorage::enterModule(ModuleAST* module) {
    m_currentModule = module;
    m_currentModuleTable = &getOrCreateModuleTable(module);
}

ModuleTable& SymbolStorage::getOrCreateModuleTable(ModuleAST* module) {
    auto it = m_moduleTables.find(module);
    if (it != m_moduleTables.end()) {
        return it->second;
    }

    ModuleTable& table = m_moduleTables[module];
    table.module = module;
    return table;
}

ModuleTable* SymbolStorage::findModuleTable(ModuleAST* module) {
    auto it = m_moduleTables.find(module);
    return it != m_moduleTables.end() ? &it->second : nullptr;
}

// ─── Scope Management ────────────────────────────────────────────────────

void SymbolStorage::pushScope() {
    m_scopes.push_back(Scope{});
}

void SymbolStorage::popScope() {
    if (!m_scopes.empty()) {
        m_scopes.pop_back();
    }
}

Scope& SymbolStorage::currentScope() {
    return m_scopes.back();
}

const Scope& SymbolStorage::currentScope() const {
    return m_scopes.back();
}

// ─── Insertion ──────────────────────────────────────────────────────────

void SymbolStorage::insertValue(InternedString name, ValueDeclAST* decl) {
    if (isAtModuleLevel()) {
        m_currentModuleTable->values[name] = decl;
    } else {
        currentScope().values[name] = decl;
    }
}

void SymbolStorage::insertType(InternedString name, TypeDeclAST* decl) {
    if (isAtModuleLevel()) {
        m_currentModuleTable->types[name] = decl;
    } else {
        currentScope().types[name] = decl;
    }
}

void SymbolStorage::insertGenericParam(InternedString name, GenericParamDeclAST* param) {
    assert(!isAtModuleLevel() && "insertGenericParam() requires an open Scope");
    currentScope().genericParams[name] = param;
}

// ─── Lookup ─────────────────────────────────────────────────────────────

ValueDeclAST* SymbolStorage::lookupValue(InternedString name) const {
    // Search scopes from innermost to outermost
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            return found->second;
        }
    }

    // Fall back to current module's persistent table
    if (m_currentModuleTable) {
        auto found = m_currentModuleTable->values.find(name);
        if (found != m_currentModuleTable->values.end()) {
            return found->second;
        }
    }

    return nullptr;
}

FuncDeclAST* SymbolStorage::lookupFunction(InternedString name) const {
    ValueDeclAST* v = lookupValue(name);
    return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
}

TypeDeclAST* SymbolStorage::lookupType(InternedString name) const {
    // Search scopes from innermost to outermost
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        // Generic parameters shadow type names
        auto gen = it->genericParams.find(name);
        if (gen != it->genericParams.end()) {
            // Generic parameters are not TypeDeclAST
            return nullptr;
        }

        auto found = it->types.find(name);
        if (found != it->types.end()) {
            return found->second;
        }
    }

    // Fall back to current module's persistent table
    if (m_currentModuleTable) {
        auto found = m_currentModuleTable->types.find(name);
        if (found != m_currentModuleTable->types.end()) {
            return found->second;
        }
    }

    return nullptr;
}

GenericParamDeclAST* SymbolStorage::lookupGenericParam(InternedString name) const {
    // Generic parameters are always transient, so only search scopes
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        auto found = it->genericParams.find(name);
        if (found != it->genericParams.end()) {
            return found->second;
        }
    }
    return nullptr;
}

// ─── Import Aliases ─────────────────────────────────────────────────────

void SymbolStorage::addImportAlias(InternedString alias, ModuleAST* module) {
    if (m_currentModuleTable) {
        m_currentModuleTable->importAliases[alias] = module;
    }
}

ModuleAST* SymbolStorage::lookupImport(InternedString alias) const {
    if (!m_currentModuleTable) return nullptr;
    auto it = m_currentModuleTable->importAliases.find(alias);
    return it != m_currentModuleTable->importAliases.end() ? it->second : nullptr;
}

} // namespace sema