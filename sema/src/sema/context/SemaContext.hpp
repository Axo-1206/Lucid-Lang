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
 * ─── Design: the language has three declaration forms ─────────────────────
 * §1 of the grammar says a module contains TABLE declarations, FN
 * declarations, and variable declarations. Three forms, three shapes.
 * The AST models this directly: `TableDeclAST`, `FnDeclAST`,
 * `VarDeclAST` are siblings under `DeclAST`, with no shared value/type
 * intermediate between them and no diamond in the inheritance graph.
 *
 * The module table mirrors the AST's three forms: a `tables` map, a
 * `functions` map, and a `variables` map. A lookup tells the caller
 * which form it found without a family check.
 *
 * ─── Design: a table is both a type and a value ──────────────────────────
 * A table name refers to a sheet, and a sheet is usable in two
 * positions:
 *
 *   - as a **type** (`&Person`, `let p: Person`), where the name
 *     resolves through the type namespace;
 *   - as a **value** (`Person.ADD(...)`, `Person[i]`, and a bare
 *     `Person` in an expression position), where the name resolves
 *     through the value namespaces.
 *
 * The two positions are stored in two maps, but they hold the *same*
 * `TableDeclAST*`. There is one node per table declaration; the two
 * maps just provide two ways to reach it. This is the "one node, two
 * entries" model.
 *
 *   - `types` holds tables (as `TypeDeclAST*`), and is what
 *     `lookupType` reads.
 *   - `tables` holds tables (as `TableDeclAST*`), and is what
 *     `lookupTable` reads.
 *
 * The polymorphic `lookupValue` consults `variables`, then
 * `functions`, then `tables`, and reports which map it found the name
 * in.
 *
 * ─── Design: local scopes hold values only ────────────────────────────────
 * `FN` and `TABLE` are module-only declarations (§12.5). A local scope
 * therefore holds only variables and parameters, both of which are
 * `ValueDeclAST`s. `Scope::values` is a single map of `ValueDeclAST*`.
 *
 * The module table needs three maps for values because a top-level
 * declaration can be any of the three forms; a local scope needs one
 * because a local declaration can only be a variable or a parameter.
 *
 * ─── Design: the type cache is load-bearing ───────────────────────────────
 * `TypeCache` canonicalizes every type it produces: two references to
 * `int` share one `PrimitiveTypeAST`, two references to `Person` share
 * one `NamedTypeAST`, two references to `[int]` share one
 * `ArrayTypeAST`. The share is by pointer, and it is what makes
 * `typesEqual` O(1) in the common case.
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
 * session's registries die with the session.
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
// ValueLookup
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The result of a polymorphic value lookup.
///
/// A bare identifier in an expression position can resolve to one of
/// three value forms:
///
///   - a **variable** — a `let`/`const` binding;
///   - a **function** — a `FN` declaration;
///   - a **table** — the sheet itself, used as a value.
///
/// A parameter is a fourth form, but only inside a function's lexical
/// scope; a `lookupValue` on the module table never produces one.
/// `ValueLookup::Kind::Param` exists so a lookup that walks into the
/// scope chain can report a parameter without a second result type.
///
/// The struct is a discriminated union: `kind` says which field is
/// valid. A caller that wants a specific form can call the specific
/// lookup (`lookupVariable`, `lookupFunction`, `lookupTable`) and skip
/// the discrimination. A caller that wants "whatever this name is"
/// reads `kind` and picks the field.
struct ValueLookup {
    enum class Kind : uint8_t {
        None,       ///< The name did not resolve.
        Variable,   ///< A `let`/`const` binding.
        Function,   ///< A `FN` declaration.
        Table,      ///< A table, used as a value.
        Param,      ///< A function parameter.
    };

    Kind kind = Kind::None;

    VarDeclAST*   variable = nullptr;   ///< valid when kind == Variable
    FnDeclAST*    function = nullptr;   ///< valid when kind == Function
    TableDeclAST* table    = nullptr;   ///< valid when kind == Table
    ParamAST*     param    = nullptr;   ///< valid when kind == Param

    /// True if the name resolved to anything.
    bool found() const { return kind != Kind::None; }

    // ─── Convenience factories ──────────────────────────────────────────

    static ValueLookup none()     { return {}; }

    static ValueLookup from(VarDeclAST* v) {
        ValueLookup r; r.kind = Kind::Variable; r.variable = v; return r;
    }
    static ValueLookup from(FnDeclAST* f) {
        ValueLookup r; r.kind = Kind::Function; r.function = f; return r;
    }
    static ValueLookup from(TableDeclAST* t) {
        ValueLookup r; r.kind = Kind::Table; r.table = t; return r;
    }
    static ValueLookup from(ParamAST* p) {
        ValueLookup r; r.kind = Kind::Param; r.param = p; return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleTable
// ─────────────────────────────────────────────────────────────────────────────

/// @brief All symbols declared at the top level of one module.
///
/// Four maps, one per form a top-level name can take:
///
///   - `tables` holds the table declarations, as values. This is the
///     map that `lookupTable` and the table case of `lookupValue`
///     consult.
///   - `types` holds the table declarations, as types. This is the map
///     that `lookupType` consults. It has the same names and the same
///     nodes as `tables`; the two maps are two views of the same set
///     of declarations.
///   - `functions` holds the `FN` declarations.
///   - `variables` holds the top-level `let`/`const` declarations.
///
/// `importAliases` maps a local alias (from `import x.y as z`) to the
/// imported `ModuleAST*`. It is populated by Sema during pass 1.
struct ModuleTable {
    ModuleAST* module = nullptr;

    std::unordered_map<InternedString, TableDeclAST*> tables;
    std::unordered_map<InternedString, TypeDeclAST*>  types;
    std::unordered_map<InternedString, FnDeclAST*>    functions;
    std::unordered_map<InternedString, VarDeclAST*>   variables;
    std::unordered_map<InternedString, ModuleAST*>    importAliases;
};

// ─────────────────────────────────────────────────────────────────────────────
// Scope
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One lexical scope: a map of local value bindings.
///
/// Scopes are pushed on entry to a function body, a block, an
/// if-branch, a loop body, and a switch body, and popped on exit. The
/// scope chain sits on top of the current module's tables: `lookupValue`
/// walks innermost scope outward, then the module table.
///
/// A scope holds `ValueDeclAST*` only. The two forms a local declaration
/// can take — a `let`/`const` variable and a function parameter — are
/// both `ValueDeclAST`s. A `TABLE` or `FN` is module-only (§12.5), so
/// neither can appear in a local scope.
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
        uint64_t  size;
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
    StringPool&             pool;
    ASTArena&               arena;
    diag::DiagnosticEngine& diagnostics;

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

    /// Lambdas collected during the module walk, keyed by the module
    /// they belong to. Appended to by `resolveLambdaExpr`; materialized
    /// into `ModuleAST::lambdas` by the module driver after pass 3.
    ///
    /// Keyed by module rather than a single vector because the three
    /// Sema passes are batched across modules: pass 2 of every module
    /// runs before pass 3 of any module. A lambda in a top-level
    /// `let`'s initializer is collected during pass 2; a lambda in a
    /// function body is collected during pass 3. Both land in the
    /// same per-module vector.
    std::unordered_map<ModuleAST*, std::vector<LambdaExprAST*>> pendingLambdas;

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

    bool         isAtModuleLevel() const;
    void         pushScope();
    void         popScope();
    Scope&       currentScope();
    const Scope& currentScope() const;

    bool isInCurrentScope(InternedString name) const;
    bool isModuleMember(InternedString name) const;

    // ─────────────────────────────────────────────────────────────────────
    // Symbol insertion
    // ─────────────────────────────────────────────────────────────────────

    /// Insert a table's name into the module's `tables` and `types`
    /// maps. Both entries point at the same `TableDeclAST*`. Types are
    /// module-level only, so this asserts `isAtModuleLevel()`.
    bool insertTable(TableDeclAST* decl);

    /// Insert a `FN` name into the module's `functions` map. Functions
    /// are module-level only.
    bool insertFunction(FnDeclAST* decl);

    /// Insert a top-level `let`/`const` name into the module's
    /// `variables` map. Variables are module-level only.
    bool insertVariable(VarDeclAST* decl);

    /// Insert a local variable or a parameter into the innermost lexical
    /// scope. Local bindings are always `ValueDeclAST`s — a `let`/`const`
    /// variable or a function parameter. The caller must have pushed a
    /// scope; this asserts `!isAtModuleLevel()`.
    bool insertLocal(ValueDeclAST* decl);

    /// Insert a type binding. Types are module-level only (§12.5); this
    /// asserts `isAtModuleLevel()` and writes to the current module
    /// table's `types` map.
    ///
    /// The only `TypeDeclAST` the new grammar produces is a
    /// `TableDeclAST`. `insertTable` performs the two-map write; a
    /// caller that wants only the type half (rare — a Sema pass that
    /// wants to check a name is already in the type namespace) calls
    /// this directly.
    bool insertType(TypeDeclAST* decl);

    bool addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node);

    // ─────────────────────────────────────────────────────────────────────
    // Symbol lookup — polymorphic
    // ─────────────────────────────────────────────────────────────────────

    /// Resolve `name` in the current lexical scopes and the current
    /// module's namespaces. Walks innermost scope outward, then the
    /// module's `variables`, `functions`, and `tables` maps.
    ///
    /// The return value is a discriminated union. A caller that wants a
    /// specific form can call the specific lookup (`lookupVariable`,
    /// `lookupFunction`, `lookupTable`) instead; those skip the
    /// discrimination and return the concrete pointer.
    ValueLookup lookupValue(InternedString name) const;

    // ─────────────────────────────────────────────────────────────────────
    // Symbol lookup — per form
    // ─────────────────────────────────────────────────────────────────────

    /// Resolve `name` to a function, or null.
    FnDeclAST* lookupFunction(InternedString name) const;

    /// Resolve `name` to a table (as a value), or null.
    TableDeclAST* lookupTable(InternedString name) const;

    /// Resolve `name` to a top-level variable, or null.
    VarDeclAST* lookupVariable(InternedString name) const;

    /// Resolve `name` to a type declaration, or null. Types live in the
    /// module's `types` map only; there is no lexical type scope.
    TypeDeclAST* lookupType(InternedString name) const;

    // ─────────────────────────────────────────────────────────────────────
    // Import and cross-module lookup
    // ─────────────────────────────────────────────────────────────────────

    ModuleAST* lookupImport(InternedString alias) const;

    ValueDeclAST* lookupModuleValueMember(ModuleAST* module,
                                          InternedString memberName) const;

    TableDeclAST* lookupModuleTableMember(ModuleAST* module,
                                          InternedString memberName) const;

    TypeDeclAST* lookupModuleTypeMember(ModuleAST* module,
                                        InternedString memberName) const;

    ValueDeclAST* lookupImportedValue(InternedString alias,
                                      InternedString member) const;

    TypeDeclAST* lookupImportedType(InternedString alias,
                                    InternedString member) const;

    // ─────────────────────────────────────────────────────────────────────
    // Type canonicalization accessors
    // ─────────────────────────────────────────────────────────────────────

    PrimitiveTypeAST* getPrimitiveType(PrimitiveKind kind);
    UnknownTypeAST*   getUnknownType();

    NamedTypeAST*    getNamedType    (InternedString name);
    ArrayTypeAST*    getArrayType    (ArrayKind kind, uint64_t size, TypeAST* element);
    RowRefTypeAST*   getRowRefType   (TypeAST* table);
    NullableTypeAST* getNullableType (TypeAST* inner);
    FunctionTypeAST* getFunctionType (ArenaSpan<TypeAST*> params, TypeAST* returnType);

    // ─── Named accessors for the primitives the compiler uses most ──────

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