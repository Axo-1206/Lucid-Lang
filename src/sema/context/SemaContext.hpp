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
    UnknownTypeAST* unknownType = nullptr;
    std::unordered_map<PrimitiveKind, PrimitiveTypeAST*> primitives;
    
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

        // ─── Nullable / Fallible / Combined Type Caches ─────────────────────
    //
    // Same rationale as ptrTypes/refTypes: canonicalize so that two
    // call sites writing `int?` produce one node, not two. The
    // instantiation cache relies on this.
    struct NullableTypeKey {
        TypeAST* inner;
        bool operator==(const NullableTypeKey& other) const { return inner == other.inner; }
    };
    struct NullableTypeKeyHash {
        size_t operator()(const NullableTypeKey& key) const {
            return std::hash<TypeAST*>{}(key.inner);
        }
    };
    std::unordered_map<NullableTypeKey, NullableTypeAST*, NullableTypeKeyHash> nullableTypes;

    struct FallibleTypeKey {
        TypeAST* inner;
        bool operator==(const FallibleTypeKey& other) const { return inner == other.inner; }
    };
    struct FallibleTypeKeyHash {
        size_t operator()(const FallibleTypeKey& key) const {
            return std::hash<TypeAST*>{}(key.inner);
        }
    };
    std::unordered_map<FallibleTypeKey, FallibleTypeAST*, FallibleTypeKeyHash> fallibleTypes;

    struct CombinedTypeKey {
        TypeAST* inner;
        bool operator==(const CombinedTypeKey& other) const { return inner == other.inner; }
    };
    struct CombinedTypeKeyHash {
        size_t operator()(const CombinedTypeKey& key) const {
            return std::hash<TypeAST*>{}(key.inner);
        }
    };
    std::unordered_map<CombinedTypeKey, CombinedTypeAST*, CombinedTypeKeyHash> combinedTypes;
};

// ─── Instantiation Cache Key ─────────────────────────────────────────────

/// @brief Key for identifying a unique instantiation.
/// 
/// Uniquely identifies an instantiation by the template declaration and
/// the concrete type arguments.
struct InstantiationKey {
    DeclAST* templateDecl;              // The generic declaration
    ArenaSpan<TypeAST*> typeArgs;       // Concrete type arguments

    bool operator==(const InstantiationKey& other) const {
        if (templateDecl != other.templateDecl) return false;
        if (typeArgs.size() != other.typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (typeArgs[i] != other.typeArgs[i]) return false;
        }
        return true;
    }
};

/// @brief Hash for InstantiationKey.
struct InstantiationKeyHash {
    size_t operator()(const InstantiationKey& key) const {
        size_t h1 = std::hash<DeclAST*>{}(key.templateDecl);
        size_t h2 = 0;
        for (size_t i = 0; i < key.typeArgs.size(); ++i) {
            h2 ^= std::hash<TypeAST*>{}(key.typeArgs[i]) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
        }
        return h1 ^ (h2 << 1);
    }
};

// ─── Generic Type Instantiations ────────────────────────────────────────
// Structural identity map for concrete instantiations of generic *types*
// (structs — the only generic type in Lucid; enums are not generic, and
// type aliases were deliberately rejected by the language design, so this
// map has exactly one value shape: StructDeclAST*).
//
// Distinct from `instantiationCache` above:
//   instantiationCache     = "have we started building this?" (recursion guard)
//                            keyed on (templateDecl*, typeArgs) pointer identity
//   genericTypeInstantiations = "what IS Box<int>?" (semantic identity)
//                            keyed on (source name, canonical args) structural identity
//
// This is what makes two `NamedTypeAST("Box", [int])` nodes from different
// call sites compare equal: they share the same `resolvedDecl`.

struct GenericTypeKey {
    InternedString name;          // "Box" — source name, NOT mangled
    ArenaSpan<TypeAST*> args;     // canonicalized args

    bool operator==(const GenericTypeKey& other) const {
        if (name != other.name) return false;
        if (args.size() != other.args.size()) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] != other.args[i]) return false;
        }
        return true;
    }
};

struct GenericTypeKeyHash {
    size_t operator()(const GenericTypeKey& key) const {
        size_t h = std::hash<uint32_t>{}(key.name.id);
        for (TypeAST* arg : key.args) {
            h ^= std::hash<TypeAST*>{}(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
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

    // ─── Instantiation Cache ──────────────────────────────────────────────
    /// @brief Cache for instantiated declarations.
    /// 
    /// Tracks both in-progress and completed instantiations to:
    ///   1. Prevent infinite recursion for self-referential types
    ///   2. Deduplicate identical instantiations
    /// 
    /// Key: (templateDecl, typeArgs) → instantiated DeclAST*
    /// 
    /// All generic instantiations follow the specialization-only path: the cache
    /// stores the specialized StructDeclAST/FuncDeclAST shell or finalized decl.
    /// 
    /// IMPORTANT: A declaration is inserted BEFORE its fields are substituted,
    /// breaking recursive cycles. This is the "register before recursing" pattern.
    std::unordered_map<InstantiationKey, DeclAST*, InstantiationKeyHash> instantiationCache;

    /// @brief Check if an instantiation is already in progress or completed.
    bool hasInstantiation(DeclAST* templateDecl, const ArenaSpan<TypeAST*>& typeArgs) const {
        InstantiationKey key{templateDecl, typeArgs};
        return instantiationCache.find(key) != instantiationCache.end();
    }

    /// @brief Get a cached instantiation.
    DeclAST* getInstantiation(DeclAST* templateDecl, const ArenaSpan<TypeAST*>& typeArgs) const {
        InstantiationKey key{templateDecl, typeArgs};
        auto it = instantiationCache.find(key);
        return it != instantiationCache.end() ? it->second : nullptr;
    }

    /// @brief Register an instantiation BEFORE filling its fields.
    /// 
    /// Registers a shell declaration that will be filled later. This breaks
    /// recursive cycles during specialization.
    void registerInstantiation(DeclAST* templateDecl, const ArenaSpan<TypeAST*>& typeArgs, DeclAST* instantiated) {
        InstantiationKey key{templateDecl, typeArgs};
        instantiationCache[key] = instantiated;
    }

    // ─── Generic Type Instantiations ────────────────────────────────────────

    std::unordered_map<GenericTypeKey, StructDeclAST*, GenericTypeKeyHash> genericTypeInstantiations;

    // ─── Lookup / Insertion Helpers ─────────────────────────────────────────

    /// @brief Check if a concrete generic type instantiation already exists.
    /// Key is (source name, canonicalized args). Caller must canonicalize
    /// args before calling — see `canonicalizeTypeArg` in Instantiation.cpp.
    StructDeclAST* getGenericTypeInstantiation(InternedString name, const ArenaSpan<TypeAST*>& canonicalArgs) const;

    /// @brief Register a completed generic type instantiation.
    /// Called from `finalizeInstantiatedStruct` AFTER the struct is fully
    /// resolved, so the next `Box<int>` at a different call site finds it.
    void registerGenericTypeInstantiation(InternedString name, const ArenaSpan<TypeAST*>& canonicalArgs, StructDeclAST* instantiated);
    
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
    PrimitiveTypeAST* getUint8Type();
    UnknownTypeAST* getUnknownType();
    NamedTypeAST* getNamedType(InternedString name, const ArenaSpan<TypeAST*>& genericArgs = {});
    ArrayTypeAST* getArrayType(ArrayKind kind, uint64_t size, TypeAST* element);
    PtrTypeAST* getPtrType(TypeAST* inner);
    RefTypeAST* getRefType(TypeAST* inner);
    NullableTypeAST* getNullableType(TypeAST* inner);
    FallibleTypeAST* getFallibleType(TypeAST* inner);
    CombinedTypeAST* getCombinedType(TypeAST* inner);

    // ─── Primitive Type Canonicalization ──────────────────────────────
    //
    // Returns the canonical singleton node for the given primitive kind.
    // Every call site that produces a `PrimitiveTypeAST` (typically the
    // parser) yields a distinct node; this accessor ensures that two
    // same-kind primitives written at different source locations map to
    // one node in the type cache.
    //
    // Why this matters for instantiation: the instantiation cache is
    // keyed on `(templateDecl, typeArgs)` with pointer identity on each
    // type argument. Without canonicalization, `factorial<int>` written
    // twice produces two distinct `int` nodes, two distinct keys, and
    // two separate specializations. With canonicalization, both keys
    // are `(factorial, [the-int-singleton])` and the second lookup
    // hits the cache.
    PrimitiveTypeAST* getPrimitiveType(PrimitiveKind kind);

    /// Arena is a compiler-builtin type representing a bump allocator.
    /// Bindings of this type must be declared with `const`.
    NamedTypeAST* getArenaType();

    /// ArenaDescriptor is a compiler-builtin POD type used for FFI.
    /// It has fixed layout: { base: *uint8, size: uint64 }
    /// This type is NOT literal-constructible by users.
    NamedTypeAST* getArenaDescriptorType();
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    bool isDefiningType(TypeDeclAST* decl) const;
    TypeDeclAST* currentDefiningType() const;
    
    // ─── Closure Helpers ──────────────────────────────────────────────
    size_t getClosureDepth() const;
    bool insideNestedFunction() const;

    // ─── Other Helpers ─────────────────────────────────────────────────

    /// @brief Parse a compile-time integer constant.
    int64_t parseConstantInt(ExprAST* expr) {
        if (!expr->isa<LiteralExprAST>()) return 0;
        LiteralExprAST* lit = expr->as<LiteralExprAST>();
        try {
            std::string valStr = pool.lookup(lit->value);
            return std::stoll(valStr, nullptr, 0);
        } catch (const std::exception& e) {
            return 0;
        }
    }
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
    ScopedFunction(SemaContext& ctx, AnonFuncExprAST* expr, TypeAST* returnType);
    
    ~ScopedFunction();
    
    ScopedFunction(const ScopedFunction&) = delete;
    ScopedFunction& operator=(const ScopedFunction&) = delete;

private:
    SemaContext& ctx_;
    SymbolScope paramScope_;
};


} // namespace sema