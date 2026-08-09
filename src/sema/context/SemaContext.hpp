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
/// // Look up a variable (checks narrowing stack first)
/// const ValueDeclAST* decl = ctx.lookupValue(name);
/// const TypeAST* narrowed = ctx.stack.getNarrowedType(name);
/// if (narrowed) {
///     // Use narrowed type
/// }
/// ```
///
/// ## 3. Return Type Validation
///
/// ```cpp
/// // Get the expected return type for the current function
/// const TypeAST* expected = ctx.stack.currentReturnType();
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
/// for (const InternedString& name : ctx.getPendingAsyncNames()) {
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
///     use(x)              ← 3. lookupValue() returns narrowed type
/// } else {
///     // x is nil         ← 4. Inverse narrowing in else branch
///     handleNil()
/// }
/// // x is int?            ← 5. Narrowing level popped
///
/// if x == nil { return }  ← 6. Standalone if with early exit
/// // x is int             ← 7. Pending inverse narrowing applied to block
/// use(x)                  ← 8. lookupValue() returns narrowed type
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
/// │  │  values:  std::unordered_map<InternedString, const ValueDeclAST*>   │   │
/// │  │  ┌────────────────────────────────────────────────────────────────┐ │   │
/// │  │  │  "add"     → FuncDeclAST (function)                            │ │   │
/// │  │  │  "PI"      → VarDeclAST (const variable)                       │ │   │
/// │  │  │  "result"  → VarDeclAST (let variable)                         │ │   │
/// │  │  │  "North"   → EnumVariantAST (enum variant)                     │ │   │
/// │  │  └────────────────────────────────────────────────────────────────┘ │   │
/// │  │                                                                     │   │
/// │  │  types:   std::unordered_map<InternedString, const TypeDeclAST*>    │   │
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
    std::unordered_map<InternedString, const ValueDeclAST*> values;
    std::unordered_map<InternedString, const TypeDeclAST*> types;
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
/// | `namedTypes`  | `{ name }`                | `NamedTypeAST*`     | User-defined types  |
/// | `arrayTypes`  | `{ kind, size, element }` | `ArrayTypeAST*`     | Array types         |
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
///   ├─► Check narrowing stack (getNarrowedType)
///   │    └─► If found, return narrowed type
///   │
///   ├─► Check local scopes (innermost to outermost)
///   │    └─► If found, return declaration
///   │
///   ├─► Check module table (current module)
///   │    └─► If found, return declaration
///   │
///   └─► Check import aliases (via lookupImport)
///        └─► If found, return declaration from imported module
///
/// Note: lookupType() follows the same pattern but searches the
///       type namespace instead of the value namespace.
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
///     const TypeAST* expected = ctx.stack.currentReturnType();
///     TypeAST* valueType = resolveExprWithTarget(returnValue, expected, ctx);
/// }
///
/// // Look up a variable (checks narrowing stack first)
/// const ValueDeclAST* decl = ctx.lookupValue(name);
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
    
    bool insertValue(const ValueDeclAST* decl) {
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
    
    bool insertType(const TypeDeclAST* decl) {
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
    
    bool insertGenericParam(const GenericParamDeclAST* param) {
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
    
    bool addImportAlias(InternedString alias, ModuleAST* module, const BaseAST* node = nullptr) {
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
    
    const TypeAST* getEffectiveType(const ValueDeclAST* decl, InternedString name) const {
        if (!decl || !decl->type) return nullptr;
        
        const TypeAST* narrowedType = stack.getNarrowedType(name);
        if (narrowedType) {
            return narrowedType;
        }
        
        return decl->type;
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
    
    const ValueDeclAST* lookupValue(InternedString name) const {
        const ValueDeclAST* decl = lookupValueRaw(name);
        if (!decl) return nullptr;
        
        const TypeAST* narrowedType = stack.getNarrowedType(name);
        if (narrowedType) {
            const_cast<ValueDeclAST*>(decl)->type = const_cast<TypeAST*>(narrowedType);
            return decl;
        }
        
        return decl;
    }
    
    const ValueDeclAST* lookupValueRaw(InternedString name) const {
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
    
    ModuleAST* lookupImport(InternedString alias) const {
        if (!currentModuleTable) return nullptr;
        auto it = currentModuleTable->importAliases.find(alias);
        return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
    }
    
    // ─── Module Member Lookup ──────────────────────────────────────────
    
    const ValueDeclAST* lookupModuleValueMember(ModuleAST* module, InternedString memberName) const {
        if (!module) return nullptr;
        auto it = moduleTables.find(module);
        if (it == moduleTables.end()) return nullptr;
        auto found = it->second.values.find(memberName);
        return found != it->second.values.end() ? found->second : nullptr;
    }
    
    const TypeDeclAST* lookupModuleTypeMember(ModuleAST* module, InternedString memberName) const {
        if (!module) return nullptr;
        auto it = moduleTables.find(module);
        if (it == moduleTables.end()) return nullptr;
        auto found = it->second.types.find(memberName);
        return found != it->second.types.end() ? found->second : nullptr;
    }
    
    const ValueDeclAST* lookupValueByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return nullptr;
        return lookupModuleValueMember(module, memberName);
    }
    
    const TypeDeclAST* lookupTypeByAlias(InternedString alias, InternedString memberName) const {
        ModuleAST* module = lookupImport(alias);
        if (!module) return nullptr;
        return lookupModuleTypeMember(module, memberName);
    }
    
    // ─── Export Checking ──────────────────────────────────────────────
    
    bool isExported(const DeclAST* decl) const {
        if (!decl) return false;
        for (AttributeAST* attr : decl->attributes) {
            if (attr->name == pool.intern("export")) {
                return true;
            }
        }
        return false;
    }
    
    bool isTypeExported(const TypeDeclAST* decl) const {
        return isExported(decl);
    }
    
    bool isValueExported(const ValueDeclAST* decl) const {
        return isExported(decl);
    }
    
    // ─── Module Member Keyword Info ────────────────────────────────────
    
    DeclKeyword lookupModuleMemberKeyword(ModuleAST* module, InternedString memberName) const {
        const ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
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
        const ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
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
        const ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
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
    
    void addPendingAsync(InternedString name, const ExprAST* call, const SourceLocation& loc) {
        if (isAtModuleLevel()) return;
        PendingAsync pending{name, call, loc};
        currentScope().pendingAsync[name] = pending;
    }
    
    void addPendingSpawn(InternedString name, const ExprAST* call, const SourceLocation& loc) {
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
/// bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
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
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind, const BaseAST* node)
        : ctx_(ctx) {
        ctx_.stack.push(kind, const_cast<BaseAST*>(node));
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
                    const TypeAST* narrowedType, bool isInverse = false)
        : ctx_(ctx) {
        ctx_.stack.pushNarrowingLevel(isInverse);
        ctx_.stack.narrowVariable(varName, narrowedType);
    }
    
    ScopedNarrowing(SemaContext& ctx, 
                    const std::unordered_map<InternedString, const TypeAST*>& narrowings,
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