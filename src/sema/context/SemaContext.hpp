/// @file SemaContext.hpp
/// @brief Unified semantic context - monolithic design with integrated symbol storage.
///
/// # Quick Reference
///
/// | Feature                        | Method                                    |
/// | ------------------------------ | ----------------------------------------- |
/// | Look up a value by name        | `lookupValue(name)`                       |
/// | Look up a type by name         | `lookupType(name)`                        |
/// | Look up a generic param        | `lookupGenericParam(name)`                |
/// | Insert a value declaration     | `insertValue(decl)`                       |
/// | Insert a type declaration      | `insertType(decl)`                        |
/// | Check if name is generic param | `isGenericParam(name)`                    |
/// | Self-reference check           | `isDefiningType(decl)`                    |
/// | Push type being defined        | `pushDefiningType(decl)`                  |
/// | Get current defining type      | `currentDefiningType()`                   |
///
/// # Symbol Storage Design
///
/// The symbol storage uses a two-tier model:
///   1. **ModuleTable** (persistent): One per module, holds top-level declarations
///   2. **Scope** (transient): Pushed/popped for blocks, functions, generic params
///
/// Lookup priority:
///   1. Generic parameters in current scope (highest priority)
///   2. Value/Type declarations in local scopes (innermost to outermost)
///   3. Value/Type declarations in module scope (global)

#pragma once

#include "ContextStack.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <vector>
#include <unordered_map>
#include <sstream>
#include <cassert>

namespace sema {

// ─── ModuleTable ──────────────────────────────────────────────────────────

/// @brief Persistent top-level symbol table for exactly one module.
struct ModuleTable {
    ModuleAST* module = nullptr;
    
    /// Top-level value namespace: variables, functions.
    std::unordered_map<InternedString, const ValueDeclAST*> values;
    
    /// Top-level type namespace: structs, enums, traits.
    std::unordered_map<InternedString, const TypeDeclAST*> types;
    
    /// Import aliases: alias → module.
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

// ─── Scope ────────────────────────────────────────────────────────────────

/// @brief A single transient lexical scope.
struct Scope {
    /// Value namespace: variables, functions, parameters, fields, enum variants
    std::unordered_map<InternedString, const ValueDeclAST*> values;
    
    /// Type namespace: structs, enums, traits
    std::unordered_map<InternedString, const TypeDeclAST*> types;
    
    /// Generic parameter names (shadow type lookups)
    std::unordered_map<InternedString, const GenericParamDeclAST*> genericParams;
};

// ─── SemaContext ──────────────────────────────────────────────────────────

/// @brief Unified semantic context - all in one struct.
/// 
/// This is the main context passed to all semantic analysis functions.
/// It's intentionally monolithic - all state is directly accessible.
struct SemaContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    ASTArena& arena;
    ContextStack contexts;
    
    // ─── Modules ────────────────────────────────────────────────────────
    
    std::vector<ModuleAST*> modules;
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;
    
    // ─── Symbol Storage ────────────────────────────────────────────────
    
    /// Current module being analyzed.
    ModuleAST* currentModule = nullptr;
    
    /// Pointer to the current module's table (cached for performance).
    ModuleTable* currentModuleTable = nullptr;
    
    /// Persistent per-module tables.
    std::unordered_map<ModuleAST*, ModuleTable> moduleTables;
    
    /// Transient scope stack.
    std::vector<Scope> scopes;
    
    // ─── Self-Reference Tracking ──────────────────────────────────────
    
    /// Stack of types currently being defined.
    /// 
    /// When resolving a struct's fields, we push the struct onto this stack.
    /// This allows `resolveNamedType()` to detect self-references.
    /// 
    /// @example
    ///   struct Node<T> {           // push Node
    ///       value T,
    ///       next *Node<T>?     // isDefiningType(Node) → true
    ///   }                          // pop Node
    std::vector<const TypeDeclAST*> definingTypes;
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    SemaContext(StringPool& p, ASTArena& a, std::vector<ModuleAST*> mods)
        : pool(p), arena(a), modules(std::move(mods)) {
        for (ModuleAST* m : modules) {
            if (m) modulesByPath[m->filePath] = m;
        }
    }
    
    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;
    
    // ─── Module Management ─────────────────────────────────────────────
    
    /// @brief Switch to a module, creating its table if needed.
    void enterModule(ModuleAST* module) {
        currentModule = module;
        currentModuleTable = &getOrCreateModuleTable(module);
    }
    
    /// @brief Get or create a module's persistent table.
    ModuleTable& getOrCreateModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        if (it != moduleTables.end()) {
            return it->second;
        }
        
        ModuleTable& table = moduleTables[module];
        table.module = module;
        return table;
    }
    
    /// @brief Find a module's table without creating one.
    ModuleTable* findModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        return it != moduleTables.end() ? &it->second : nullptr;
    }
    
    /// @brief Find a module by path.
    ModuleAST* findModuleByPath(InternedString path) const {
        auto it = modulesByPath.find(path);
        return it != modulesByPath.end() ? it->second : nullptr;
    }
    
    // ─── Scope Management ──────────────────────────────────────────────
    
    /// @brief True if there are no open transient scopes.
    bool isAtModuleLevel() const { return scopes.empty(); }
    
    /// @brief Push a new empty scope.
    void pushScope() { scopes.emplace_back(); }
    
    /// @brief Pop the innermost scope.
    void popScope() {
        if (!scopes.empty()) {
            scopes.pop_back();
        }
    }
    
    /// @brief Get the current (innermost) scope.
    Scope& currentScope() {
        assert(!scopes.empty() && "No scope open");
        return scopes.back();
    }
    
    const Scope& currentScope() const {
        assert(!scopes.empty() && "No scope open");
        return scopes.back();
    }
    
    // ─── Symbol Insertion ──────────────────────────────────────────────
    
    /// @brief Insert a value declaration at the current level.
    void insertValue(const ValueDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->values[decl->name] = decl;
        } else {
            currentScope().values[decl->name] = decl;
        }
    }
    
    /// @brief Insert a type declaration at the current level.
    void insertType(const TypeDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->types[decl->name] = decl;
        } else {
            currentScope().types[decl->name] = decl;
        }
    }
    
    /// @brief Insert a generic parameter into the innermost scope.
    /// @pre A scope must be open (not at module level).
    void insertGenericParam(const GenericParamDeclAST* param) {
        assert(!isAtModuleLevel() && "insertGenericParam() requires an open Scope");
        currentScope().genericParams[param->name] = param;
    }
    
    /// @brief Add an import alias to the current module.
    void addImportAlias(InternedString alias, ModuleAST* module) {
        if (currentModuleTable) {
            currentModuleTable->importAliases[alias] = module;
        }
    }
    
    // ─── Symbol Lookup ──────────────────────────────────────────────────
    
    /// @brief Look up a value declaration by name.
    /// 
    /// Searches: scopes (innermost to outermost) → current module table.
    const ValueDeclAST* lookupValue(InternedString name) const {
        // Search scopes from innermost to outermost
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->values.find(name);
            if (found != it->values.end()) {
                return found->second;
            }
        }
        
        // Fall back to current module's persistent table
        if (currentModuleTable) {
            auto found = currentModuleTable->values.find(name);
            if (found != currentModuleTable->values.end()) {
                return found->second;
            }
        }
        
        return nullptr;
    }
    
    /// @brief Look up a function by name (convenience wrapper).
    const FuncDeclAST* lookupFunction(InternedString name) const {
        const ValueDeclAST* v = lookupValue(name);
        return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
    }
    
    /// @brief Look up a type declaration by name.
    /// 
    /// Searches: scopes (innermost to outermost) → current module table.
    /// Generic parameters shadow type names in scopes.
    const TypeDeclAST* lookupType(InternedString name) const {
        // Search scopes from innermost to outermost
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            // Generic parameters shadow type names
            auto gen = it->genericParams.find(name);
            if (gen != it->genericParams.end()) {
                return nullptr; // Generic param, not a type
            }
            
            auto found = it->types.find(name);
            if (found != it->types.end()) {
                return found->second;
            }
        }
        
        // Fall back to current module's persistent table
        if (currentModuleTable) {
            auto found = currentModuleTable->types.find(name);
            if (found != currentModuleTable->types.end()) {
                return found->second;
            }
        }
        
        return nullptr;
    }
    
    /// @brief Look up a generic parameter by name.
    /// 
    /// Generic parameters are always transient, so only search scopes.
    const GenericParamDeclAST* lookupGenericParam(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->genericParams.find(name);
            if (found != it->genericParams.end()) {
                return found->second;
            }
        }
        return nullptr;
    }
    
    /// @brief Check if a name is a generic parameter.
    bool isGenericParam(InternedString name) const {
        return lookupGenericParam(name) != nullptr;
    }
    
    /// @brief Look up an import alias.
    ModuleAST* lookupImport(InternedString alias) const {
        if (!currentModuleTable) return nullptr;
        auto it = currentModuleTable->importAliases.find(alias);
        return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
    }
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    
    /// @brief Push a type that's currently being defined.
    /// 
    /// Called when we start resolving a struct/enum/trait's internals.
    /// @see ScopedTypeDefinition RAII guard
    void pushDefiningType(const TypeDeclAST* decl) {
        definingTypes.push_back(decl);
    }
    
    /// @brief Pop the current defining type.
    void popDefiningType() {
        if (!definingTypes.empty()) {
            definingTypes.pop_back();
        }
    }
    
    /// @brief Check if a type is currently being defined.
    /// 
    /// Used by `resolveNamedType()` to detect self-references.
    /// 
    /// @param decl The type declaration to check.
    /// @return true if the type is on the defining stack.
    /// 
    /// @example
    ///   // In resolveNamedType for Node<T>:
    ///   if (ctx.isDefiningType(decl)) {
    ///       // This is a self-reference!
    ///       // Check if it's wrapped in ptr/ref/nullable
    ///   }
    bool isDefiningType(const TypeDeclAST* decl) const {
        for (const TypeDeclAST* d : definingTypes) {
            if (d == decl) return true;
        }
        return false;
    }
    
    /// @brief Get the innermost type currently being defined.
    /// @return The innermost TypeDeclAST, or nullptr if none.
    const TypeDeclAST* currentDefiningType() const {
        return definingTypes.empty() ? nullptr : definingTypes.back();
    }
    
    // ─── Error Reporting ─────────────────────────────────────────────────
    
    template<typename... Args>
    void error(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(node ? node->loc : SourceLocation{}, code, {msg});
    }
    
    template<typename... Args>
    void warning(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(node ? node->loc : SourceLocation{}, code, {msg});
    }
    
    template<typename... Args>
    void note(const BaseAST* node, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(node ? node->loc : SourceLocation{}, msg);
    }
    
    bool canContinue() const {
        return diagnostic::canContinue();
    }
    
private:
    template<typename... Args>
    std::string buildMessage(Args&&... args) const {
        std::ostringstream oss;
        (oss << ... << args);
        return oss.str();
    }
};

// ─── RAII Guards ─────────────────────────────────────────────────────────

/// @brief RAII guard for semantic context tracking.
/// 
/// Pushes a ContextKind frame on construction and pops it on destruction.
/// 
/// @example
/// ```cpp
/// void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
///     ScopedSemanticContext guard(ctx, ContextKind::FuncBody, decl, decl->loc);
///     // ctx.contexts.current() now returns FuncBody
///     // return is legal inside the body
/// }
/// ```
struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind,
                          const BaseAST* node, const SourceLocation& loc)
        : ctx_(ctx) {
        ctx_.contexts.push(kind, const_cast<BaseAST*>(node), loc);
    }
    ~ScopedSemanticContext() { ctx_.contexts.pop(); }
    
    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for if condition context.
/// 
/// Enables type narrowing detection during condition analysis.
/// 
/// @example
/// ```cpp
/// void resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
///     ScopedIfCondition guard(ctx, stmt->elseBranch != nullptr);
///     // During checkExpr, ctx.contexts.isIfConditionCtx() returns true
///     // checkBinaryExpr can detect patterns like x != nil
///     resolveExpr(stmt->condition, ctx);
/// }
/// ```
struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse)
        : ctx_(ctx) {
        ctx_.contexts.setIfConditionCtx(true);
        ctx_.contexts.setHasElse(hasElse);
        ctx_.contexts.clearPendingNarrowing();
    }
    ~ScopedIfCondition() {
        ctx_.contexts.setIfConditionCtx(false);
    }
    
    ScopedIfCondition(const ScopedIfCondition&) = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for type narrowing in a branch.
/// 
/// Pushes a narrowing level for the then/else branch of an if statement.
/// 
/// @example
/// ```cpp
/// // Then branch: direct narrowing
/// ScopedNarrowing guard(ctx, varName, narrowedType, false);
/// analyzeBlock(thenBranch, ctx);
/// 
/// // Else branch: inverse narrowing
/// ScopedNarrowing guard(ctx, varName, narrowedType, true);
/// analyzeBlock(elseBranch, ctx);
/// ```
struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                    const TypeAST* narrowedType, bool isInverse = false)
        : ctx_(ctx) {
        ctx_.contexts.pushNarrowingLevel(isInverse);
        ctx_.contexts.narrowVariable(varName, narrowedType);
    }
    ~ScopedNarrowing() {
        ctx_.contexts.popNarrowingLevel();
    }
    
    ScopedNarrowing(const ScopedNarrowing&) = delete;
    ScopedNarrowing& operator=(const ScopedNarrowing&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for self-reference detection.
/// 
/// Marks a type as "currently being defined" so that self-references
/// can be detected and validated.
/// 
/// @example
/// ```cpp
/// void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
///     ScopedTypeDefinition guard(ctx, decl);
///     // ctx.isDefiningType(decl) returns true
///     // ctx.currentDefiningType() returns decl
///     resolveStructFields(decl, ctx);
/// }
/// ```
struct ScopedTypeDefinition {
    ScopedTypeDefinition(SemaContext& ctx, const TypeDeclAST* decl)
        : ctx_(ctx) {
        ctx_.pushDefiningType(decl);
    }
    ~ScopedTypeDefinition() {
        ctx_.popDefiningType();
    }
    
    ScopedTypeDefinition(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition& operator=(const ScopedTypeDefinition&) = delete;

private:
    SemaContext& ctx_;
};

} // namespace sema