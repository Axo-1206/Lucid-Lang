/**
 * @file SemaContext.hpp
 *
 * @responsibility Session-level semantic state: modules, symbol tables,
 *                 the type cache, the attribute registry, the built-in
 *                 method registry, and the traversal stack.
 *
 * ─── Role ─────────────────────────────────────────────────────────────────
 * A `SemaContext` is constructed once per compilation session by the
 * CLI, handed to every Sema entry point, and discarded when the session
 * ends. It owns:
 *
 *   - the borrowed resources (pool, arena, diagnostics),
 *   - the set of modules being analyzed,
 *   - one `ModuleTable` per module,
 *   - the lexical scope stack on top of the current module,
 *   - the type canonicalization cache,
 *   - the attribute registry (a fixed table of attribute metadata),
 *   - the built-in method registry (a fixed table of method metadata),
 *   - the `ContextStack` that tracks traversal position and narrowings.
 *
 * ─── Design: two namespaces per module ────────────────────────────────────
 * A `ModuleTable` stores values and types in separate maps. `TABLE
 * Person` registers `Person` in *both* maps — the type-namespace entry
 * is what `&Person` and `let p: Person` resolve against; the
 * value-namespace entry is what `Person.ADD(...)`, `Person[i]`, and a
 * bare `Person` in an expression position resolve against. Both point
 * at the same `TableDeclAST`.
 *
 * The two namespaces are still separate: a `let Person = ...` in the
 * same module collides with the table's value-namespace entry, because
 * `Person` already names a value (the sheet). That is the intended
 * behavior.
 *
 * Local scopes hold values only. `FN` and `TABLE` are module-only
 * declarations (§12.5), so `insertType` asserts module level and writes
 * to the module table.
 *
 * ─── Design: the type cache is load-bearing ───────────────────────────────
 * `TypeCache` canonicalizes every type it produces: two references to
 * `int` share one `PrimitiveTypeAST`, two references to `Person` share
 * one `NamedTypeAST`, two references to `[int]` share one
 * `ArrayTypeAST`. The share is by pointer, and it is what makes
 * `typesEqual` O(1) in the common case (`a == b` succeeds before the
 * structural fallback runs).
 *
 * The cache is also how the resolver disambiguates forms whose type is
 * not inferable from the expression alone. When a condition (`if`,
 * `while`) has no declared type, the resolver asks the cache for the
 * singleton `bool` and uses it as the target. Without canonicalization,
 * two `bool` nodes would compare unequal and the resolver could not
 * recognize "the expected type".
 *
 * ─── Design: the registries are members, not globals ──────────────────────
 * `AttributeRegistry` and `BuiltinMethodRegistry` are per-session
 * tables. They are constructed by the context's constructor, never
 * reassigned, and reached as `ctx.attributeRegistry` and
 * `ctx.builtinMethodRegistry`. Two sessions do not share them; a
 * session's registries die with the session. This mirrors how the type
 * cache and the symbol tables work and keeps the whole Sema subsystem
 * session-scoped.
 *
 * Both registries are cheap to construct (a `std::unordered_map` with a
 * handful of static entries) and hold no per-session state. Making them
 * members rather than static singletons means a future test can build a
 * context with a modified registry if it needs to.
 *
 * ─── Design: the context is not thread-safe ───────────────────────────────
 * Like the diagnostic engine and the string pool, `SemaContext` is
 * confined to one thread. The compiler pipeline is strictly sequential.
 * A future parallel-resolving pass would need per-worker contexts that
 * share the pool and the arena.
 */

#pragma once

#include "ContextStack.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "core/registry/AttributeRegistry.hpp"
#include "core/registry/BuiltinMethodRegistry.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <cassert>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// ModuleTable
// ─────────────────────────────────────────────────────────────────────────────

/// @brief All symbols declared at the top level of one module.
///
/// `values` holds tables, functions, and top-level `let`/`const`
/// variables. `types` holds tables — a table is a type, and
/// `TypeDeclAST` is the family base for type declarations. A `TABLE`
/// therefore appears in both maps under the same name; the two are
/// distinct lookups, not duplicates.
///
/// `importAliases` maps a local alias (from `import x.y as z`) to the
/// imported `ModuleAST*`. It is populated by Sema during pass 1.
struct ModuleTable {
    ModuleAST* module = nullptr;
    std::unordered_map<InternedString, ValueDeclAST*> values;
    std::unordered_map<InternedString, TypeDeclAST*>  types;
    std::unordered_map<InternedString, ModuleAST*>    importAliases;
};

// ─────────────────────────────────────────────────────────────────────────────
// Scope
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One lexical scope: a map of value bindings.
///
/// Scopes are pushed on entry to a function body, a block, an
/// if-branch, a loop body, and a switch body, and popped on exit. The
/// scope chain sits on top of the current module's table: `lookupValue`
/// walks innermost scope outward, then the module table.
///
/// A scope holds values only. `FN` and `TABLE` are module-only
/// (§12.5), so a local declaration cannot introduce a type name; the
/// `types` map that an earlier design carried here is gone.
struct Scope {
    std::unordered_map<InternedString, ValueDeclAST*> values;
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeCache
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Canonicalization cache for type nodes.
///
/// Every accessor returns the same pointer for structurally identical
/// requests. Two call sites writing `int` (or `int?`, or `[int]`) share
/// one AST node, which makes type comparison a pointer comparison and
/// keeps the arena from filling with duplicate nodes.
///
/// The cache stores one entry per primitive kind, one entry per named
/// type (keyed by the name), one entry per array shape (keyed by kind,
/// size, element), and so on. Every key is a structural fact about the
/// type; every value is the canonical node for that fact.
struct TypeCache {
    // ─── The unknown singleton ──────────────────────────────────────────
    /// The one `UnknownTypeAST`. Returned by every resolver on failure.
    UnknownTypeAST* unknownType = nullptr;

    // ─── Primitive types ────────────────────────────────────────────────
    /// One entry per `PrimitiveKind`. Populated lazily as primitives are
    /// requested.
    std::unordered_map<PrimitiveKind, PrimitiveTypeAST*> primitives;

    // ─── Named types ────────────────────────────────────────────────────
    /// One entry per (name). A `NamedTypeAST` is a reference to a table
    /// or a host type; the name is the whole key.
    struct NamedTypeKey {
        InternedString name;
        bool operator==(const NamedTypeKey& o) const { return name == o.name; }
    };
    struct NamedTypeKeyHash {
        size_t operator()(const NamedTypeKey& k) const {
            return std::hash<uint32_t>{}(k.name.id);
        }
    };
    std::unordered_map<NamedTypeKey, NamedTypeAST*, NamedTypeKeyHash> namedTypes;

    // ─── Array types ────────────────────────────────────────────────────
    struct ArrayTypeKey {
        ArrayKind kind;
        uint64_t  size;      // meaningful only when kind == Fixed
        TypeAST*  element;
        bool operator==(const ArrayTypeKey& o) const {
            return kind == o.kind && size == o.size && element == o.element;
        }
    };
    struct ArrayTypeKeyHash {
        size_t operator()(const ArrayTypeKey& k) const {
            size_t h = std::hash<int>{}(static_cast<int>(k.kind));
            h ^= std::hash<uint64_t>{}(k.size)     + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<TypeAST*>{}(k.element)  + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<ArrayTypeKey, ArrayTypeAST*, ArrayTypeKeyHash> arrayTypes;

    // ─── Row-reference types ────────────────────────────────────────────
    struct RowRefTypeKey {
        TypeAST* inner;
        bool operator==(const RowRefTypeKey& o) const { return inner == o.inner; }
    };
    struct RowRefTypeKeyHash {
        size_t operator()(const RowRefTypeKey& k) const {
            return std::hash<TypeAST*>{}(k.inner);
        }
    };
    std::unordered_map<RowRefTypeKey, RowRefTypeAST*, RowRefTypeKeyHash> rowRefTypes;

    // ─── Nullable types ─────────────────────────────────────────────────
    struct NullableTypeKey {
        TypeAST* inner;
        bool operator==(const NullableTypeKey& o) const { return inner == o.inner; }
    };
    struct NullableTypeKeyHash {
        size_t operator()(const NullableTypeKey& k) const {
            return std::hash<TypeAST*>{}(k.inner);
        }
    };
    std::unordered_map<NullableTypeKey, NullableTypeAST*, NullableTypeKeyHash> nullableTypes;

    // ─── Function types ─────────────────────────────────────────────────
    /// Keyed by the parameter-type span (as a pointer-and-size view,
    /// element-by-element) and the return type.
    struct FunctionTypeKey {
        ArenaSpan<TypeAST*> params;
        TypeAST*            returnType;
        bool operator==(const FunctionTypeKey& o) const {
            if (returnType != o.returnType) return false;
            if (params.size() != o.params.size()) return false;
            for (size_t i = 0; i < params.size(); ++i) {
                if (params[i] != o.params[i]) return false;
            }
            return true;
        }
    };
    struct FunctionTypeKeyHash {
        size_t operator()(const FunctionTypeKey& k) const {
            size_t h = std::hash<TypeAST*>{}(k.returnType);
            for (TypeAST* p : k.params) {
                h ^= std::hash<TypeAST*>{}(p) + 0x9e3779b9 + (h << 6) + (h >> 2);
            }
            return h;
        }
    };
    std::unordered_map<FunctionTypeKey, FunctionTypeAST*, FunctionTypeKeyHash> functionTypes;
};

// ─────────────────────────────────────────────────────────────────────────────
// SemaContext
// ─────────────────────────────────────────────────────────────────────────────

struct SemaContext {
    // ─── Borrowed resources ─────────────────────────────────────────────
    /// The string pool. Owns every interned name and string literal.
    StringPool&        pool;

    /// The AST arena. Every AST node and every type node is allocated
    /// from here. The context never frees a node; the arena is reclaimed
    /// when the session dies.
    ASTArena&          arena;

    /// The diagnostic engine. Collects errors, warnings, and notes.
    diag::DiagnosticEngine&  diagnostics;

    // ─── Modules ────────────────────────────────────────────────────────
    /// Every module the CLI handed over. Populated before any pass runs.
    /// The order is the order the CLI supplied; `analyze` runs the three
    /// passes in that order.
    std::vector<ModuleAST*> modules;

    /// The module lookup by path. A module's identity is its file path
    /// relative to the package root (§3.1).
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;

    /// The module currently being analyzed. Set by `enterModule`. Every
    /// name lookup starts here; every symbol insertion goes here.
    ModuleAST*    currentModule      = nullptr;

    /// The current module's table. Points into `moduleTables`. A pointer
    /// rather than a copy because the module table can be reallocated
    /// when a new module is entered; the pointer is refreshed by
    /// `enterModule` on every switch.
    ModuleTable*  currentModuleTable = nullptr;

    /// One table per module. Populated lazily by `getOrCreateModuleTable`.
    std::unordered_map<ModuleAST*, ModuleTable> moduleTables;

    // ─── Lexical scopes ─────────────────────────────────────────────────
    /// Scopes on top of the current module. Empty means module level.
    /// Each entry is pushed by a `SymbolScope` guard on entry to a
    /// function body, a block, an if-branch, a loop body, or a switch
    /// body.
    std::vector<Scope> scopes;

    // ─── Type canonicalization ──────────────────────────────────────────
    /// The canonicalization cache. Every type accessor below returns a
    /// node from this cache, and every pair of structurally identical
    /// requests returns the same pointer.
    TypeCache typeCache;

    // ─── Registries ─────────────────────────────────────────────────────
    /// The fixed table of language attributes. Constructed once per
    /// session by the context's constructor; never reassigned. Reached
    /// as `ctx.attributeRegistry` by `AttributeValidator`.
    AttributeRegistry attributeRegistry;

    /// The fixed table of built-in methods on tables, column views, and
    /// arrays. Constructed once per session; never reassigned. Reached
    /// as `ctx.builtinMethodRegistry` by `SemaExpr.cpp` when it
    /// classifies a table or array method.
    BuiltinMethodRegistry builtinMethodRegistry;

    // ─── Traversal state ────────────────────────────────────────────────
    /// The context stack. Tracks "where are we?" (function, loop,
    /// switch, if, block, sequence) and "what is narrowed here?". Owned
    /// here for convenience; accessed by call sites as `ctx.stack`.
    ContextStack stack;

    // ─── Construction ───────────────────────────────────────────────────
    SemaContext(StringPool& p, ASTArena& a, diag::DiagnosticEngine& d);

    SemaContext(const SemaContext&)            = delete;
    SemaContext& operator=(const SemaContext&) = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Module management
    // ─────────────────────────────────────────────────────────────────────

    /// Register a module with the context. Must be called before any
    /// pass runs. The module's `filePath` is the identity key for
    /// cross-module lookup.
    void addModule(ModuleAST* module);

    /// Make `module` the active module. Subsequent lookups start from
    /// its symbol table; `pushScope` layers lexical scopes on top.
    void enterModule(ModuleAST* module);

    /// The table for `module`, created on first access.
    ModuleTable& getOrCreateModuleTable(ModuleAST* module);

    /// The table for `module`, or null if none exists yet.
    ModuleTable* findModuleTable(ModuleAST* module);

    /// The module whose path equals `path`, or null.
    ModuleAST* findModuleByPath(InternedString path) const;

    // ─────────────────────────────────────────────────────────────────────
    // Scope management
    // ─────────────────────────────────────────────────────────────────────

    /// True when no lexical scope is open — we are at module level.
    bool isAtModuleLevel() const;

    void   pushScope();
    void   popScope();
    Scope&       currentScope();
    const Scope& currentScope() const;

    /// True if `name` is bound in the innermost lexical scope.
    bool isInCurrentScope(InternedString name) const;

    /// True if `name` is declared at the top level of the current module
    /// (in either the value or the type namespace).
    bool isModuleMember(InternedString name) const;

    // ─────────────────────────────────────────────────────────────────────
    // Symbol insertion
    // ─────────────────────────────────────────────────────────────────────

    /// Insert a value binding. At module level this writes to the
    /// current module table; inside a scope it writes to the innermost
    /// scope. Emits a redeclaration diagnostic and returns false on a
    /// collision.
    bool insertValue(ValueDeclAST* decl);

    /// Insert a type binding. Types are module-level only (§12.5); this
    /// asserts `isAtModuleLevel()` and writes to the current module
    /// table. Emits a redeclaration diagnostic and returns false on a
    /// collision.
    bool insertType(TypeDeclAST* decl);

    /// Register an import alias on the current module. Emits a
    /// diagnostic and returns false if the alias is already taken.
    bool addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node);

    // ─────────────────────────────────────────────────────────────────────
    // Symbol lookup
    // ─────────────────────────────────────────────────────────────────────
    //
    // Lookup walks innermost scope outward, then the current module
    // table. It does not follow imports; cross-module lookup goes
    // through `lookupImport` / `lookupModuleValueMember`.

    ValueDeclAST* lookupValue   (InternedString name) const;
    FnDeclAST*    lookupFunction(InternedString name) const;
    TypeDeclAST*  lookupType    (InternedString name) const;

    // ─────────────────────────────────────────────────────────────────────
    // Import and cross-module lookup
    // ─────────────────────────────────────────────────────────────────────

    /// Resolve an import alias to its module, or null.
    ModuleAST* lookupImport(InternedString alias) const;

    /// Look up a member of an imported module's value namespace.
    /// Does not check `@export`; the caller (the field-access
    /// classifier in `SemaExpr.cpp`) checks it.
    ValueDeclAST* lookupModuleValueMember(ModuleAST* module,
                                          InternedString memberName) const;

    /// Look up a member of an imported module's type namespace.
    TypeDeclAST* lookupModuleTypeMember(ModuleAST* module,
                                        InternedString memberName) const;

    /// Convenience: resolve an alias to a module, then look up the
    /// member in that module's value namespace. Null if either step
    /// fails.
    ValueDeclAST* lookupImportedValue(InternedString alias,
                                      InternedString member) const;

    /// Convenience: same, for the type namespace.
    TypeDeclAST* lookupImportedType(InternedString alias,
                                    InternedString member) const;

    // ─────────────────────────────────────────────────────────────────────
    // Type canonicalization accessors
    // ─────────────────────────────────────────────────────────────────────
    //
    // Every accessor returns the canonical singleton for its request.
    // The cache is what makes `typesEqual` cheap; two structurally
    // identical types share a pointer.

    PrimitiveTypeAST* getPrimitiveType(PrimitiveKind kind);
    UnknownTypeAST*   getUnknownType();

    NamedTypeAST*  getNamedType    (InternedString name);
    ArrayTypeAST*  getArrayType    (ArrayKind kind, uint64_t size, TypeAST* element);
    RowRefTypeAST* getRowRefType   (TypeAST* table);
    NullableTypeAST* getNullableType(TypeAST* inner);
    FunctionTypeAST* getFunctionType(ArenaSpan<TypeAST*> params, TypeAST* returnType);

    // ─── Named accessors for the primitives the compiler uses most ──────
    //
    // `getPrimitiveType(PrimitiveKind::Bool)` is correct but reads
    // poorly at a call site. These named forms are the ones most Sema
    // code uses; they are thin wrappers over `getPrimitiveType`.

    PrimitiveTypeAST* getBoolType();
    PrimitiveTypeAST* getIntType();
    PrimitiveTypeAST* getUint32Type();
    PrimitiveTypeAST* getUint64Type();
    PrimitiveTypeAST* getFloatType();
    PrimitiveTypeAST* getCharType();
    PrimitiveTypeAST* getStringType();
    PrimitiveTypeAST* getUnitType();
};

// ═════════════════════════════════════════════════════════════════════════════
// RAII guards
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Push a context frame for the duration of a scope.
///
/// `ScopedContext` is the generic guard for any context kind other than
/// `FuncBody`/`SequenceBody`. Those two are pushed by `ScopedFunction`.
struct ScopedContext {
    ScopedContext(SemaContext& ctx, ContextKind kind, BaseAST* node);
    ~ScopedContext();

    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief Mark the current if-statement as being in condition-analysis
///        mode.
///
/// Sets `isIfConditionCtx` on the innermost `IfStmt` frame; clears it on
/// destruction. While the flag is set, `resolveBinaryExpr` runs the
/// narrowing detector on the condition it is resolving.
struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse);
    ~ScopedIfCondition();

    ScopedIfCondition(const ScopedIfCondition&)            = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief Push a lexical scope. Pops on destruction.
struct SymbolScope {
    explicit SymbolScope(SemaContext& ctx);
    ~SymbolScope();

    SymbolScope(const SymbolScope&)            = delete;
    SymbolScope& operator=(const SymbolScope&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief Push a narrowing level. Pops on destruction.
///
/// The `isInverse` flag marks a level that was pushed for an `== nil`
/// branch. See `ContextStack::pushNarrowingLevel`.
struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName,
                    TypeAST* narrowedType, bool isInverse = false);
    ScopedNarrowing(SemaContext& ctx,
                    const std::unordered_map<InternedString, TypeAST*>& narrowings,
                    bool isInverse = false);
    ~ScopedNarrowing();

    ScopedNarrowing(const ScopedNarrowing&)            = delete;
    ScopedNarrowing& operator=(const ScopedNarrowing&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief Enter a function body.
///
/// Picks `FuncBody` vs. `SequenceBody` from `decl->isSequence`, pushes a
/// symbol scope for the parameters, and records the expected return
/// type on the frame.
///
/// This is the only place that distinction is made. Every downstream
/// check (`insideFunction`, `insideSequence`) reads the frame.
struct ScopedFunction {
    ScopedFunction(SemaContext& ctx, FnDeclAST* decl);
    ~ScopedFunction();

    ScopedFunction(const ScopedFunction&)            = delete;
    ScopedFunction& operator=(const ScopedFunction&) = delete;

private:
    SemaContext& ctx_;
    SymbolScope  paramScope_;
};

} // namespace lucid::sema