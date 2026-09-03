/// @file SemaContext.hpp
/// @brief Unified semantic context - monolithic design with integrated symbol storage and type cache.
///
/// This file contains ONLY declarations. All implementations are in SemaContext.cpp.
/// See SemaContext.cpp for detailed documentation.

#pragma once

#include "ContextStack.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <vector>
#include <unordered_map>
#include <cassert>

namespace sema {

// ─── Forward Declarations ──────────────────────────────────────────────────

struct ModuleTable;
struct TypeCache;
struct SemaContext;

// ─── ModuleTable ──────────────────────────────────────────────────────────

/// @brief Stores all symbols for a single module (source file).
/// 
/// Values and types are stored in separate maps to allow:
/// - `struct Point` and `let Point = 42` to coexist
/// - Faster lookup (search only the relevant namespace)
struct ModuleTable {
    ModuleAST* module = nullptr;
    std::unordered_map<InternedString, ValueDeclAST*> values;
    std::unordered_map<InternedString, TypeDeclAST*> types;
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

// ─── TypeCache ─────────────────────────────────────────────────────────────

/// @brief Canonicalization cache for type nodes.
/// 
/// Ensures semantically equivalent types share the same AST node pointer,
/// enabling fast type comparison via pointer equality.
struct TypeCache {
    // ─── Primitive Types ──────────────────────────────────────────────────
    PrimitiveTypeAST* boolType = nullptr;
    PrimitiveTypeAST* intType = nullptr;
    PrimitiveTypeAST* floatType = nullptr;
    PrimitiveTypeAST* stringType = nullptr;
    PrimitiveTypeAST* charType = nullptr;
    PrimitiveTypeAST* uint64Type = nullptr;
    UnknownTypeAST* unknownType = nullptr;
    
    // ─── Named Type Cache ──────────────────────────────────────────────────
    struct NamedTypeKey {
        InternedString name;
        ArenaSpan<TypeAST*> genericArgs;
        
        bool operator==(const NamedTypeKey& other) const;
    };
    
    struct NamedTypeKeyHash {
        size_t operator()(const NamedTypeKey& key) const;
    };
    std::unordered_map<NamedTypeKey, NamedTypeAST*, NamedTypeKeyHash> namedTypes;
    
    // ─── Array Type Cache ──────────────────────────────────────────────────
    struct ArrayTypeKey {
        ArrayKind kind;
        uint64_t size;
        TypeAST* element;
        bool operator==(const ArrayTypeKey& other) const;
    };
    struct ArrayTypeKeyHash {
        size_t operator()(const ArrayTypeKey& key) const;
    };
    std::unordered_map<ArrayTypeKey, ArrayTypeAST*, ArrayTypeKeyHash> arrayTypes;

    // ─── Pointer Type Cache ────────────────────────────────────────────────
    struct PtrTypeKey {
        TypeAST* inner;
        bool operator==(const PtrTypeKey& other) const;
    };
    struct PtrTypeKeyHash {
        size_t operator()(const PtrTypeKey& key) const;
    };
    std::unordered_map<PtrTypeKey, PtrTypeAST*, PtrTypeKeyHash> ptrTypes;
    
    // ─── Reference Type Cache ──────────────────────────────────────────────
    struct RefTypeKey {
        TypeAST* inner;
        bool operator==(const RefTypeKey& other) const;
    };
    struct RefTypeKeyHash {
        size_t operator()(const RefTypeKey& key) const;
    };
    std::unordered_map<RefTypeKey, RefTypeAST*, RefTypeKeyHash> refTypes;
};

// ─── SemaContext ──────────────────────────────────────────────────────────

/// @brief Central hub for semantic analysis.
/// 
/// Holds all state needed to resolve types, validate declarations, and
/// perform flow-sensitive analysis.
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
    std::vector<TypeDeclAST*> definingTypes;
    
    // ─── Constructor ────────────────────────────────────────────────────
    SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d);
    
    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;
    
    // ─── Module Management ─────────────────────────────────────────────
    void enterModule(ModuleAST* module);
    ModuleTable& getOrCreateModuleTable(ModuleAST* module);
    ModuleTable* findModuleTable(ModuleAST* module);
    ModuleAST* findModuleByPath(InternedString path) const;
    
    // ─── Scope Management ──────────────────────────────────────────────
    bool isAtModuleLevel() const;
    void pushScope();
    void popScope();
    Scope& currentScope();
    const Scope& currentScope() const;
    
    // ─── Scope Queries ──────────────────────────────────────────────────
    bool isInCurrentScope(InternedString name) const;
    bool isModuleMember(InternedString name) const;
    bool isTypeInCurrentScope(InternedString name) const;
    bool isModuleTypeMember(InternedString name) const;
    bool isGenericParamInCurrentScope(InternedString name) const;
    
    // ─── Symbol Insertion ──────────────────────────────────────────────
    bool insertValue(ValueDeclAST* decl);
    bool insertType(TypeDeclAST* decl);
    bool insertGenericParam(GenericParamDeclAST* param);
    bool addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node = nullptr);
    
    // ─── Symbol Lookup ──────────────────────────────────────────────────
    TypeAST* getEffectiveType(ValueDeclAST* decl, InternedString name) const;
    GenericParamDeclAST* lookupGenericParam(InternedString name) const;
    bool isGenericParam(InternedString name) const;
    ValueDeclAST* lookupValue(InternedString name) const;
    FuncDeclAST* lookupFunction(InternedString name) const;
    TypeDeclAST* lookupType(InternedString name) const;
    ModuleAST* lookupImport(InternedString alias) const;
    
    // ─── Type Lookup with Context ──────────────────────────────────────
    TypeDeclAST* lookupTypeDecl(InternedString name) const;
    bool isGenericTypeParam(InternedString name) const;
    TypeDeclAST* lookupTypeDeclWithAlias(InternedString name) const;
    
    // ─── Module Member Lookup ──────────────────────────────────────────
    ValueDeclAST* lookupModuleValueMember(ModuleAST* module, InternedString memberName) const;
    TypeDeclAST* lookupModuleTypeMember(ModuleAST* module, InternedString memberName) const;
    ValueDeclAST* lookupValueByAlias(InternedString alias, InternedString memberName) const;
    TypeDeclAST* lookupTypeByAlias(InternedString alias, InternedString memberName) const;
    
    // ─── Export Checking ──────────────────────────────────────────────
    bool isExported(DeclAST* decl) const;
    bool isTypeExported(TypeDeclAST* decl) const;
    bool isValueExported(ValueDeclAST* decl) const;
    
    // ─── Module Member Keyword Info ────────────────────────────────────
    DeclKeyword lookupModuleMemberKeyword(ModuleAST* module, InternedString memberName) const;
    bool isModuleMemberMutable(ModuleAST* module, InternedString memberName) const;
    bool isModuleMemberConst(ModuleAST* module, InternedString memberName) const;
    DeclKeyword lookupModuleMemberKeywordByAlias(InternedString alias, InternedString memberName) const;
    bool isModuleMemberMutableByAlias(InternedString alias, InternedString memberName) const;
    bool isModuleMemberConstByAlias(InternedString alias, InternedString memberName) const;
    
    // ─── Concurrency Helpers ───────────────────────────────────────────
    void addPendingAsync(InternedString name, ExprAST* call, const SourceLocation& loc);
    void addPendingSpawn(InternedString name, ExprAST* call, const SourceLocation& loc);
    bool hasPendingAsync(InternedString name) const;
    bool hasPendingSpawn(InternedString name) const;
    bool isPendingFuture(InternedString name) const;
    void resolveAsync(InternedString name);
    void resolveSpawn(InternedString name);
    std::vector<InternedString> getPendingAsyncNames() const;
    std::vector<InternedString> getPendingSpawnNames() const;
    bool hasPendingAsync() const;
    bool hasPendingSpawn() const;
    
    // ─── Type Cache Accessors ──────────────────────────────────────────
    PrimitiveTypeAST* getBoolType();
    PrimitiveTypeAST* getIntType();
    PrimitiveTypeAST* getFloatType();
    PrimitiveTypeAST* getStringType();
    PrimitiveTypeAST* getCharType();
    PrimitiveTypeAST* getUint64Type();
    UnknownTypeAST* getUnknownType();
    NamedTypeAST* getNamedType(InternedString name, const ArenaSpan<TypeAST*>& genericArgs = {});
    ArrayTypeAST* getArrayType(ArrayKind kind, uint64_t size, TypeAST* element);
    PtrTypeAST* getPtrType(TypeAST* inner);
    RefTypeAST* getRefType(TypeAST* inner);

    /// Arena is a compiler-builtin type representing a bump allocator.
    /// Bindings of this type must be declared with `const`.
    NamedTypeAST* getArenaType();

    /// ArenaDescriptor is a compiler-builtin POD type used for FFI.
    /// It has fixed layout: { base: *uint8, size: uint64 }
    /// This type is NOT literal-constructible by users.
    NamedTypeAST* getArenaDescriptorType();
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    void pushDefiningType(TypeDeclAST* decl);
    void popDefiningType();
    bool isDefiningType(TypeDeclAST* decl) const;
    TypeDeclAST* currentDefiningType() const;
    
    // ─── Closure Helpers ──────────────────────────────────────────────
    size_t getClosureDepth() const;
    bool insideNestedFunction() const;
    FuncDeclAST* getInnermostFunction() const;
    BaseAST* getInnermostFunctionNode() const;
};

// ─── RAII Guards ─────────────────────────────────────────────────────────

struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind, BaseAST* node);
    ~ScopedSemanticContext();
    
    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse);
    ~ScopedIfCondition();
    
    ScopedIfCondition(const ScopedIfCondition&) = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

struct SymbolScope {
    explicit SymbolScope(SemaContext& ctx);
    ~SymbolScope();
    
    SymbolScope(const SymbolScope&) = delete;
    SymbolScope& operator=(const SymbolScope&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                    TypeAST* narrowedType, bool isInverse = false);
    
    ScopedNarrowing(SemaContext& ctx, 
                    const std::unordered_map<InternedString, TypeAST*>& narrowings,
                    bool isInverse = false);
    
    ~ScopedNarrowing();
    
    ScopedNarrowing(const ScopedNarrowing&) = delete;
    ScopedNarrowing& operator=(const ScopedNarrowing&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedTypeDefinition {
    ScopedTypeDefinition(SemaContext& ctx, TypeDeclAST* decl);
    ~ScopedTypeDefinition();
    
    ScopedTypeDefinition(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition& operator=(const ScopedTypeDefinition&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedFunction {
    ScopedFunction(SemaContext& ctx, FuncDeclAST* decl, TypeAST* returnType);
    ScopedFunction(SemaContext& ctx, AnonFuncExprAST* expr, TypeAST* returnType);
    
    ~ScopedFunction();
    
    ScopedFunction(const ScopedFunction&) = delete;
    ScopedFunction& operator=(const ScopedFunction&) = delete;

private:
    SemaContext& ctx_;
    SymbolScope paramScope_;
};


} // namespace sema