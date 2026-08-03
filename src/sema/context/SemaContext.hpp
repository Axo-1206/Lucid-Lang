/// @file SemaContext.hpp
/// @brief Unified semantic context - monolithic design with integrated symbol storage and type cache.

#pragma once

#include "ContextStack.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <vector>
#include <unordered_map>
#include <sstream>
#include <cassert>

namespace sema {

// ─── ModuleTable ──────────────────────────────────────────────────────────

struct ModuleTable {
    ModuleAST* module = nullptr;
    std::unordered_map<InternedString, const ValueDeclAST*> values;
    std::unordered_map<InternedString, const TypeDeclAST*> types;
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

// ─── TypeCache ────────────────────────────────────────────────────────────

struct TypeCache {
    PrimitiveTypeAST* boolType = nullptr;
    PrimitiveTypeAST* intType = nullptr;
    PrimitiveTypeAST* floatType = nullptr;
    PrimitiveTypeAST* stringType = nullptr;
    PrimitiveTypeAST* charType = nullptr;
    UnknownTypeAST* unknownType = nullptr;
    
    struct NamedTypeKey {
        InternedString name;
        bool operator==(const NamedTypeKey& other) const {
            return name == other.name;
        }
    };
    struct NamedTypeKeyHash {
        size_t operator()(const NamedTypeKey& key) const {
            return std::hash<uint32_t>{}(key.name.id);
        }
    };
    std::unordered_map<NamedTypeKey, NamedTypeAST*, NamedTypeKeyHash> namedTypes;
    
    struct ArrayTypeKey {
        ArrayKind kind;
        uint64_t size;
        const TypeAST* element;
        bool operator==(const ArrayTypeKey& other) const {
            return kind == other.kind && 
                   size == other.size && 
                   element == other.element;
        }
    };
    struct ArrayTypeKeyHash {
        size_t operator()(const ArrayTypeKey& key) const {
            return std::hash<int>{}(static_cast<int>(key.kind)) ^
                   std::hash<uint64_t>{}(key.size) ^
                   std::hash<const TypeAST*>{}(key.element);
        }
    };
    std::unordered_map<ArrayTypeKey, ArrayTypeAST*, ArrayTypeKeyHash> arrayTypes;
};

// ─── SemaContext ──────────────────────────────────────────────────────────

/// @brief Unified semantic context - all in one struct.
/// 
/// This is the main context passed to all semantic analysis functions.
/// It's intentionally monolithic - all state is directly accessible.
/// 
/// @design_decision Diagnostics use DiagnosticEngine& (shared with parser)
///   The diagnostic engine is the same across parsing and semantic phases.
///   Both ParserContext and SemaContext hold references to the same
///   DiagnosticEngine instance, ensuring all diagnostics go to one place.
/// 
/// @design_decision No convenience wrappers for diagnostics
///   Use ctx.diagnostics.error() directly. This keeps the interface
///   consistent with ParserContext and avoids duplication.
struct SemaContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    ASTArena& arena;
    DiagnosticEngine& diagnostics;
    ContextStack stack;
    
    // ─── Modules ────────────────────────────────────────────────────────
    
    std::vector<ModuleAST*> modules;
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;
    
    // ─── Symbol Storage ────────────────────────────────────────────────
    
    ModuleAST* currentModule = nullptr;
    ModuleTable* currentModuleTable = nullptr;
    std::unordered_map<ModuleAST*, ModuleTable> moduleTables;
    std::vector<Scope> scopes;
    
    // ─── Type Cache ────────────────────────────────────────────────────
    
    TypeCache typeCache;
    
    // ─── Self-Reference Tracking ──────────────────────────────────────
    
    std::vector<const TypeDeclAST*> definingTypes;
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d, std::vector<ModuleAST*> mods)
        : pool(p)
        , arena(a)
        , diagnostics(d)
        , modules(std::move(mods)) {
        for (ModuleAST* m : modules) {
            if (m) modulesByPath[m->filePath] = m;
        }
    }
    
    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;
    
    // ─── Module Management ─────────────────────────────────────────────
    
    void enterModule(ModuleAST* module) {
        currentModule = module;
        currentModuleTable = &getOrCreateModuleTable(module);
    }
    
    ModuleTable& getOrCreateModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        if (it != moduleTables.end()) {
            return it->second;
        }
        ModuleTable& table = moduleTables[module];
        table.module = module;
        return table;
    }
    
    ModuleTable* findModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        return it != moduleTables.end() ? &it->second : nullptr;
    }
    
    ModuleAST* findModuleByPath(InternedString path) const {
        auto it = modulesByPath.find(path);
        return it != modulesByPath.end() ? it->second : nullptr;
    }
    
    // ─── Scope Management ──────────────────────────────────────────────
    
    bool isAtModuleLevel() const { return scopes.empty(); }
    
    void pushScope() { scopes.emplace_back(); }
    
    void popScope() {
        if (!scopes.empty()) {
            scopes.pop_back();
        }
    }
    
    Scope& currentScope() {
        assert(!scopes.empty() && "No scope open");
        return scopes.back();
    }
    
    const Scope& currentScope() const {
        assert(!scopes.empty() && "No scope open");
        return scopes.back();
    }
    
    // ─── Symbol Insertion ──────────────────────────────────────────────
    
    void insertValue(const ValueDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->values[decl->name] = decl;
        } else {
            currentScope().values[decl->name] = decl;
        }
    }
    
    void insertType(const TypeDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->types[decl->name] = decl;
        } else {
            currentScope().types[decl->name] = decl;
        }
    }
    
    void insertGenericParam(const GenericParamDeclAST* param) {
        assert(!isAtModuleLevel() && "insertGenericParam() requires an open Scope");
        currentScope().genericParams[param->name] = param;
    }
    
    void addImportAlias(InternedString alias, ModuleAST* module) {
        if (currentModuleTable) {
            currentModuleTable->importAliases[alias] = module;
        }
    }
    
    // ─── Symbol Lookup ──────────────────────────────────────────────────
    
    const ValueDeclAST* lookupValue(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->values.find(name);
            if (found != it->values.end()) {
                return found->second;
            }
        }
        if (currentModuleTable) {
            auto found = currentModuleTable->values.find(name);
            if (found != currentModuleTable->values.end()) {
                return found->second;
            }
        }
        return nullptr;
    }
    
    const FuncDeclAST* lookupFunction(InternedString name) const {
        const ValueDeclAST* v = lookupValue(name);
        return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
    }
    
    const TypeDeclAST* lookupType(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto gen = it->genericParams.find(name);
            if (gen != it->genericParams.end()) {
                return nullptr;
            }
            auto found = it->types.find(name);
            if (found != it->types.end()) {
                return found->second;
            }
        }
        if (currentModuleTable) {
            auto found = currentModuleTable->types.find(name);
            if (found != currentModuleTable->types.end()) {
                return found->second;
            }
        }
        return nullptr;
    }
    
    const GenericParamDeclAST* lookupGenericParam(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->genericParams.find(name);
            if (found != it->genericParams.end()) {
                return found->second;
            }
        }
        return nullptr;
    }
    
    bool isGenericParam(InternedString name) const {
        return lookupGenericParam(name) != nullptr;
    }
    
    ModuleAST* lookupImport(InternedString alias) const {
        if (!currentModuleTable) return nullptr;
        auto it = currentModuleTable->importAliases.find(alias);
        return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
    }
    
    // ─── Concurrency Helpers ─────────────────────────────────────────────
    
    /// @brief Add a pending async operation to the current scope.
    void addPendingAsync(InternedString name, const ExprAST* call, const SourceLocation& loc) {
        if (isAtModuleLevel()) return;
        PendingAsync pending{name, call, loc};
        currentScope().pendingAsync[name] = pending;
    }
    
    /// @brief Add a pending spawn operation to the current scope.
    void addPendingSpawn(InternedString name, const ExprAST* call, const SourceLocation& loc) {
        if (isAtModuleLevel()) return;
        PendingSpawn pending{name, call, loc};
        currentScope().pendingSpawn[name] = pending;
    }
    
    /// @brief Check if a name is a pending async operation.
    bool hasPendingAsync(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().pendingAsync.find(name) != currentScope().pendingAsync.end();
    }
    
    /// @brief Check if a name is a pending spawn operation.
    bool hasPendingSpawn(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().pendingSpawn.find(name) != currentScope().pendingSpawn.end();
    }
    
    void resolveAsync(InternedString name) {
        if (!scopes.empty()) {
            currentScope().pendingAsync.erase(name);
        }
    }
    
    /// @brief Resolve a pending spawn operation (remove it from the list).
    void resolveSpawn(InternedString name) {
        if (!scopes.empty()) {
            currentScope().pendingSpawn.erase(name);
        }
    }
    
    /// @brief Get all pending async names in the current scope.
    std::vector<InternedString> getPendingAsyncNames() const {
        std::vector<InternedString> result;
        if (!scopes.empty()) {
            for (const auto& [name, _] : currentScope().pendingAsync) {
                result.push_back(name);
            }
        }
        return result;
    }
    
    /// @brief Get all pending spawn names in the current scope.
    std::vector<InternedString> getPendingSpawnNames() const {
        std::vector<InternedString> result;
        if (!scopes.empty()) {
            for (const auto& [name, _] : currentScope().pendingSpawn) {
                result.push_back(name);
            }
        }
        return result;
    }
    
    /// @brief Check if there are any pending async operations.
    bool hasPendingAsync() const {
        return !scopes.empty() && !currentScope().pendingAsync.empty();
    }
    
    /// @brief Check if there are any pending spawn operations.
    bool hasPendingSpawn() const {
        return !scopes.empty() && !currentScope().pendingSpawn.empty();
    }
    
    // ─── Type Cache Accessors ──────────────────────────────────────────
    
    PrimitiveTypeAST* getBoolType() {
        if (!typeCache.boolType) {
            typeCache.boolType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }
        return typeCache.boolType;
    }
    
    PrimitiveTypeAST* getIntType() {
        if (!typeCache.intType) {
            typeCache.intType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Int);
        }
        return typeCache.intType;
    }
    
    PrimitiveTypeAST* getFloatType() {
        if (!typeCache.floatType) {
            typeCache.floatType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Float);
        }
        return typeCache.floatType;
    }
    
    PrimitiveTypeAST* getStringType() {
        if (!typeCache.stringType) {
            typeCache.stringType = arena.make<PrimitiveTypeAST>(PrimitiveKind::String);
        }
        return typeCache.stringType;
    }
    
    PrimitiveTypeAST* getCharType() {
        if (!typeCache.charType) {
            typeCache.charType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Char);
        }
        return typeCache.charType;
    }
    
    UnknownTypeAST* getUnknownType() {
        if (!typeCache.unknownType) {
            typeCache.unknownType = arena.make<UnknownTypeAST>();
        }
        return typeCache.unknownType;
    }
    
    NamedTypeAST* getNamedType(InternedString name) {
        TypeCache::NamedTypeKey key{name};
        auto it = typeCache.namedTypes.find(key);
        if (it != typeCache.namedTypes.end()) {
            return it->second;
        }
        NamedTypeAST* type = arena.make<NamedTypeAST>(name);
        typeCache.namedTypes[key] = type;
        return type;
    }
    
    ArrayTypeAST* getArrayType(ArrayKind kind, uint64_t size, TypeAST* element) {
        TypeCache::ArrayTypeKey key{kind, size, element};
        auto it = typeCache.arrayTypes.find(key);
        if (it != typeCache.arrayTypes.end()) {
            return it->second;
        }
        ArrayTypeAST* type = arena.make<ArrayTypeAST>(kind, size, element);
        typeCache.arrayTypes[key] = type;
        return type;
    }
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    
    void pushDefiningType(const TypeDeclAST* decl) {
        definingTypes.push_back(decl);
    }
    
    void popDefiningType() {
        if (!definingTypes.empty()) {
            definingTypes.pop_back();
        }
    }
    
    bool isDefiningType(const TypeDeclAST* decl) const {
        for (const TypeDeclAST* d : definingTypes) {
            if (d == decl) return true;
        }
        return false;
    }
    
    const TypeDeclAST* currentDefiningType() const {
        return definingTypes.empty() ? nullptr : definingTypes.back();
    }
};

// ─── RAII Guards ─────────────────────────────────────────────────────────

struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind,
                          const BaseAST* node, const SourceLocation& loc)
        : ctx_(ctx) {
        ctx_.stack.push(kind, const_cast<BaseAST*>(node), loc);
    }
    ~ScopedSemanticContext() { ctx_.stack.pop(); }
    
    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse)
        : ctx_(ctx) {
        ctx_.stack.setIfConditionCtx(true);
        ctx_.stack.setHasElse(hasElse);
        ctx_.stack.clearPendingNarrowing();
    }
    ~ScopedIfCondition() {
        ctx_.stack.setIfConditionCtx(false);
    }
    
    ScopedIfCondition(const ScopedIfCondition&) = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                    const TypeAST* narrowedType, bool isInverse = false)
        : ctx_(ctx) {
        ctx_.stack.pushNarrowingLevel(isInverse);
        ctx_.stack.narrowVariable(varName, narrowedType);
    }
    ~ScopedNarrowing() {
        ctx_.stack.popNarrowingLevel();
    }
    
    ScopedNarrowing(const ScopedNarrowing&) = delete;
    ScopedNarrowing& operator=(const ScopedNarrowing&) = delete;

private:
    SemaContext& ctx_;
};

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