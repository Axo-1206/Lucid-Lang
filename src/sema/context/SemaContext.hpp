/// @file SemaContext.hpp
/// @brief Unified semantic context - monolithic design with integrated symbol storage and type cache.
/// # ContextStack Integration
///
/// The ContextStack is embedded in SemaContext and provides:
///
/// ## 1. Context Validation
///
/// ```cpp
/// // Check if we're in a valid context for a statement
/// if (!ctx.stack.insideFunction()) {
///     ctx.diagnostics.error(...);
///     return false;
/// }
/// ```
///
/// ## 2. Type Narrowing
///
/// ```cpp
/// // Narrow a variable from Future<T> to T
/// ctx.stack.narrowVariable(targetName, innerType);
///
/// // Look up a variable's declaration (never narrowed — decl->type is
/// // always the parser-written type, immutable, no inference)
/// ValueDeclAST* decl = ctx.lookupValue(name);
///
/// // Get the type that actually applies at this point in control flow
/// // (narrowed type if narrowing is active, decl->type otherwise)
/// TypeAST* effective = ctx.getEffectiveType(decl, name);
/// ```
///
/// ## 3. Return Type Validation
///
/// ```cpp
/// // Get the expected return type for the current function
/// TypeAST* expected = ctx.stack.currentReturnType();
///
/// // Validate return value against expected type
/// TypeAST* valueType = resolveExprWithTarget(returnValue, expected, ctx);
/// ```
///
/// ## 4. Pending Narrowing for Standalone If
///
/// ```cpp
/// // In resolveIfStmt - store inverse narrowing for later
/// if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
///     ctx.stack.setPendingInverseNarrowing(info);
/// }
///
/// // In resolveBlock - apply pending inverse narrowing
/// if (ctx.stack.hasPendingInverseNarrowing()) {
///     const NarrowingInfo& info = ctx.stack.getPendingInverseNarrowing();
///     for (const auto& [varName, narrowedType] : info.narrowings) {
///         ctx.stack.narrowVariable(varName, narrowedType);
///     }
///     ctx.stack.clearPendingInverseNarrowing();
/// }
/// ```
///
/// ## 5. Concurrency Tracking
///
/// ```cpp
/// // Register pending async
/// ctx.addPendingAsync(binding->name, call, loc);
///
/// // Check and resolve async
/// if (ctx.hasPendingAsync(targetName)) {
///     ctx.resolveAsync(targetName);
///     ctx.stack.narrowVariable(targetName, innerType);
/// }
///
/// // Warn about unawaited async at block exit
/// for (InternedString name : ctx.getPendingAsyncNames()) {
///     ctx.diagnostics.warning(DiagCode::Warn_UnawaitedAsync, ...);
/// }
/// ```
///
/// # The Narrowing Flow (Full Example)
///
/// ```lucid
/// let x int? = getValue()
///
/// if x != nil {           ← 1. Condition analyzed
///     // x is int         ← 2. Direct narrowing applied
///     use(x)              ← 3. getEffectiveType() returns the narrowed type
/// } else {
///     // x is nil         ← 4. Inverse narrowing in else branch
///     handleNil()
/// }
/// // x is int?            ← 5. Narrowing level popped
///
/// if x == nil { return }  ← 6. Standalone if with early exit
/// // x is int             ← 7. Pending inverse narrowing applied to block
/// use(x)                  ← 8. getEffectiveType() returns the narrowed type
/// ```
///
/// # The Narrowing Stack Structure
///
/// ```
/// ┌────────────────────────────────────────────────────────────────────┐
/// │                          Narrowing Stack                           │
/// │                                                                    │
/// │  Level 3 (innermost)  ──────────────────────────────┐              │
/// │  { x: int, y: string }                              │              │
/// │                                                     │              │
/// │  Level 2              ──────────────────────────────│──┐           │
/// │  { x: int? }                                        │  │           │
/// │                                                     │  │           │
/// │  Level 1              ──────────────────────────────│──│──┐        │
/// │  { }                                                │  │  │        │
/// │                                                     │  │  │        │
/// │                                                     │  │  │        │
/// │  Lookup "x" ────────────────────────────────────────┘  │  │        │
/// │    → Level 3 has x → returns int                       │  │        │
/// │                                                        │  │        │
/// │  Lookup "y" ───────────────────────────────────────────┘  │        │
/// │    → Level 3 has y → returns string                       │        │
/// │                                                           │        │
/// │  Lookup "z" ──────────────────────────────────────────────┘        │
/// │    → No level has z → returns nullptr                              │
/// └────────────────────────────────────────────────────────────────────┘
/// ```

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

/// # ModuleTable
///
/// The ModuleTable stores all symbols for a single module (source file).
/// It serves as the backing store for module-level symbol lookup.
///
/// ## Structure
///
/// ```
/// ┌────────────────────────────────────────────────────────────────────────────┐
/// │                         ModuleTable                                        │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │  values:  std::unordered_map<InternedString, ValueDeclAST*>         │   │
/// │  │  ┌────────────────────────────────────────────────────────────────┐ │   │
/// │  │  │  "add"     → FuncDeclAST (function)                            │ │   │
/// │  │  │  "PI"      → VarDeclAST (const variable)                       │ │   │
/// │  │  │  "result"  → VarDeclAST (let variable)                         │ │   │
/// │  │  │  "North"   → EnumVariantAST (enum variant)                     │ │   │
/// │  │  └────────────────────────────────────────────────────────────────┘ │   │
/// │  │                                                                     │   │
/// │  │  types:   std::unordered_map<InternedString, TypeDeclAST*>          │   │
/// │  │  ┌────────────────────────────────────────────────────────────────┐ │   │
/// │  │  │  "Vec2"    → StructDeclAST                                     │ │   │
/// │  │  │  "Color"   → EnumDeclAST                                       │ │   │
/// │  │  │  "Named"   → TraitDeclAST                                      │ │   │
/// │  │  └────────────────────────────────────────────────────────────────┘ │   │
/// │  │                                                                     │   │
/// │  │  importAliases: std::unordered_map<InternedString, ModuleAST*>      │   │
/// │  │  ┌────────────────────────────────────────────────────────────────┐ │   │
/// │  │  │  "math"   → ModuleAST for "std/math.luc"                       │ │   │
/// │  │  │  "io"     → ModuleAST for "std/io.luc"                         │ │   │
/// │  │  └────────────────────────────────────────────────────────────────┘ │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// └────────────────────────────────────────────────────────────────────────────┘
/// ```
///
/// ## Lifetime
///
/// A ModuleTable is created when `enterModule()` is called and persists
/// for the entire semantic analysis of that module. It is stored in
/// `SemaContext::moduleTables` keyed by the ModuleAST pointer.
///
/// ## Lookup Priority
///
/// When looking up a symbol, the search order is:
/// 1. **Local scopes** (`SemaContext::scopes`) - innermost first
/// 2. **Module table** (`currentModuleTable`) - current module's symbols
/// 3. **Import aliases** - symbols from imported modules (via `lookupImport()`)
///
/// ## Symbol Namespaces
///
/// Values and types are stored in separate maps to allow:
/// - `struct Point` and `let Point = 42` to coexist
/// - Faster lookup (search only the relevant namespace)
/// - Clearer error messages ("undefined variable" vs "undefined type")
struct ModuleTable {
    ModuleAST* module = nullptr;
    std::unordered_map<InternedString, ValueDeclAST*> values;
    std::unordered_map<InternedString, TypeDeclAST*> types;
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

/// # TypeCache
///
/// The TypeCache is a **canonicalization cache** for type nodes. It ensures
/// that semantically equivalent types share the same AST node pointer,
/// enabling fast type comparison via pointer equality (`typesEqual()`).
///
/// ## Why Canonicalization?
///
/// Without canonicalization, two identical type annotations would produce
/// two different AST nodes:
///
/// ```cpp
/// // Both parse to different NamedTypeAST nodes
/// let x Vec2 = ...
/// let y Vec2 = ...
/// // typesEqual(x->type, y->type) would need deep structural comparison
/// ```
///
/// With canonicalization:
/// ```cpp
/// // Both resolve to the SAME NamedTypeAST pointer
/// let x Vec2 = ...  // → ctx.getNamedType("Vec2")
/// let y Vec2 = ...  // → ctx.getNamedType("Vec2") returns cached pointer
/// // typesEqual(x->type, y->type) → pointer equality ✅
/// ```
///
/// ## Cached Types
///
/// | Cache         | Key                       | Value               | Notes               |
/// | ------------- | ------------------------- | ------------------- | ------------------- |
/// | `boolType`    | N/A                       | `PrimitiveTypeAST*` | Built-in bool       |
/// | `intType`     | N/A                       | `PrimitiveTypeAST*` | Built-in int        |
/// | `floatType`   | N/A                       | `PrimitiveTypeAST*` | Built-in float      |
/// | `stringType`  | N/A                       | `PrimitiveTypeAST*` | Built-in string     |
/// | `charType`    | N/A                       | `PrimitiveTypeAST*` | Built-in char       |
/// | `unknownType` | N/A                       | `UnknownTypeAST*`   | Error recovery type |
/// | `namedTypes`  | `{ name, genericArgs }`   | `NamedTypeAST*`     | User-defined types  |
/// | `arrayTypes`  | `{ kind, size, element }` | `ArrayTypeAST*`     | Array types         |
/// | `ptrTypes`    | `{ inner }`               | `PtrTypeAST*`       | Pointer types       |
/// | `refTypes`    | `{ inner }`               | `RefTypeAST*`       | Reference types     |
///
/// ## Example: Type Cache in Action
///
/// ```lucid
/// struct Box<T> { value T }              // T is a generic param
/// const add (a int)(b int) -> int = ...  // int is cached
/// const process (data string) = ...      // string is cached
/// let items [4]Vec2 = ...                // [4]Vec2 is cached
/// ```
///
/// ## Usage
///
/// ```cpp
/// // Get a cached primitive type
/// PrimitiveTypeAST* intType = ctx.getIntType();
///
/// // Get a cached named type
/// NamedTypeAST* vec2Type = ctx.getNamedType(pool.intern("Vec2"));
///
/// // Get a cached array type
/// ArrayTypeAST* arrayType = ctx.getArrayType(ArrayKind::Fixed, 4, vec2Type);
///
/// // Check if two types are equal (fast pointer comparison)
/// if (typesEqual(typeA, typeB)) { ... }
/// ```
///
/// ## Lifetime
///
/// All types in the cache are arena-allocated and live for the entire
/// compilation session. The cache is populated lazily as types are resolved.
/// # TypeCache
///
/// ════════════════════════════════════════════════════════════════════════════
/// WHY GENERIC ARGUMENTS MUST BE IN THE CACHE KEY
/// ════════════════════════════════════════════════════════════════════════════
///
/// Without generic arguments in the key, `Vec2<int>` and `Vec2<float>` both
/// hash to the SAME `NamedTypeAST*`. This causes silent type corruption:
///   - Module A resolves Vec2<int> → stores genericArgs = [int]
///   - Module B resolves Vec2<float> → returns same node, overwrites to [float]
///   - Module A later sees Vec2<int> with genericArgs = [float] → ❌ WRONG
///
/// Including genericArgs in the key ensures each instantiation gets its
/// own canonical node, preventing cross-contamination.
struct TypeCache {
    PrimitiveTypeAST* boolType = nullptr;
    PrimitiveTypeAST* intType = nullptr;
    PrimitiveTypeAST* floatType = nullptr;
    PrimitiveTypeAST* stringType = nullptr;
    PrimitiveTypeAST* charType = nullptr;
    UnknownTypeAST* unknownType = nullptr;
    
    // ─── Named Type Cache (key includes generic arguments) ────────────────
    struct NamedTypeKey {
        InternedString name;
        ArenaSpan<TypeAST*> genericArgs;
        
        bool operator==(const NamedTypeKey& other) const {
            if (name != other.name) return false;
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (genericArgs[i] != other.genericArgs[i]) return false;
            }
            return true;
        }
    };
    
    struct NamedTypeKeyHash {
        size_t operator()(const NamedTypeKey& key) const {
            size_t h = std::hash<uint32_t>{}(key.name.id);
            for (TypeAST* arg : key.genericArgs) {
                h ^= std::hash<TypeAST*>{}(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
            }
            return h;
        }
    };
    std::unordered_map<NamedTypeKey, NamedTypeAST*, NamedTypeKeyHash> namedTypes;
    
    // ─── Array Type Cache ──────────────────────────────────────────────────
    struct ArrayTypeKey {
        ArrayKind kind;
        uint64_t size;
        TypeAST* element;
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
                   std::hash<TypeAST*>{}(key.element);
        }
    };
    std::unordered_map<ArrayTypeKey, ArrayTypeAST*, ArrayTypeKeyHash> arrayTypes;

    // ─── Pointer Type Cache ──────────────────────────────────────────────────
    struct PtrTypeKey {
        TypeAST* inner;
        bool operator==(const PtrTypeKey& other) const {
            return inner == other.inner;
        }
    };
    struct PtrTypeKeyHash {
        size_t operator()(const PtrTypeKey& key) const {
            return std::hash<TypeAST*>{}(key.inner);
        }
    };
    std::unordered_map<PtrTypeKey, PtrTypeAST*, PtrTypeKeyHash> ptrTypes;
    
    // ─── Reference Type Cache ──────────────────────────────────────────────
    struct RefTypeKey {
        TypeAST* inner;
        bool operator==(const RefTypeKey& other) const {
            return inner == other.inner;
        }
    };
    struct RefTypeKeyHash {
        size_t operator()(const RefTypeKey& key) const {
            return std::hash<TypeAST*>{}(key.inner);
        }
    };
    std::unordered_map<RefTypeKey, RefTypeAST*, RefTypeKeyHash> refTypes;
};

/// # SemaContext
///
/// The SemaContext is the **central hub** for semantic analysis. It holds
/// all state needed to resolve types, validate declarations, and perform
/// flow-sensitive analysis.
///
/// ## Architecture Overview
///
/// ```
/// ┌────────────────────────────────────────────────────────────────────────────┐
/// │                           SemaContext                                      │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                        Shared Resources                             │   │
/// │  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────────┐  │   │
/// │  │  │ StringPool  │  │ ASTArena    │  │  DiagnosticEngine           │  │   │
/// │  │  │ (interned   │  │ (memory     │  │  (error reporting)          │  │   │
/// │  │  │  strings)   │  │ allocation) │  │                             │  │   │
/// │  │  └─────────────┘  └─────────────┘  └─────────────────────────────┘  │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                        State Management                             │   │
/// │  │  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────────┐  │   │
/// │  │  │ ContextStack │  │ ModuleTables │  │ TypeCache                 │  │   │
/// │  │  │ (context,    │  │ (module      │  │ (type canonicalization)   │  │   │
/// │  │  │  narrowing)  │  │  symbols)    │  │                           │  │   │
/// │  │  └──────────────┘  └──────────────┘  └───────────────────────────┘  │   │
/// │  │  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────────┐  │   │
/// │  │  │ Scopes Stack │  │ DefiningTypes│  │ Pending Concurrency       │  │   │
/// │  │  │ (local       │  │ (self-ref    │  │ (async/spawn tracking)    │  │   │
/// │  │  │  symbols)    │  │  detection)  │  │                           │  │   │
/// │  │  └──────────────┘  └──────────────┘  └───────────────────────────┘  │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// └────────────────────────────────────────────────────────────────────────────┘
/// ```
///
/// ## Key Responsibilities
///
/// ### 1. Symbol Management
///
/// | Responsibility       | Methods                                                 |
/// | -------------------- | ------------------------------------------------------- |
/// | Insert symbols       | `insertValue()`, `insertType()`, `insertGenericParam()` |
/// | Lookup symbols       | `lookupValue()`, `lookupType()`, `lookupGenericParam()` |
/// | Scope management     | `pushScope()`, `popScope()`, `isAtModuleLevel()`        |
/// | Module symbol lookup | `lookupModuleValueMember()`, `lookupTypeByAlias()`      |
///
/// ### 2. Type Management
///
/// | Responsibility      | Methods                                                  |
/// | ------------------- | -------------------------------------------------------- |
/// | Get primitive types | `getBoolType()`, `getIntType()`, `getStringType()`, etc. |
/// | Get named types     | `getNamedType()` (canonicalization cache)                |
/// | Get array types     | `getArrayType()` (canonicalization cache)                |
/// | Error recovery      | `getUnknownType()`                                       |
///
/// ### 3. Context Management
///
/// | Responsibility       | Methods                                              |
/// | -------------------- | ---------------------------------------------------- |
/// | Context queries      | `stack.insideFunction()`, `stack.insideLoop()`, etc. |
/// | Type narrowing       | `stack.narrowVariable()`, `stack.getNarrowedType()`  |
/// | Return type tracking | `stack.currentReturnType()`                          |
///
/// ### 4. Concurrency Tracking
///
/// | Responsibility       | Methods                                            |
/// | -------------------- | -------------------------------------------------- |
/// | Register pending ops | `addPendingAsync()`, `addPendingSpawn()`           |
/// | Check pending ops    | `hasPendingAsync()`, `hasPendingSpawn()`           |
/// | Resolve pending ops  | `resolveAsync()`, `resolveSpawn()`                 |
/// | Get pending names    | `getPendingAsyncNames()`, `getPendingSpawnNames()` |
///
/// ### 5. Self-Reference Detection
///
/// | Responsibility      | Methods                                     |
/// | ------------------- | ------------------------------------------- |
/// | Track defining type | `pushDefiningType()`, `popDefiningType()`   |
/// | Check if defining   | `isDefiningType()`, `currentDefiningType()` |
///
/// ## Symbol Lookup Algorithm
///
/// ```
/// lookupValue(name)
///   │
///   ├─► Check local scopes (innermost to outermost)
///   │    └─► If found, return declaration (decl->type is always the
///   │        parser-written type — never mutated, never narrowed here)
///   │
///   ├─► Check module table (current module)
///   │    └─► If found, return declaration
///   │
///   └─► Check import aliases (via lookupImport)
///        └─► If found, return declaration from imported module
///
/// getEffectiveType(decl, name)   ← call this separately when the
///   │                              currently-applicable type is needed
///   ├─► Check narrowing stack (getNarrowedType)
///   │    └─► If found, return the narrowed type
///   │
///   └─► Otherwise, return decl->type
///
/// Note: lookupType() follows lookupValue's pattern but searches the
///       type namespace instead of the value namespace. Narrowing never
///       applies to lookupType() — only value bindings narrow.
/// ```
///
/// ## Usage Example
///
/// ```cpp
/// // Create the context
/// SemaContext ctx(pool, arena, diagnostics, modules);
///
/// // Enter a module
/// ctx.enterModule(module);
///
/// // Push a scope for the function body
/// SymbolScope scope(ctx);
///
/// // Insert a local variable
/// ctx.insertValue(varDecl);
///
/// // Check if we're in a function
/// if (ctx.stack.insideFunction()) {
///     // Validate return type
///     TypeAST* expected = ctx.stack.currentReturnType();
///     TypeAST* valueType = resolveExprWithTarget(returnValue, expected, ctx);
/// }
///
/// // Look up a variable's declaration, then its currently-applicable type
/// ValueDeclAST* decl = ctx.lookupValue(name);
/// TypeAST* effective = ctx.getEffectiveType(decl, name);
/// ```
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
    
    SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d)
        : pool(p)
        , arena(a)
        , diagnostics(d) {
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
    
    // ─── Scope Queries ──────────────────────────────────────────────────
    
    /// @brief Check if a name is defined in the current scope.
    /// This includes local variables and parameters, but NOT outer scopes.
    bool isInCurrentScope(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().values.find(name) != currentScope().values.end();
    }
    
    /// @brief Check if a name is a module member (top-level declaration).
    bool isModuleMember(InternedString name) const {
        if (!currentModuleTable) return false;
        return currentModuleTable->values.find(name) != currentModuleTable->values.end();
    }
    
    /// @brief Check if a type is defined in the current scope.
    bool isTypeInCurrentScope(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().types.find(name) != currentScope().types.end();
    }
    
    /// @brief Check if a type is a module type member.
    bool isModuleTypeMember(InternedString name) const {
        if (!currentModuleTable) return false;
        return currentModuleTable->types.find(name) != currentModuleTable->types.end();
    }
    
    /// @brief Check if a generic parameter is in the current scope.
    bool isGenericParamInCurrentScope(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().genericParams.find(name) != currentScope().genericParams.end();
    }
    
    // ─── Symbol Insertion ──────────────────────────────────────────────
    
    bool insertValue(ValueDeclAST* decl) {
        if (isAtModuleLevel()) {
            if (currentModuleTable->values.find(decl->name) != currentModuleTable->values.end()) {
                diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                                  "redeclaration of '", pool.lookup(decl->name), 
                                  "' in the same scope");
                return false;
            }
            currentModuleTable->values[decl->name] = decl;
            return true;
        } else {
            if (currentScope().values.find(decl->name) != currentScope().values.end()) {
                diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                                  "redeclaration of '", pool.lookup(decl->name), 
                                  "' in the same scope");
                return false;
            }
            currentScope().values[decl->name] = decl;
            return true;
        }
    }
    
    bool insertType(TypeDeclAST* decl) {
        if (isAtModuleLevel()) {
            if (currentModuleTable->types.find(decl->name) != currentModuleTable->types.end()) {
                diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                                  "redeclaration of '", pool.lookup(decl->name), 
                                  "' in the same scope");
                return false;
            }
            currentModuleTable->types[decl->name] = decl;
            return true;
        } else {
            if (currentScope().types.find(decl->name) != currentScope().types.end()) {
                diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                                  "redeclaration of '", pool.lookup(decl->name), 
                                  "' in the same scope");
                return false;
            }
            currentScope().types[decl->name] = decl;
            return true;
        }
    }
    
    bool insertGenericParam(GenericParamDeclAST* param) {
        assert(!isAtModuleLevel() && "insertGenericParam() requires an open Scope");
        if (currentScope().genericParams.find(param->name) != currentScope().genericParams.end()) {
            diagnostics.error(DiagCode::Sem_GenericParamRedeclaration, param,
                              "redeclaration of generic parameter '", 
                              pool.lookup(param->name), "' in the same scope");
            return false;
        }
        currentScope().genericParams[param->name] = param;
        return true;
    }
    
    bool addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node = nullptr) {
        if (!currentModuleTable) return false;
        if (currentModuleTable->importAliases.find(alias) != currentModuleTable->importAliases.end()) {
            diagnostics.error(DiagCode::Sem_ImportAliasRedeclaration, node ? node : module,
                              "redeclaration of import alias '", 
                              pool.lookup(alias), "'");
            return false;
        }
        currentModuleTable->importAliases[alias] = module;
        return true;
    }
    
    // ─── Symbol Lookup ──────────────────────────────────────────────────
    
    /// @brief Get the currently-applicable type for a declaration, accounting for narrowing.
    /// 
    /// This is the key function for flow-sensitive type narrowing. It checks
    /// the narrowing stack first, and only falls back to decl->type if no
    /// narrowing is active.
    /// 
    /// @param decl The declaration (may be null).
    /// @param name The name of the variable (used to look up narrowing state).
    /// @return The currently-applicable type, or nullptr if decl is null.
    TypeAST* getEffectiveType(ValueDeclAST* decl, InternedString name) const {
        if (!decl) return nullptr;
        
        // Check if there's a narrowed type for this variable
        TypeAST* narrowedType = stack.getNarrowedType(name);
        if (narrowedType) {
            // We need to cast away const because the AST uses mutable fields
            // This is safe because the narrowing type is stored in the stack,
            // not in the AST node itself.
            return narrowedType;
        }
        
        // decl->type is the single source of truth for a declaration's type:
        // parser-locked, no inference, not mutated by narrowing.
        return decl->type;
    }
    
    GenericParamDeclAST* lookupGenericParam(InternedString name) const {
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
    
    /// Looks up a value declaration by name: local scopes (innermost to
    /// outermost), then the current module's table, then module-level
    /// values. Returns the raw declaration, unmodified — decl->type is
    /// always the parser-written type as declared, this function never
    /// mutates it. Callers that need the *currently applicable* type
    /// (accounting for narrowing, e.g. T? narrowed to T, or Future<T>
    /// narrowed to T after await) must call getEffectiveType(decl, name)
    /// separately, above — narrowing is tracked entirely on the
    /// ContextStack's narrowing stack, never by writing back into the
    /// declaration itself.
    ValueDeclAST* lookupValue(InternedString name) const {
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
    
    FuncDeclAST* lookupFunction(InternedString name) const {
        ValueDeclAST* v = lookupValue(name);
        return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
    }
    
    TypeDeclAST* lookupType(InternedString name) const {
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
    
    ModuleAST* lookupImport(InternedString alias) const {
        if (!currentModuleTable) return nullptr;
        auto it = currentModuleTable->importAliases.find(alias);
        return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
    }

    // ─── Type Lookup with Context ──────────────────────────────────────────

    /// @brief Look up a type by name, returning the declaration.
    /// 
    /// This searches local scopes (innermost to outermost), then the module table.
    /// For generic parameters, it returns the GenericParamDeclAST (which is a
    /// TypeDeclAST) so that callers can distinguish between concrete types and
    /// generic parameters.
    TypeDeclAST* lookupTypeDecl(InternedString name) const {
        // Check local scopes first (innermost to outermost)
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            // Check generic params first - they shadow types
            auto gen = it->genericParams.find(name);
            if (gen != it->genericParams.end()) {
                return gen->second;  // GenericParamDeclAST inherits from TypeDeclAST
            }
            // Then check local types
            auto found = it->types.find(name);
            if (found != it->types.end()) {
                return found->second;
            }
        }
        
        // Check module table
        if (currentModuleTable) {
            auto found = currentModuleTable->types.find(name);
            if (found != currentModuleTable->types.end()) {
                return found->second;
            }
        }
        
        return nullptr;
    }

    /// @brief Check if a name resolves to a generic type parameter.
    bool isGenericTypeParam(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto gen = it->genericParams.find(name);
            if (gen != it->genericParams.end()) {
                return true;
            }
        }
        return false;
    }

    /// @brief Look up a type by name, with alias resolution.
    /// 
    /// This handles both unqualified lookup and module-qualified lookup.
    /// For module-qualified names, use lookupTypeByAlias().
    TypeDeclAST* lookupTypeDeclWithAlias(InternedString name) const {
        // First try unqualified lookup
        TypeDeclAST* decl = lookupTypeDecl(name);
        if (decl) return decl;
        
        // If not found, try to parse as module:type
        // This is a fallback for cases where the parser didn't create a ModuleTypeAccessAST
        // The proper way is to use ModuleTypeAccessAST for qualified names
        return nullptr;
    }
    
    // ─── Module Member Lookup ──────────────────────────────────────────
    
    ValueDeclAST* lookupModuleValueMember(ModuleAST* module, InternedString memberName) const {
        if (!module) return nullptr;
        auto it = moduleTables.find(module);
        if (it == moduleTables.end()) return nullptr;
        auto found = it->second.values.find(memberName);
        return found != it->second.values.end() ? found->second : nullptr;
    }
    
    TypeDeclAST* lookupModuleTypeMember(ModuleAST* module, InternedString memberName) const {
        if (!module) return nullptr;
        auto it = moduleTables.find(module);
        if (it == moduleTables.end()) return nullptr;
        auto found = it->second.types.find(memberName);
        return found != it->second.types.end() ? found->second : nullptr;
    }
    
    ValueDeclAST* lookupValueByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return nullptr;
        return lookupModuleValueMember(module, memberName);
    }
    
    TypeDeclAST* lookupTypeByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return nullptr;
        return lookupModuleTypeMember(module, memberName);
    }
    
    // ─── Export Checking ──────────────────────────────────────────────
    
    bool isExported(DeclAST* decl) const {
        if (!decl) return false;
        for (AttributeAST* attr : decl->attributes) {
            if (attr->name == pool.intern("export")) {
                return true;
            }
        }
        return false;
    }
    
    bool isTypeExported(TypeDeclAST* decl) const {
        return isExported(decl);
    }
    
    bool isValueExported(ValueDeclAST* decl) const {
        return isExported(decl);
    }
    
    // ─── Module Member Keyword Info ────────────────────────────────────
    
    DeclKeyword lookupModuleMemberKeyword(ModuleAST* module, InternedString memberName) const {
        ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
        if (!decl) return DeclKeyword::Let;
        if (decl->isa<VarDeclAST>()) {
            return decl->as<VarDeclAST>()->keyword;
        }
        if (decl->isa<FuncDeclAST>()) {
            return decl->as<FuncDeclAST>()->keyword;
        }
        return DeclKeyword::Let;
    }
    
    bool isModuleMemberMutable(ModuleAST* module, InternedString memberName) const {
        ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
        if (!decl) return false;
        if (decl->isa<VarDeclAST>()) {
            return decl->as<VarDeclAST>()->keyword == DeclKeyword::Let;
        }
        if (decl->isa<FuncDeclAST>()) {
            return decl->as<FuncDeclAST>()->keyword == DeclKeyword::Let;
        }
        return false;
    }
    
    bool isModuleMemberConst(ModuleAST* module, InternedString memberName) const {
        ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
        if (!decl) return false;
        if (decl->isa<VarDeclAST>()) {
            return decl->as<VarDeclAST>()->keyword == DeclKeyword::Const;
        }
        if (decl->isa<FuncDeclAST>()) {
            return decl->as<FuncDeclAST>()->keyword == DeclKeyword::Const;
        }
        if (decl->isa<EnumVariantAST>()) {
            return true;
        }
        return false;
    }
    
    DeclKeyword lookupModuleMemberKeywordByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return DeclKeyword::Let;
        return lookupModuleMemberKeyword(module, memberName);
    }
    
    bool isModuleMemberMutableByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return false;
        return isModuleMemberMutable(module, memberName);
    }
    
    bool isModuleMemberConstByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return false;
        return isModuleMemberConst(module, memberName);
    }
    
    // ─── Concurrency Helpers ─────────────────────────────────────────────
    
    void addPendingAsync(InternedString name, ExprAST* call, const SourceLocation& loc) {
        if (isAtModuleLevel()) return;
        PendingAsync pending{name, call, loc};
        currentScope().pendingAsync[name] = pending;
    }
    
    void addPendingSpawn(InternedString name, ExprAST* call, const SourceLocation& loc) {
        if (isAtModuleLevel()) return;
        PendingSpawn pending{name, call, loc};
        currentScope().pendingSpawn[name] = pending;
    }
    
    bool hasPendingAsync(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().pendingAsync.find(name) != currentScope().pendingAsync.end();
    }
    
    bool hasPendingSpawn(InternedString name) const {
        if (scopes.empty()) return false;
        return currentScope().pendingSpawn.find(name) != currentScope().pendingSpawn.end();
    }
    
    bool isPendingFuture(InternedString name) const {
        return hasPendingAsync(name) || hasPendingSpawn(name);
    }
    
    void resolveAsync(InternedString name) {
        if (!scopes.empty()) {
            currentScope().pendingAsync.erase(name);
        }
    }
    
    void resolveSpawn(InternedString name) {
        if (!scopes.empty()) {
            currentScope().pendingSpawn.erase(name);
        }
    }
    
    std::vector<InternedString> getPendingAsyncNames() const {
        std::vector<InternedString> result;
        if (!scopes.empty()) {
            for (const auto& [name, _] : currentScope().pendingAsync) {
                result.push_back(name);
            }
        }
        return result;
    }
    
    std::vector<InternedString> getPendingSpawnNames() const {
        std::vector<InternedString> result;
        if (!scopes.empty()) {
            for (const auto& [name, _] : currentScope().pendingSpawn) {
                result.push_back(name);
            }
        }
        return result;
    }
    
    bool hasPendingAsync() const {
        return !scopes.empty() && !currentScope().pendingAsync.empty();
    }
    
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
    
    /// @brief Get or create a named type with the given name and generic arguments.
    /// 
    /// The generic arguments are part of the cache key, so each distinct
    /// instantiation (e.g., Vec2<int> vs Vec2<float>) gets its own canonical node.
    /// 
    /// @param name The type name.
    /// @param genericArgs The generic arguments (empty for non-generic types).
    /// @return The canonical NamedTypeAST node.
    NamedTypeAST* getNamedType(InternedString name, const ArenaSpan<TypeAST*>& genericArgs = {}) {
        TypeCache::NamedTypeKey key{name, genericArgs};
        auto it = typeCache.namedTypes.find(key);
        if (it != typeCache.namedTypes.end()) {
            return it->second;
        }
        
        // Create a new NamedTypeAST with the generic args stored as ArenaSpan
        NamedTypeAST* type = arena.make<NamedTypeAST>(name);
        
        // Since the node is newly created, we need to store the generic args.
        // The ArenaSpan points into arena memory - this is safe because the
        // ASTArena owns all type nodes and the generic args are also arena-allocated.
        type->genericArgs = genericArgs;
        
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

    /// @brief Get or create a pointer type.
    PtrTypeAST* getPtrType(TypeAST* inner) {
        if (!inner) return nullptr;
        
        TypeCache::PtrTypeKey key{inner};
        auto it = typeCache.ptrTypes.find(key);
        if (it != typeCache.ptrTypes.end()) {
            return it->second;
        }
        
        PtrTypeAST* type = arena.make<PtrTypeAST>(inner);
        typeCache.ptrTypes[key] = type;
        return type;
    }

    /// @brief Get or create a reference type.
    RefTypeAST* getRefType(TypeAST* inner) {
        if (!inner) return nullptr;
        
        TypeCache::RefTypeKey key{inner};
        auto it = typeCache.refTypes.find(key);
        if (it != typeCache.refTypes.end()) {
            return it->second;
        }
        
        RefTypeAST* type = arena.make<RefTypeAST>(inner);
        typeCache.refTypes[key] = type;
        return type;
    }
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    
    void pushDefiningType(TypeDeclAST* decl) {
        definingTypes.push_back(decl);
    }
    
    void popDefiningType() {
        if (!definingTypes.empty()) {
            definingTypes.pop_back();
        }
    }
    
    bool isDefiningType(TypeDeclAST* decl) const {
        for (TypeDeclAST* d : definingTypes) {
            if (d == decl) return true;
        }
        return false;
    }
    
    TypeDeclAST* currentDefiningType() const {
        return definingTypes.empty() ? nullptr : definingTypes.back();
    }
    
    size_t getClosureDepth() const { return stack.getClosureDepth(); }
    bool insideNestedFunction() const { return stack.insideNestedFunction(); }
    FuncDeclAST* getInnermostFunction() const { return stack.getInnermostFunction(); }
    BaseAST* getInnermostFunctionNode() const { return stack.getInnermostFunctionNode(); }
};

/// # RAII Guards
///
/// RAII guards provide automatic cleanup for semantic analysis state.
/// They ensure that contexts, scopes, and narrowing levels are properly
/// popped even when errors occur.
///
/// ## Guard Hierarchy
///
/// ```
/// ┌────────────────────────────────────────────────────────────────────────────┐
/// │                           RAII Guards                                      │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                    ScopedSemanticContext                            │   │
/// │  │  Pushes/pops a context frame on the ContextStack                    │   │
/// │  │  Usage: Entering a function, loop, switch, or block                 │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                        ScopedIfCondition                            │   │
/// │  │  Sets up if condition context for narrowing detection               │   │
/// │  │  Usage: Analyzing the condition of an if statement                  │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                         SymbolScope                                 │   │
/// │  │  Pushes/pops a lexical scope for symbol storage                     │   │
/// │  │  Usage: Entering a block, function body, or loop body               │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                       ScopedNarrowing                               │   │
/// │  │  Pushes/pops a narrowing level for type refinement                  │   │
/// │  │  Usage: Entering the then/else branch of an if statement            │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// │                                                                            │
/// │  ┌─────────────────────────────────────────────────────────────────────┐   │
/// │  │                      ScopedTypeDefinition                           │   │
/// │  │  Tracks the type currently being defined for self-reference checks  │   │
/// │  │  Usage: Resolving a struct, enum, or trait declaration              │   │
/// │  └─────────────────────────────────────────────────────────────────────┘   │
/// └────────────────────────────────────────────────────────────────────────────┘
/// ```
///
/// ## Guard Reference
///
/// ### ScopedSemanticContext
///
/// ```cpp
/// // Usage: Push a context frame
/// ScopedSemanticContext context(ctx, ContextKind::FuncBody, node);
/// // ... resolve statements ...
/// // Automatically popped on destruction
/// ```
///
/// | ContextKind  | Use Case        | Effect                            |
/// | ------------ | --------------- | --------------------------------- |
/// | `FuncBody`   | Function body   | `return` allowed                  |
/// | `LoopBody`   | Loop body       | `break`/`continue` allowed        |
/// | `SwitchBody` | Switch body     | `case`/`default` allowed          |
/// | `IfStmt`     | If statement    | Type narrowing tracked            |
/// | `Block`      | Block statement | Pending inverse narrowing tracked |
///
/// ### ScopedIfCondition
///
/// ```cpp
/// // Usage: Analyzing an if condition
/// ScopedIfCondition ifCtx(ctx, stmt->elseBranch != nullptr);
/// // ... resolve condition ...
/// // Narrowing info captured in ctx.stack.getPendingNarrowing()
/// // Automatically cleared on destruction
/// ```
///
/// ### SymbolScope
///
/// ```cpp
/// // Usage: Enter a new scope for a block
/// SymbolScope scope(ctx);
/// // Insert local symbols
/// ctx.insertValue(localVar);
/// // Automatically popped on destruction
/// ```
///
/// ### ScopedNarrowing
///
/// ```cpp
/// // Usage: Enter then branch of an if statement
/// ScopedNarrowing narrowing(ctx, narrowings, false);  // direct narrowing
/// // ... resolve then branch ...
/// // Narrowing level popped on destruction
///
/// // Usage: Enter else branch
/// ScopedNarrowing narrowing(ctx, narrowings, true);   // inverse narrowing
/// // ... resolve else branch ...
/// // Narrowing level popped on destruction
/// ```
///
/// ### ScopedTypeDefinition
///
/// ```cpp
/// // Usage: Resolving a struct declaration
/// ScopedTypeDefinition def(ctx, structDecl);
/// // ... resolve fields ...
/// // If a field references the struct itself, isValidStructSelfReference()
/// // detects it via ctx.currentDefiningType()
/// // Automatically popped on destruction
/// ```
///
/// ## Guard Composition Example
///
/// ```cpp
/// bool resolveIfStmt(IfStmtAST* stmt, SemaContext& ctx) {
///     // 1. Push if context for narrowing tracking
///     ScopedSemanticContext context(ctx, ContextKind::IfStmt, stmt);
///
///     // 2. Set up if condition context
///     ScopedIfCondition ifCtx(ctx, stmt->elseBranch != nullptr);
///
///     // 3. Resolve condition (captures narrowing info)
///     resolveExpr(stmt->condition, ctx);
///
///     // 4. Get captured narrowing info
///     NarrowingInfo info = ctx.stack.getPendingNarrowing();
///
///     // 5. Push scope for then branch
///     SymbolScope scope(ctx);
///
///     // 6. Apply narrowing to then branch
///     ScopedNarrowing narrowing(ctx, info.narrowings, false);
///
///     // 7. Resolve then branch
///     resolveStmt(stmt->thenBranch, ctx);
///
///     // All guards automatically pop in reverse order:
///     // 1. narrowing pops
///     // 2. scope pops
///     // 3. ifCtx pops (clears pending narrowing)
///     // 4. context pops
///     return ...;
/// }
/// ```

struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind, BaseAST* node)
        : ctx_(ctx) {
        ctx_.stack.push(kind, node);
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

struct SymbolScope {
    explicit SymbolScope(SemaContext& ctx)
        : ctx_(ctx) {
        ctx_.pushScope();
    }
    
    ~SymbolScope() {
        ctx_.popScope();
    }
    
    SymbolScope(const SymbolScope&) = delete;
    SymbolScope& operator=(const SymbolScope&) = delete;

private:
    SemaContext& ctx_;
};

struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                    TypeAST* narrowedType, bool isInverse = false)
        : ctx_(ctx) {
        ctx_.stack.pushNarrowingLevel(isInverse);
        ctx_.stack.narrowVariable(varName, narrowedType);
    }
    
    ScopedNarrowing(SemaContext& ctx, 
                    const std::unordered_map<InternedString, TypeAST*>& narrowings,
                    bool isInverse = false)
        : ctx_(ctx) {
        ctx_.stack.pushNarrowingLevel(isInverse);
        for (const auto& [name, type] : narrowings) {
            ctx_.stack.narrowVariable(name, type);
        }
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
    ScopedTypeDefinition(SemaContext& ctx, TypeDeclAST* decl)
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