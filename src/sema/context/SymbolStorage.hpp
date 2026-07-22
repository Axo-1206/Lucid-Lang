/**
 * @file SymbolStorage.hpp
 * @brief Two-tier symbol storage for the semantic phase.
 *
 * Manages:
 *   - Persistent ModuleTables (one per module, never erased)
 *   - Transient Scopes (pushed/popped for blocks, functions, etc.)
 *   - Symbol insertion and lookup with automatic tier selection
 *
 * @architectural_note Two-Tier Storage Model
 *   - ModuleTable: PERSISTENT, one per module, holds top-level names
 *   - Scope: TRANSIENT, pushed/popped for local constructs
 *
 *   `insertValue()` / `insertType()` pick the right tier automatically:
 *   - If no Scope is open → ModuleTable
 *   - Otherwise → innermost Scope
 *
 * @architectural_note Lookup Order
 *   1. Scopes from innermost to outermost
 *   2. Current module's persistent ModuleTable
 *   3. Does NOT cross into other modules automatically
 *
 * @architectural_note Const-correctness
 *   - AST nodes are read-only (const). The parser created them.
 *   - We store const pointers to read-only AST nodes.
 *   - Insertion takes const pointers, lookup returns const pointers.
 *   - The only exception is selfType which is a semantic annotation.
 */
#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"

#include <unordered_map>
#include <vector>
#include <cassert>

namespace sema {

/**
 * @brief The persistent top-level symbol table for exactly one module.
 *
 * One ModuleTable exists per ModuleAST for the lifetime of the whole
 * semantic analysis (created on first visit, never erased).
 *
 * @note Only top-level names live here. Struct fields, enum variants,
 *       and trait field requirements are reached through their owning
 *       TypeDeclAST, never through ModuleTable.
 */
struct ModuleTable {
    /// The module this table belongs to.
    ModuleAST* module = nullptr;

    /// Top-level value namespace: variables, functions.
    std::unordered_map<InternedString, const ValueDeclAST*> values;

    /// Top-level type namespace: structs, enums, traits.
    std::unordered_map<InternedString, const TypeDeclAST*> types;

    /// Import aliases declared by this module.
    /// Example: `import std.io as io` → importAliases["io"] = module_ast_for_std_io
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

/**
 * @brief A single transient lexical scope.
 *
 * Pushed on entry to any construct with local names (function body,
 * block, for-loop, generic parameter list) and popped on exit.
 * Everything contained in a Scope is discarded when popped.
 */
struct Scope {
    /// Value namespace: variables, functions, parameters, fields, enum variants
    std::unordered_map<InternedString, const ValueDeclAST*> values;

    /// Type namespace: structs, enums, traits
    std::unordered_map<InternedString, const TypeDeclAST*> types;

    /// Generic parameter names (shadow type lookups)
    std::unordered_map<InternedString, const GenericParamDeclAST*> genericParams;
};

/**
 * @brief Symbol storage manager with two-tier storage.
 *
 * Provides automatic tier selection for insertion and scoped lookup.
 */
class SymbolStorage {
public:
    // ─── Constructor ──────────────────────────────────────────────────────

    SymbolStorage() = default;

    // Non-copyable (contains references to external state)
    SymbolStorage(const SymbolStorage&) = delete;
    SymbolStorage& operator=(const SymbolStorage&) = delete;

    // Move is allowed (scopes and tables can be moved)
    SymbolStorage(SymbolStorage&&) = default;
    SymbolStorage& operator=(SymbolStorage&&) = default;

    // ─── Module Management ─────────────────────────────────────────────

    /**
     * @brief Switch the current module, creating its table if needed.
     */
    void enterModule(ModuleAST* module);

    /**
     * @brief Get this module's persistent table, creating it on first visit.
     */
    ModuleTable& getOrCreateModuleTable(ModuleAST* module);

    /**
     * @brief Look up an already-created module table without creating one.
     * @return nullptr if that module hasn't been visited yet.
     */
    ModuleTable* findModuleTable(ModuleAST* module);

    /**
     * @brief Get the current module's table.
     * @return The current ModuleTable, or nullptr if no module is entered.
     */
    ModuleTable* currentModuleTable() const { return m_currentModuleTable; }

    /**
     * @brief Get the current module.
     * @return The current ModuleAST, or nullptr if no module is entered.
     */
    ModuleAST* currentModule() const { return m_currentModule; }

    // ─── Scope Management ────────────────────────────────────────────────

    /**
     * @brief True if there is no open transient scope.
     *
     * When true, insertions go to the persistent ModuleTable.
     */
    bool isAtModuleLevel() const { return m_scopes.empty(); }

    /**
     * @brief Push a new empty scope onto the stack.
     */
    void pushScope();

    /**
     * @brief Pop the innermost scope from the stack.
     */
    void popScope();

    /**
     * @brief Get the current (innermost) scope.
     * @pre !isAtModuleLevel()
     */
    Scope& currentScope();

    /**
     * @brief Get the current (innermost) scope (const version).
     * @pre !isAtModuleLevel()
     */
    const Scope& currentScope() const;

    /**
     * @brief Get the scope stack (for saving/restoring).
     */
    const std::vector<Scope>& scopes() const { return m_scopes; }

    /**
     * @brief Set the scope stack (for restoring).
     */
    void setScopes(std::vector<Scope> scopes) { m_scopes = std::move(scopes); }

    // ─── Insertion ────────────────────────────────────────────────────────

    /**
     * @brief Insert a value declaration at the current level.
     *
     * If no scope is open: goes into currentModuleTable (persistent).
     * Otherwise: goes into the innermost Scope (transient).
     */
    void insertValue(const ValueDeclAST* decl);

    /**
     * @brief Insert a type declaration at the current level.
     *
     * Same tiering rule as insertValue().
     */
    void insertType(const TypeDeclAST* decl);

    /**
     * @brief Insert a generic parameter into the innermost open scope.
     *
     * Generic parameters are never module-level, so this always targets
     * the innermost Scope.
     * @pre !isAtModuleLevel()
     */
    void insertGenericParam(const GenericParamDeclAST* param);

    // ─── Lookup ───────────────────────────────────────────────────────────

    /**
     * @brief Look up a value: scopes then current module table.
     *
     * Does NOT cross into other modules automatically.
     * @return The ValueDeclAST if found, nullptr otherwise.
     */
    const ValueDeclAST* lookupValue(InternedString name) const;

    /**
     * @brief Look up a function: narrowed version of lookupValue().
     * @return The FuncDeclAST if found, nullptr otherwise.
     */
    const FuncDeclAST* lookupFunction(InternedString name) const;

    /**
     * @brief Look up a type: scopes then current module table.
     *
     * Generic parameters shadow type names in scopes.
     * @return The TypeDeclAST if found, nullptr otherwise.
     */
    const TypeDeclAST* lookupType(InternedString name) const;

    /**
     * @brief Look up a generic parameter in the open scope stack.
     *
     * Generic parameters are always transient, so this only searches scopes.
     * @return The GenericParamDeclAST if found, nullptr otherwise.
     */
    const GenericParamDeclAST* lookupGenericParam(InternedString name) const;

    // ─── Import Aliases ──────────────────────────────────────────────────

    /**
     * @brief Register an import alias for the current module.
     */
    void addImportAlias(InternedString alias, ModuleAST* module);

    /**
     * @brief Look up an imported module by its alias for the current module.
     */
    ModuleAST* lookupImport(InternedString alias) const;

private:
    // ─── Members ─────────────────────────────────────────────────────────

    /// Persistent per-module tables
    std::unordered_map<ModuleAST*, ModuleTable> m_moduleTables;

    /// Current module being analyzed
    ModuleAST* m_currentModule = nullptr;

    /// Pointer to the current module's table (cached for performance)
    ModuleTable* m_currentModuleTable = nullptr;

    /// Transient scope stack
    std::vector<Scope> m_scopes;
};

} // namespace sema