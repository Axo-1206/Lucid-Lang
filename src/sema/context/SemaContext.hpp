/// @file SemaContext.hpp
/// @brief Session-level semantic state: modules, symbol tables, and the type cache.
///
/// ─── Role ─────────────────────────────────────────────────────────────────
/// A `SemaContext` is constructed once per compilation session by the CLI,
/// handed to every Sema entry point, and discarded when the session ends.
/// It owns:
///
///   - the borrowed resources (pool, arena, diagnostics),
///   - the set of modules being analyzed,
///   - one `ModuleTable` per module,
///   - the lexical scope stack on top of the current module,
///   - the type canonicalization cache.
///
/// It does NOT track traversal position or flow state — that is the
/// `ContextStack`'s job, and it lives as a member here so callers reach it
/// as `ctx.stack`.
///
/// ─── What is not here (vs. the previous design) ───────────────────────────
/// No generics, no instantiations, no closures, no async/spawn, no
/// trait/struct self-reference tracking, no fallible/combined/pointer/ref
/// type accessors, no per-declaration keyword lookup helpers. See the
/// "deleted vs. the old design" table in the migration notes.
///
/// ─── Two namespaces per module ────────────────────────────────────────────
/// A `ModuleTable` stores values and types in separate maps. This is the
/// same split the old design used: `TABLE Person` and `let Person = ...`
/// can coexist, and lookup searches only the relevant namespace.
///
/// Local scopes hold values only. `FN` and `TABLE` are module-only
/// declarations (grammar §12.5), so `insertType` asserts module level.

#pragma once

#include "ContextStack.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <cassert>
#include <unordered_map>
#include <vector>

namespace lucid::sema {

// ─── ModuleTable ──────────────────────────────────────────────────────────

/// @brief All symbols declared at the top level of one module.
///
/// `values` holds tables, functions, and top-level `let`/`const` variables.
/// `types` holds tables (a table is a type; `TypeDeclAST` is the family
/// base). A `TABLE` declaration therefore appears in both maps, under the
/// same name — the two are distinct lookups, not duplicates.
///
/// `importAliases` maps a local alias (from `import x.y as z`) to the
/// imported `ModuleAST*`. It is populated by Sema during import resolution.
struct ModuleTable {
    ModuleAST* module = nullptr;
    std::unordered_map<InternedString, ValueDeclAST*> values;
    std::unordered_map<InternedString, TypeDeclAST*>  types;
    std::unordered_map<InternedString, ModuleAST*>    importAliases;
};

// ─── Scope ────────────────────────────────────────────────────────────────

/// @brief One lexical scope: a map of value bindings.
///
/// Scopes are pushed on entry to a function body, block, if-branch, loop
/// body, or switch body, and popped on exit. The scope chain sits on top
/// of the current module's table: `lookupValue` walks innermost scope
/// outward, then the module table.
///
/// A scope holds values only. `FN` and `TABLE` are module-only (§12.5),
/// so there is no types map here — `insertType` asserts module level and
/// writes to the module table instead.
struct Scope {
    std::unordered_map<InternedString, ValueDeclAST*> values;
};

// ─── TypeCache ────────────────────────────────────────────────────────────

/// @brief Canonicalization cache for type nodes.
///
/// Every accessor returns the same pointer for structurally identical
/// requests. Two call sites writing `int` (or `int?`, or `[int]`) share
/// one AST node, which makes type comparison a pointer comparison and
/// keeps the arena from filling with duplicate nodes.
///
/// The cache is load-bearing for `resolveExprWithTarget`. An expression
/// like `[]` has no element type of its own; the resolver needs a target
/// type to give it one. When the surrounding context provides a target
/// (a `let` annotation, an argument position), the cache is not needed.
/// When the context is a condition (`if`, `while`), the resolver asks the
/// cache for the singleton `bool` and uses that as the target. Without
/// canonicalization, two `bool` nodes would compare unequal and the
/// resolver could not recognize "the expected type".
struct TypeCache {
    // ─── Primitive types ────────────────────────────────────────────────
    UnknownTypeAST* unknownType = nullptr;
    std::unordered_map<PrimitiveKind, PrimitiveTypeAST*> primitives;

    // ─── Named types (tables, host-backed tables) ───────────────────────
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
            h ^= std::hash<uint64_t>{}(k.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<TypeAST*>{}(k.element) + 0x9e3779b9 + (h << 6) + (h >> 2);
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

// ─── SemaContext ──────────────────────────────────────────────────────────

struct SemaContext {
    // ─── Borrowed resources ─────────────────────────────────────────────
    StringPool&        pool;
    ASTArena&          arena;
    diag::DiagnosticEngine&  diagnostics;

    // ─── Modules ────────────────────────────────────────────────────────
    /// Every module the CLI handed over. Populated before any pass runs.
    std::vector<ModuleAST*> modules;

    /// Module identity is its path relative to the package root (§3.1).
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;

    /// The module currently being analyzed. Set by `enterModule`.
    ModuleAST*    currentModule      = nullptr;
    ModuleTable*  currentModuleTable = nullptr;

    /// One table per module. Populated lazily by `getOrCreateModuleTable`.
    std::unordered_map<ModuleAST*, ModuleTable> moduleTables;

    // ─── Lexical scopes (values only) ───────────────────────────────────
    /// Scopes on top of the current module. Empty means module level.
    /// Each entry is pushed by `ScopedScope` / `SymbolScope` on entry to a
    /// function body, block, if-branch, loop body, or switch body.
    ///
    /// Local scopes hold values only — `FN` and `TABLE` are module-only
    /// (§12.5), so `insertType` asserts `isAtModuleLevel()`.
    std::vector<Scope> scopes;

    // ─── Type canonicalization ──────────────────────────────────────────
    TypeCache typeCache;

    // ─── Traversal state ────────────────────────────────────────────────
    /// Owned here for convenience; accessed by call sites as `ctx.stack`.
    /// `ContextStack` stores only "where are we" and "what is narrowed";
    /// it emits no diagnostics and enforces no rules.
    ContextStack stack;

    // ─── Construction ───────────────────────────────────────────────────
    SemaContext(StringPool& p, ASTArena& a, diag::DiagnosticEngine& d);

    SemaContext(const SemaContext&)            = delete;
    SemaContext& operator=(const SemaContext&) = delete;

    // ─── Module management ──────────────────────────────────────────────

    /// Register a module with the context. Must be called before any pass.
    /// The module's `filePath` is the identity key for cross-module lookup.
    void addModule(ModuleAST* module);

    /// Make `module` the active module. Subsequent lookups start from its
    /// symbol table; `pushScope` layers lexical scopes on top of it.
    void enterModule(ModuleAST* module);

    /// The table for `module`, created on first access.
    ModuleTable& getOrCreateModuleTable(ModuleAST* module);

    /// The table for `module`, or null if none exists yet.
    ModuleTable* findModuleTable(ModuleAST* module);

    /// The module whose path equals `path`, or null.
    ModuleAST* findModuleByPath(InternedString path) const;

    // ─── Scope management ───────────────────────────────────────────────

    /// True when no lexical scope is open — we are at module level.
    bool isAtModuleLevel() const;

    void   pushScope();
    void   popScope();
    Scope&       currentScope();
    const Scope& currentScope() const;

    // ─── Scope queries ──────────────────────────────────────────────────

    /// True if `name` is bound in the innermost lexical scope.
    bool isInCurrentScope(InternedString name) const;

    /// True if `name` is declared at the top level of the current module.
    bool isModuleMember(InternedString name) const;

    // ─── Symbol insertion ───────────────────────────────────────────────

    /// Insert a value binding. At module level this writes to the current
    /// module table; inside a scope it writes to the innermost scope.
    /// Emits a redeclaration diagnostic and returns false on a collision.
    bool insertValue(ValueDeclAST* decl);

    /// Insert a type binding. Types are module-level only (§12.5); this
    /// asserts `isAtModuleLevel()` and writes to the current module table.
    /// Emits a redeclaration diagnostic and returns false on a collision.
    bool insertType(TypeDeclAST* decl);

    /// Register an import alias on the current module. Emits a diagnostic
    /// and returns false if the alias is already taken.
    bool addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node);

    // ─── Symbol lookup ──────────────────────────────────────────────────
    //
    // Lookup walks innermost scope outward, then the current module table.
    // It does not follow imports; cross-module lookup goes through
    // `lookupImportedValue` / `lookupImportedType`.

    ValueDeclAST* lookupValue   (InternedString name) const;
    FnDeclAST*    lookupFunction(InternedString name) const;
    TypeDeclAST*  lookupType    (InternedString name) const;

    // ─── Import lookup ──────────────────────────────────────────────────
    //
    // Import aliases live on the current module's table. `lookupImport`
    // resolves an alias to its module; the member lookups then read the
    // target module's table.

    ModuleAST*    lookupImport       (InternedString alias) const;
    ValueDeclAST* lookupImportedValue(InternedString alias, InternedString member) const;
    TypeDeclAST*  lookupImportedType (InternedString alias, InternedString member) const;

    // ─── Type canonicalization accessors ────────────────────────────────
    //
    // Every accessor returns the canonical singleton for its request.

    PrimitiveTypeAST* getPrimitiveType(PrimitiveKind kind);
    UnknownTypeAST*   getUnknownType();

    NamedTypeAST*  getNamedType    (InternedString name);
    ArrayTypeAST*  getArrayType    (ArrayKind kind, uint64_t size, TypeAST* element);
    RowRefTypeAST* getRowRefType   (TypeAST* table);
    NullableTypeAST* getNullableType(TypeAST* inner);
    FunctionTypeAST* getFunctionType(ArenaSpan<TypeAST*> params, TypeAST* returnType);
};

// ─── RAII Guards ──────────────────────────────────────────────────────────

/// Push a context frame for the duration of a scope.
struct ScopedContext {
    ScopedContext(SemaContext& ctx, ContextKind kind, BaseAST* node);
    ~ScopedContext();

    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    SemaContext& ctx_;
};

/// Mark the current if-statement as being in condition-analysis mode.
/// Narrowing detection reads `isIfConditionCtx()` to know when a
/// comparison against `nil` is a narrowing site.
struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse);
    ~ScopedIfCondition();

    ScopedIfCondition(const ScopedIfCondition&)            = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

/// Push a lexical scope. Pops on destruction.
struct SymbolScope {
    explicit SymbolScope(SemaContext& ctx);
    ~SymbolScope();

    SymbolScope(const SymbolScope&)            = delete;
    SymbolScope& operator=(const SymbolScope&) = delete;

private:
    SemaContext& ctx_;
};

/// Push a narrowing level. Pops on destruction, restoring the outer
/// narrowing state. The `isInverse` flag marks a level that was pushed
/// for an `== nil` branch (where the outer `T?` is narrowed to `nil`
/// rather than to `T`).
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

/// Enter a function body. Picks `FuncBody` vs. `SequenceBody` from
/// `decl->isSequence`, pushes a symbol scope for the parameters, and
/// records the expected return type on the frame.
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

    /// Pushed before the function frame, popped after it — a small
    /// inversion of the conceptual "enter function, enter scope" reading,
    /// kept so the member's RAII drives the pop. Both stacks are
    /// independent, so the inversion is invisible; it matters only if a
    /// future change ever reads the two stacks together between pushes.
    SymbolScope  paramScope_;
};

} // namespace lucid::sema