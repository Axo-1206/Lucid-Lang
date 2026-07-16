/**
 * @file SemaContext.hpp
 *
 * @responsibility Shared state for all semantic analysis passes.
 *                 This struct holds everything needed during name resolution,
 *                 type checking, and validation across the entire semantic phase.
 *
 * @architectural_note
 *   SemaContext is the semantic-phase counterpart of ParserContext, and is
 *   deliberately built on the same two ideas that make ParserContext work:
 *
 *   1. **Shared resources, single instance.** Just like ParserContext holds
 *      one StringPool/ASTArena for the whole (possibly multi-file) parse,
 *      SemaContext holds one StringPool/ASTArena/ModuleRegistry for the whole
 *      (possibly multi-module) semantic analysis. It is constructed once,
 *      up front, from the full list of ModuleAST produced by the parser, and
 *      passed by reference to every semantic pass (NameResolver, TypeChecker,
 *      FFIValidator, ...).
 *
 *   2. **Two tiers of symbol storage, not one.** This is the main structural
 *      difference from the parser's TokenStream/ParserContext split, so it's
 *      worth stating up front:
 *
 *        - `ModuleTable`  — PERSISTENT. One per module, keyed by ModuleAST*,
 *          created the first time that module is visited and never erased.
 *          Holds that module's top-level `values` and `types` namespaces,
 *          plus its import aliases. Because it is keyed per module, two
 *          different modules can each declare a top-level `count` without
 *          colliding — they simply live in two different ModuleTables.
 *
 *        - `Scope`        — TRANSIENT. Pushed when entering a function body,
 *          a block, or any construct with its own local names (including,
 *          notably, a generic struct/trait/function's generic-parameter
 *          list — see "Self-Reference" below). Popped, and forgotten
 *          entirely, on leaving that construct. There is no cross-module
 *          transient state; a Scope only ever exists while some specific
 *          construct in some specific module is being analyzed.
 *
 *      `insertValue()` / `insertType()` pick the right tier automatically:
 *      if no Scope is open, the name is module-level and goes into
 *      `currentModuleTable`; otherwise it goes into the innermost Scope.
 *      Callers never have to decide which table to write to — see
 *      `isAtModuleLevel()`.
 *
 * @architectural_note Why single-flow, not collect-then-check
 *   It is tempting to run two full passes per module: first walk every
 *   top-level declaration purely to populate `ModuleTable` (a "collect"
 *   pass), then walk again to type-check bodies (a "check" pass) — this is
 *   what lets many languages allow a function to call another function
 *   declared later in the same file. Lucid's semantic phase deliberately
 *   does NOT do this. Symbols are inserted into scope/table as each
 *   declaration is *reached* during a single top-to-bottom traversal, and
 *   lookups only ever see what has already been inserted so far. Collecting
 *   everything up front would make a later-declared type "resolve" simply
 *   because the second pass never noticed it hadn't been declared yet at
 *   the use site — the check would report success while, textually, the
 *   use came before the declaration. Single-flow makes that impossible by
 *   construction: nothing is visible before its own insertion point.
 *
 *   The one deliberate exception is a type referencing itself (see below) —
 *   and that exception is handled by *ordering*, not by a special table.
 *
 * @architectural_note Self-reference (a struct containing itself)
 *   ```lucid
 *   struct Node<T> {
 *       value T,
 *       next  *Node<T>?
 *   }
 *   ```
 *   `Node`'s own field list mentions `Node`. This works with no AST changes
 *   and no special-cased lookup, purely because of *when* the visitor calls
 *   `insertType()`: the pass that handles a StructDeclAST inserts the
 *   struct's own name into `currentModuleTable` FIRST, then pushes a Scope
 *   for its generic parameters, THEN walks its fields. By the time `next`'s
 *   type annotation is resolved, `lookupType("Node")` already succeeds —
 *   because `Node` was inserted before its own body was ever visited, not
 *   because SemaContext special-cases structs.
 *
 *   What SemaContext *does* provide on top of that is `definingTypeStack`
 *   (see below): a way for TypeChecker to distinguish "this name resolved
 *   because it was fully defined earlier" from "this name resolved but I am
 *   currently in the middle of defining it" — which is exactly the
 *   information needed to reject `value Node<T>` (infinite size) while
 *   accepting `next ptr<Node<T>>?` (indirection breaks the cycle). Deciding
 *   which indirections are acceptable is TypeChecker's business; SemaContext
 *   only exposes the "am I mid-definition" fact.
 *
 *   The context is passed by reference to all semantic passes.
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <unordered_map>
#include <vector>
#include <sstream>
#include <type_traits>
#include <cassert>

class ModuleRegistry;

// ─────────────────────────────────────────────────────────────────────────────
// SemanticContext — what kind of semantic construct we're currently inside.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The kind of semantic construct currently being analyzed.
 *
 * Pushed/popped as the semantic passes enter and leave nested constructs
 * (function bodies, loops, switches, async contexts, etc). This is
 * semantic-level state — it tracks what's being *validated*, not what's
 * being *parsed* (that's SyntacticContext, in the parser). It answers
 * questions like "is `return` legal right here", not "what token closes
 * the thing I'm currently inside".
 *
 * Used by validation rules to determine what's allowed in the current context:
 *   - `return` only valid inside `FuncBody`
 *   - `break`/`continue` only valid inside `LoopBody`
 *   - `case` only valid inside `SwitchBody`
 *   - `await` only valid inside `AsyncBody`
 *   - `yield` only valid inside `GeneratorBody`
 *
 * @note This stack is independent of the Scope stack (`SemaContext::scopes`).
 *       A single Scope (e.g. a function body's block) may open exactly one
 *       SemanticContext frame (FuncBody), but nested blocks inside that same
 *       function push additional Scopes without pushing additional
 *       SemanticContext frames — `currentContext()` still reports FuncBody
 *       for an `if` block nested inside a function, because entering a
 *       plain block doesn't change what constructs (return/break/await) are
 *       legal. A `for` loop nested in that function, by contrast, pushes
 *       both: a Scope (for the loop variable) AND a LoopBody frame (so
 *       `break` becomes legal).
 */
enum class SemanticContext {
    TopLevel,       // Module-level declarations (no function context)
    FuncBody,       // Inside a function body (return allowed)
    LoopBody,       // Inside a loop body (break/continue allowed)
    SwitchBody,     // Inside a switch body (case/default allowed)
    AsyncBody,      // Inside an async function (await allowed)
    GeneratorBody,  // Inside a generator function (yield allowed)
    ParallelBody,   // Inside a parallel/spawn block (no return/break/continue)
};

/// Human-readable name for a SemanticContext, for diagnostics/logging.
inline const char* semanticContextName(SemanticContext kind) {
    switch (kind) {
        case SemanticContext::TopLevel:      return "top level";
        case SemanticContext::FuncBody:      return "function body";
        case SemanticContext::LoopBody:      return "loop body";
        case SemanticContext::SwitchBody:    return "switch body";
        case SemanticContext::AsyncBody:     return "async body";
        case SemanticContext::GeneratorBody: return "generator body";
        case SemanticContext::ParallelBody:  return "parallel body";
    }
    return "unknown context";
}

/**
 * @brief One frame of the semantic context stack.
 *
 * Records not just what kind of construct is open, but the AST node
 * and location where it was opened — so a diagnostic like "return
 * outside function" can point back at the function's definition.
 */
struct SemanticFrame {
    SemanticContext kind;
    BaseAST* node;              // The AST node that opened this context
    SourceLocation openedAt;    // Where the construct was opened
};

// ─────────────────────────────────────────────────────────────────────────────
// Scope — a single TRANSIENT lexical scope (function body, block, generic
// parameter list, ...). Forgotten entirely once popped.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A single transient lexical scope.
 *
 * Each scope maintains three separate namespaces:
 *   - Value namespace: variables, functions, parameters, fields, enum variants
 *     (functions are ValueDeclAST — see DeclAST.hpp's namespace-separation
 *     note — so there is deliberately no separate "function table"; a local
 *     function-valued binding and a local variable binding contend for the
 *     same name, exactly as the grammar intends)
 *   - Type namespace: structs, enums, traits
 *   - Generic parameter namespace: names bound by an enclosing `<T, ...>`
 *
 * This separation allows `struct Point` and `let Point = 42` to coexist.
 *
 * ## Lifetime
 *
 * A Scope is pushed by `SemaContext::pushScope()` (prefer `ScopedScope`) on
 * entry to any construct with its own local names — a function body, a
 * block, a for-loop's induction variable, or a generic struct/trait/func's
 * parameter list while its internals are being resolved — and popped, with
 * everything it contains simply discarded, on exit. Nothing in a Scope
 * outlives the construct that opened it. This is what makes local
 * declare-before-use sequential and per-construct: unlike a module's
 * top-level table (see ModuleTable), a Scope is never revisited once its
 * construct is done.
 */
struct Scope {
    // Value namespace: variables, functions, parameters, fields, enum variants
    std::unordered_map<InternedString, ValueDeclAST*> values;

    // Type namespace: structs, enums, traits
    std::unordered_map<InternedString, TypeDeclAST*> types;

    // Generic parameter names (only present in generic function/struct/trait
    // contexts). These are not in the value/type namespaces but shadow type
    // lookups — see SemaContext::lookupType().
    std::unordered_map<InternedString, GenericParamDeclAST*> genericParams;
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleTable — a single PERSISTENT per-module symbol table.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The persistent top-level symbol table for exactly one module.
 *
 * One ModuleTable exists per ModuleAST for the lifetime of the whole
 * semantic analysis (created on first visit via
 * `SemaContext::getOrCreateModuleTable()`, never erased). This is what lets:
 *
 *   - Two different modules declare a top-level symbol with the same name
 *     without colliding: `moduleTables[moduleA]` and `moduleTables[moduleB]`
 *     are entirely separate maps.
 *   - Cross-module lookups after a module has been fully analyzed (e.g. for
 *     `import`/`ModuleAccessExprAST` resolution): the table survives after
 *     SemaContext moves on to the next module, unlike a Scope.
 *
 * Import aliases live here too (not in a global flat map) for the same
 * reason values/types do: `import std.io as io` in module A and a
 * differently-targeted `as io` alias in module B must not collide, since
 * each module resolves its own aliases against its own import list.
 *
 * @note Only top-level names live here. Struct fields, trait field
 *       requirements, and enum variants are reached through the owning
 *       TypeDeclAST's own spans (`StructDeclAST::fields`, etc.), never
 *       through ModuleTable — they are not independently name-resolvable
 *       at module scope, only through a value of that struct/enum's type.
 */
struct ModuleTable {
    /// The module this table belongs to (set by getOrCreateModuleTable()).
    ModuleAST* module = nullptr;

    // Top-level value namespace: variables, functions.
    std::unordered_map<InternedString, ValueDeclAST*> values;

    // Top-level type namespace: structs, enums, traits.
    std::unordered_map<InternedString, TypeDeclAST*> types;

    // Import aliases declared by this module.
    // Example: `import std.io as io` → importAliases["io"] = module_ast_for_std_io
    std::unordered_map<InternedString, ModuleAST*> importAliases;
};

// ─────────────────────────────────────────────────────────────────────────────
// SemaContext — global semantic state for the entire analysis.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Global context for all semantic analysis passes.
 *
 * This struct holds:
 *   - The full list of modules under analysis, and a persistent ModuleTable
 *     per module (see ModuleTable's own doc comment).
 *   - The transient Scope stack for whichever construct is currently being
 *     analyzed within the current module (see Scope's own doc comment).
 *   - The semantic context stack (function/loop/switch/async nesting).
 *   - The "currently defining" type stack (self-reference support).
 *   - Diagnostics (error/warning collection), mirroring ParserContext's API.
 *
 * ─── Two-Tier Symbol Storage ───────────────────────────────────────────────
 * See the file-level doc comment above for the full rationale. In short:
 *   - `moduleTables[currentModule]` (aliased by `currentModuleTable`) is
 *     PERSISTENT and holds THIS module's top-level names.
 *   - `scopes` is TRANSIENT and holds whatever local construct is currently
 *     open (function body, block, generic parameter list, ...).
 *   - `insertValue()` / `insertType()` choose between them automatically
 *     based on `isAtModuleLevel()` (i.e. whether `scopes` is empty), so
 *     callers never decide by hand which table a declaration belongs in.
 *
 * ─── Lookup Rules ──────────────────────────────────────────────────────────
 *   - Search scopes from innermost to outermost.
 *   - If not found in any open scope, fall back to the current module's
 *     persistent table.
 *   - Do NOT cross into other modules automatically — imported symbols are
 *     only reached via `ModuleAccessExprAST` (module:member), resolved
 *     through `lookupImport()` + the target module's own ModuleTable.
 *   - Generic parameters shadow type names at whatever scope declares them.
 *
 * ─── Semantic Context System ──────────────────────────────────────────────
 * The context stack tracks what semantic construct we're currently inside.
 * This is used for validation:
 *   - `return` only allowed in FuncBody
 *   - `break`/`continue` only allowed in LoopBody
 *   - `case` only allowed in SwitchBody
 *   - `await` only allowed in AsyncBody
 *
 * Unlike the parser's SyntacticContext (which tracks parsing grammar),
 * SemanticContext tracks the meaning/validation rules of the constructs.
 *
 * ─── Processing a Module ──────────────────────────────────────────────────
 * Construct exactly one `ScopedModuleContext` at the point where a pass
 * begins analyzing a given ModuleAST (see ScopedModuleContext's own doc
 * comment) — this both selects/creates that module's persistent
 * ModuleTable and resets all transient state (scopes, context stack,
 * defining-type stack, per-file error list) so each module starts clean,
 * exactly the way ScopedFileContext does for the parser.
 *
 * @see SemanticContext for the full list of contexts and their rules.
 * @see ModuleTable, Scope for the two-tier storage model.
 */
struct SemaContext {
    // ─────────────────────────────────────────────────────────────────────
    // Shared Resources
    // ─────────────────────────────────────────────────────────────────────

    /// String interner (shared with the parse phase).
    StringPool& pool;

    /// AST allocator (shared with the parse phase — semantic passes also
    /// allocate from it, e.g. TypeDeclAST::selfType is built lazily here).
    ASTArena& arena;

    /// Reference to the global module registry for cross-module lookups
    /// once a module has been fully analyzed. May be null while the
    /// registry itself isn't ready yet (e.g. during early bring-up).
    ModuleRegistry* registry = nullptr;

    // ─────────────────────────────────────────────────────────────────────
    // Modules Under Analysis
    // ─────────────────────────────────────────────────────────────────────

    /// Every module the entry point was given, in the order provided.
    std::vector<ModuleAST*> modules;

    /// Fast path lookup — module.filePath (or package-qualified path) → module.
    /// Populated in the constructor from `modules`; used to resolve
    /// `import` targets to a ModuleAST before an alias can be registered.
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;

    /// The module currently being analyzed. Set/restored by
    /// ScopedModuleContext — prefer that over assigning this by hand.
    ModuleAST* currentModule = nullptr;

    // ─────────────────────────────────────────────────────────────────────
    // Persistent Per-Module Tables
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief One ModuleTable per module, created on first visit, never erased.
     *
     * Keyed by ModuleAST* rather than by path/name: identity of the module
     * node is unambiguous and avoids a redundant string lookup on every
     * insert. See ModuleTable's own doc comment for why this exists as a
     * map-of-tables rather than one flat map.
     */
    std::unordered_map<ModuleAST*, ModuleTable> moduleTables;

    /**
     * @brief Pointer into `moduleTables[currentModule]`, kept in sync by
     *        ScopedModuleContext / enterModule().
     *
     * A raw cached pointer rather than re-looking-up `moduleTables[currentModule]`
     * on every insert/lookup. Safe to cache because `std::unordered_map` does
     * not invalidate existing element references on insertion of *other* keys
     * (only rehashing the container itself would, and we never erase entries
     * or otherwise invalidate this specific one while it's the current table).
     */
    ModuleTable* currentModuleTable = nullptr;

    // ─────────────────────────────────────────────────────────────────────
    // Transient Scope Stack
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Stack of currently-open transient scopes, innermost at back.
     *
     * Empty exactly when analysis is at module top level for the current
     * module (see `isAtModuleLevel()`). Reset to empty by
     * ScopedModuleContext at the start of every module — a Scope from one
     * module's function body must never leak into another module's
     * analysis.
     */
    std::vector<Scope> scopes;

    // ─────────────────────────────────────────────────────────────────────
    // Semantic Context Stack
    // ─────────────────────────────────────────────────────────────────────

    std::vector<SemanticFrame> contextStack;   // Stack (innermost at back)

    // ─────────────────────────────────────────────────────────────────────
    // Currently-Defining Type Stack (self-reference support)
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Stack of TypeDeclAST currently in the middle of being defined.
     *
     * Pushed when a pass begins resolving a struct/enum/trait's own
     * internals (fields, variants), popped when it finishes. This does NOT
     * gate whether the type's name is *visible* — visibility comes purely
     * from insertion order (see the file-level "Self-Reference" note above)
     * — it exists so a check like TypeChecker's "is this field's type the
     * struct's own type, used directly (illegal, infinite size) rather than
     * through a pointer/reference/nullable indirection (legal)" has a way
     * to ask "is the type I just resolved still being defined right now,
     * or was it already fully defined earlier?" without needing an extra
     * "fully resolved" flag threaded through every TypeDeclAST.
     *
     * A stack, not a single flag, because definitions nest: a struct field
     * whose type is an anonymous/local generic instantiation of another
     * type still being defined is a real (if unusual) case worth being able
     * to answer correctly at any depth.
     */
    std::vector<TypeDeclAST*> definingTypeStack;

    // ─────────────────────────────────────────────────────────────────────
    // Diagnostics — mirrors ParserContext's API for a consistent feel
    // across the two frontend stages.
    // ─────────────────────────────────────────────────────────────────────

    /// True if any error has been reported while analyzing the current module.
    bool hasErrors = false;

    /// Collected diagnostic messages for the current module.
    std::vector<Diagnostic> errors;

    /// Consecutive error count (used to prevent infinite loops in recovery).
    int consecutiveErrors = 0;

    /**
     * @brief Diagnostics from every module analyzed so far in this session.
     *
     * Unlike `errors` (per-module scratch state, reset by
     * ScopedModuleContext for each module), this accumulates across the
     * whole semantic phase — every module's `errors` gets drained into
     * this right before ScopedModuleContext restores the previous module's
     * state. This is what the driver should read for a complete picture of
     * every semantic error found across every module.
     */
    std::vector<Diagnostic> allDiagnostics;

    // ─────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Construct the semantic context for a whole compilation.
     *
     * @param p       Shared string interner (same one used by the parser).
     * @param a       Shared AST allocator (same one used by the parser).
     * @param mods    Every module produced by the parse phase, in any order.
     * @param reg     The module registry, if ready; may be null.
     */
    SemaContext(StringPool& p, ASTArena& a,
                std::vector<ModuleAST*> mods,
                ModuleRegistry* reg = nullptr)
        : pool(p), arena(a), registry(reg), modules(std::move(mods))
    {
        for (ModuleAST* m : modules) {
            if (m) modulesByPath[m->filePath] = m;
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // String Conversion Helper
    // ─────────────────────────────────────────────────────────────────────

    std::string toString(InternedString s) const {
        return std::string(pool.lookup(s));
    }

    // ─────────────────────────────────────────────────────────────────────
    // Module Table Management
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Get this module's persistent table, creating it on first visit.
     *
     * Prefer entering modules via ScopedModuleContext, which calls this for
     * you and also resets transient state. Call directly only for read-only
     * cross-module lookups (e.g. resolving an already-analyzed import)
     * where you do NOT want to switch `currentModule`/reset scopes.
     */
    ModuleTable& getOrCreateModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        if (it != moduleTables.end()) return it->second;
        ModuleTable& table = moduleTables[module];
        table.module = module;
        return table;
    }

    /**
     * @brief Look up an already-created module table without creating one.
     * @return nullptr if that module hasn't been visited yet.
     */
    ModuleTable* findModuleTable(ModuleAST* module) {
        auto it = moduleTables.find(module);
        return it != moduleTables.end() ? &it->second : nullptr;
    }

    /**
     * @brief Resolve a module by its interned file/package path.
     *
     * Used when processing an `import` statement: the path string must be
     * turned into a ModuleAST before an alias can be registered.
     */
    ModuleAST* findModuleByPath(InternedString path) const {
        auto it = modulesByPath.find(path);
        return it != modulesByPath.end() ? it->second : nullptr;
    }

    /**
     * @brief Switch the current module, creating its table if needed.
     *
     * Prefer ScopedModuleContext over calling this directly — see its own
     * doc comment for why: a bare call here needs the caller to also reset
     * `scopes`/`contextStack`/`definingTypeStack`/errors by hand, which is
     * exactly the kind of easy-to-forget bookkeeping RAII exists to avoid.
     */
    void enterModule(ModuleAST* module) {
        currentModule = module;
        currentModuleTable = &getOrCreateModuleTable(module);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Scope Management Methods
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief True if there is no open transient scope — i.e. lookups and
     *        inserts right now operate on the current module's persistent
     *        top-level table.
     *
     * This is the flag that drives insertValue()/insertType()'s automatic
     * choice of which tier to write to; see the "Two-Tier Symbol Storage"
     * note on SemaContext.
     */
    bool isAtModuleLevel() const {
        return scopes.empty();
    }

    /**
     * @brief Push a new empty scope onto the stack.
     *
     * Prefer constructing a ScopedScope instead of calling this directly.
     */
    void pushScope() {
        scopes.push_back(Scope{});
    }

    /**
     * @brief Pop the innermost scope from the stack, discarding its contents.
     *
     * Prefer letting a ScopedScope's destructor call this over calling it
     * directly.
     */
    void popScope() {
        if (!scopes.empty()) {
            scopes.pop_back();
        }
    }

    /**
     * @brief Get the current (innermost) scope. Only valid when
     *        `!isAtModuleLevel()`.
     */
    Scope& currentScope() {
        return scopes.back();
    }

    /// Get the current (innermost) scope (const version).
    const Scope& currentScope() const {
        return scopes.back();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Insert Methods — automatically choose module table vs. local scope
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Insert a value declaration at the current level.
     *
     * If no scope is open (`isAtModuleLevel()`), the value is a top-level
     * declaration of `currentModule` and goes into `currentModuleTable`
     * (persistent). Otherwise it goes into the innermost open Scope
     * (transient — forgotten when that scope is popped).
     *
     * This is the single insertion point callers use regardless of level;
     * see the file-level "Self-Reference" note for why calling this BEFORE
     * walking into a construct's own body is what makes self-reference and
     * (for module-level declarations) declare-before-use both work without
     * any special-casing here.
     */
    void insertValue(InternedString name, ValueDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->values[name] = decl;
        } else {
            currentScope().values[name] = decl;
        }
    }

    /**
     * @brief Insert a type declaration at the current level.
     *
     * Same tiering rule as insertValue(). For a struct/enum/trait, this
     * must be called BEFORE resolving that declaration's own fields/
     * variants — see the self-reference note at the top of this file.
     */
    void insertType(InternedString name, TypeDeclAST* decl) {
        if (isAtModuleLevel()) {
            currentModuleTable->types[name] = decl;
        } else {
            currentScope().types[name] = decl;
        }
    }

    /**
     * @brief Insert a generic parameter into the innermost open scope.
     *
     * Generic parameters are never module-level, so unlike insertValue()/
     * insertType() this always targets the innermost Scope — the caller
     * (whoever is about to resolve a generic struct/trait/function's
     * internals) must have pushed one first, e.g.:
     *
     * ```cpp
     * ScopedScope guard(ctx);              // open a scope for T
     * ctx.insertGenericParam(tName, tDecl);
     * // ... resolve fields/params that may reference T ...
     * ```
     */
    void insertGenericParam(InternedString name, GenericParamDeclAST* param) {
        assert(!scopes.empty() && "insertGenericParam() requires an open Scope");
        currentScope().genericParams[name] = param;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Lookup Methods
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Look up a value: innermost-to-outermost open scopes, then the
     *        current module's persistent top-level table.
     *
     * Does NOT cross into other modules — an imported symbol is only
     * reached via `ModuleAccessExprAST`, resolved through `lookupImport()`
     * plus the target module's own ModuleTable.
     *
     * @return The ValueDeclAST if found, nullptr otherwise.
     */
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

    /**
     * @brief Convenience wrapper over lookupValue() for call sites that
     *        specifically need a callable.
     *
     * There is deliberately no separate "function table" (see Scope's doc
     * comment) — functions live in the same value namespace as variables,
     * so this is just lookupValue() narrowed to FuncDeclAST results.
     *
     * @return The FuncDeclAST if `name` resolves to one, nullptr otherwise
     *         (including when it resolves to a non-function value).
     */
    FuncDeclAST* lookupFunction(InternedString name) const {
        ValueDeclAST* v = lookupValue(name);
        return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
    }

    /**
     * @brief Look up a type: innermost-to-outermost open scopes (generic
     *        parameters shadow types at whatever scope declares them), then
     *        the current module's persistent top-level table.
     *
     * This is the tool TypeChecker uses to answer "does this type name
     * exist in the correct context right now" — "correct context" meaning
     * exactly this search order: local scopes first, current module's
     * top level second, nothing else automatically.
     *
     * @return The TypeDeclAST if found, nullptr otherwise (including when
     *         `name` resolves to a generic parameter instead — see
     *         lookupGenericParam() for that case).
     */
    TypeDeclAST* lookupType(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            // Generic parameters shadow type names.
            auto gen = it->genericParams.find(name);
            if (gen != it->genericParams.end()) {
                // Generic parameters are not TypeDeclAST; the caller should
                // check lookupGenericParam() separately if it needs this.
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

    /**
     * @brief Look up a generic parameter in the open scope stack.
     *
     * Generic parameters are always transient (never stored in a
     * ModuleTable), so this only searches `scopes`.
     *
     * @return The GenericParamDeclAST if found, nullptr otherwise.
     */
    GenericParamDeclAST* lookupGenericParam(InternedString name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->genericParams.find(name);
            if (found != it->genericParams.end()) {
                return found->second;
            }
        }
        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Currently-Defining Type Management (self-reference support)
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Mark a type declaration as "currently being defined".
     *
     * Prefer ScopedTypeDefinition over calling this directly.
     */
    void beginDefiningType(TypeDeclAST* decl) {
        definingTypeStack.push_back(decl);
    }

    /// Prefer ScopedTypeDefinition over calling this directly.
    void endDefiningType() {
        if (!definingTypeStack.empty()) {
            definingTypeStack.pop_back();
        }
    }

    /**
     * @brief True if `decl` is somewhere on the currently-defining stack
     *        (not necessarily the innermost).
     *
     * This is the check that lets TypeChecker distinguish a field whose
     * type is the struct's own (still-being-defined) type from a field
     * whose type is some other, already-fully-analyzed type — the former
     * is only legal through an indirection (ptr/ref/nullable), the latter
     * is legal directly. See the file-level "Self-Reference" note.
     */
    bool isDefiningType(TypeDeclAST* decl) const {
        for (TypeDeclAST* d : definingTypeStack) {
            if (d == decl) return true;
        }
        return false;
    }

    /// The innermost type currently being defined, or nullptr if none.
    TypeDeclAST* currentlyDefiningType() const {
        return definingTypeStack.empty() ? nullptr : definingTypeStack.back();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Semantic Context Management
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Push a new semantic context frame.
     *
     * Prefer constructing a ScopedSemanticContext instead of calling this
     * directly.
     */
    void pushContext(SemanticContext kind, BaseAST* node, const SourceLocation& loc) {
        contextStack.push_back({kind, node, loc});
    }

    /**
     * @brief Pop the innermost semantic context frame.
     *
     * Prefer letting a ScopedSemanticContext's destructor call this over
     * calling it directly.
     */
    void popContext() {
        if (!contextStack.empty()) {
            contextStack.pop_back();
        }
    }

    /**
     * @brief Get the current (innermost) semantic context.
     *
     * Returns SemanticContext::TopLevel when the stack is empty — an empty
     * stack IS TopLevel by convention, the same way an empty Scope stack
     * IS module level. No explicit TopLevel frame is ever pushed.
     */
    SemanticContext currentContext() const {
        return contextStack.empty() ? SemanticContext::TopLevel : contextStack.back().kind;
    }

    /// Get the current context's AST node.
    BaseAST* currentContextNode() const {
        return contextStack.empty() ? nullptr : contextStack.back().node;
    }

    /**
     * @brief True if `kind` is open anywhere on the stack, not just innermost.
     *
     * Useful for "am I nested inside a FuncBody at all" (e.g. detecting a
     * nested/anonymous function) as opposed to "is a FuncBody the specific
     * thing directly enclosing me right now", which is what
     * currentContext() answers instead.
     */
    bool isInsideContext(SemanticContext kind) const {
        for (const auto& frame : contextStack) {
            if (frame.kind == kind) return true;
        }
        return false;
    }

    /// Current nesting depth, i.e. how many semantic constructs are open.
    size_t contextDepth() const { return contextStack.size(); }

    /// True if we're currently inside a function body (of any flavor).
    bool insideFunction() const {
        return isInsideContext(SemanticContext::FuncBody) ||
               isInsideContext(SemanticContext::AsyncBody) ||
               isInsideContext(SemanticContext::GeneratorBody);
    }

    /// True if we're currently inside a loop body.
    bool insideLoop() const {
        return isInsideContext(SemanticContext::LoopBody);
    }

    /// True if we're currently inside a switch body.
    bool insideSwitch() const {
        return isInsideContext(SemanticContext::SwitchBody);
    }

    /// True if we're currently inside an async context.
    bool insideAsync() const {
        return isInsideContext(SemanticContext::AsyncBody);
    }

    /// True if we're currently inside a generator context.
    bool insideGenerator() const {
        return isInsideContext(SemanticContext::GeneratorBody);
    }

    /// True if we're currently inside a parallel/spawn context.
    bool insideParallel() const {
        return isInsideContext(SemanticContext::ParallelBody);
    }

    /// Get the innermost function declaration (if any).
    FuncDeclAST* currentFunction() const {
        for (auto it = contextStack.rbegin(); it != contextStack.rend(); ++it) {
            if (it->kind == SemanticContext::FuncBody ||
                it->kind == SemanticContext::AsyncBody ||
                it->kind == SemanticContext::GeneratorBody) {
                return static_cast<FuncDeclAST*>(it->node);
            }
        }
        return nullptr;
    }

    /// Get the innermost loop statement (if any).
    StmtAST* currentLoop() const {
        for (auto it = contextStack.rbegin(); it != contextStack.rend(); ++it) {
            if (it->kind == SemanticContext::LoopBody) {
                return static_cast<StmtAST*>(it->node);
            }
        }
        return nullptr;
    }

    /// Get the innermost switch statement (if any).
    SwitchStmtAST* currentSwitch() const {
        for (auto it = contextStack.rbegin(); it != contextStack.rend(); ++it) {
            if (it->kind == SemanticContext::SwitchBody) {
                return static_cast<SwitchStmtAST*>(it->node);
            }
        }
        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Import Aliases — per-module, stored on the current ModuleTable
    // ─────────────────────────────────────────────────────────────────────

    /**
     * @brief Register an import alias for the CURRENT module.
     *
     * Example: `import std.io as io` → `addImportAlias(io, moduleForStdIo)`.
     */
    void addImportAlias(InternedString alias, ModuleAST* module) {
        if (currentModuleTable) {
            currentModuleTable->importAliases[alias] = module;
        }
    }

    /**
     * @brief Look up an imported module by its alias, for the CURRENT module.
     */
    ModuleAST* lookupImport(InternedString alias) const {
        if (!currentModuleTable) return nullptr;
        auto it = currentModuleTable->importAliases.find(alias);
        return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Error Reporting — mirrors ParserContext's variadic-template API
    // ─────────────────────────────────────────────────────────────────────

private:
    template<typename T>
    struct is_interned_string : std::false_type {};

    template<>
    struct is_interned_string<InternedString> : std::true_type {};

    template<>
    struct is_interned_string<InternedString&> : std::true_type {};

    template<>
    struct is_interned_string<const InternedString&> : std::true_type {};

    template<typename T>
    typename std::enable_if<!is_interned_string<typename std::decay<T>::type>::value>::type
    streamTo(std::ostringstream& oss, T&& value) const {
        oss << std::forward<T>(value);
    }

    void streamTo(std::ostringstream& oss, InternedString s) const {
        oss << pool.lookup(s);
    }

    template<typename T>
    void buildMessageImpl(std::ostringstream& oss, T&& value) const {
        streamTo(oss, std::forward<T>(value));
    }

    template<typename T, typename... Rest>
    void buildMessageImpl(std::ostringstream& oss, T&& first, Rest&&... rest) const {
        streamTo(oss, std::forward<T>(first));
        buildMessageImpl(oss, std::forward<Rest>(rest)...);
    }

    template<typename... Args>
    std::string buildMessage(Args&&... args) const {
        if constexpr (sizeof...(Args) == 0) {
            return "";
        } else {
            std::ostringstream oss;
            buildMessageImpl(oss, std::forward<Args>(args)...);
            return oss.str();
        }
    }

    void addDiagnostic(DiagnosticSeverity severity,
                        DiagnosticCategory category,
                        const SourceLocation& loc,
                        DiagCode code,
                        const std::string& message) {
        InternedString file = currentModule ? currentModule->filePath : InternedString{};
        errors.push_back({
            severity,
            category,
            file,
            loc,
            code,
            {message}
        });
        if (severity == DiagnosticSeverity::Error ||
            severity == DiagnosticSeverity::Fatal) {
            hasErrors = true;
            consecutiveErrors++;
        } else if (severity == DiagnosticSeverity::Warning) {
            consecutiveErrors++;
        }
    }

public:
    /**
     * @brief Report an error at an AST node's location, with optional
     *        format args.
     *
     * ```cpp
     * ctx.error(useSite, DiagCode::E2001, "undefined type", ctx.toString(name));
     * ```
     *
     * @note Category is hard-coded to `DiagnosticCategory::Semantic` here —
     *       adjust the category name if the shared enum in
     *       DiagnosticCodes.hpp spells it differently.
     */
    template<typename... Args>
    void error(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Error,
                      DiagnosticCategory::Semantic,
                      node ? node->loc : SourceLocation{},
                      code,
                      message);
    }

    /// Report an error at a specific location with optional format args.
    template<typename... Args>
    void errorAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Error,
                      DiagnosticCategory::Semantic,
                      loc,
                      code,
                      message);
    }

    /// Report a warning at an AST node's location with optional format args.
    template<typename... Args>
    void warning(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Warning,
                      DiagnosticCategory::Semantic,
                      node ? node->loc : SourceLocation{},
                      code,
                      message);
    }

    /// Report a warning at a specific location with optional format args.
    template<typename... Args>
    void warningAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        addDiagnostic(DiagnosticSeverity::Warning,
                      DiagnosticCategory::Semantic,
                      loc,
                      code,
                      message);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Context Queries
    // ─────────────────────────────────────────────────────────────────────

    /// True if analysis can safely continue (bounded consecutive-error count).
    bool canContinue() const {
        return consecutiveErrors < 10;
    }

    /**
     * @brief Clear per-module error-tracking state.
     *
     * Does NOT touch `scopes`, `contextStack`, or `definingTypeStack` —
     * those are reset by ScopedModuleContext alongside this, not here, for
     * the same reason ParserContext::clearErrors() leaves contextStack
     * alone: clearing them here would run before anything had a chance to
     * save the previous module's state.
     */
    void clearErrors() {
        errors.clear();
        hasErrors = false;
        consecutiveErrors = 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guards
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII guard for a transient Scope.
 *
 * Pushes a Scope on construction, pops it (discarding everything it
 * contains) on destruction — automatically, on every exit path.
 *
 * ```cpp
 * void visitBlock(BlockStmtAST* block, SemaContext& ctx) {
 *     ScopedScope guard(ctx);
 *     for (auto* stmt : block->stmts) visitStmt(stmt, ctx);
 *     // scope and everything declared in it is discarded here
 * }
 * ```
 */
struct ScopedScope {
    explicit ScopedScope(SemaContext& ctx) : ctx_(ctx) {
        ctx_.pushScope();
    }

    ~ScopedScope() {
        ctx_.popScope();
    }

    ScopedScope(const ScopedScope&) = delete;
    ScopedScope& operator=(const ScopedScope&) = delete;
    ScopedScope(ScopedScope&&) = delete;
    ScopedScope& operator=(ScopedScope&&) = delete;

private:
    SemaContext& ctx_;
};

/**
 * @brief RAII guard for semantic context tracking.
 *
 * Pushes a SemanticContext frame on construction and pops it on
 * destruction — automatically, on every exit path.
 *
 * ```cpp
 * void visitFunction(FuncDeclAST* func, SemaContext& ctx) {
 *     ScopedSemanticContext guard(ctx, SemanticContext::FuncBody,
 *                                  func, func->loc);
 *     // ctx.insideFunction() now returns true; return is legal
 * }
 * ```
 */
struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, SemanticContext kind,
                           BaseAST* node, const SourceLocation& loc)
        : ctx_(ctx) {
        ctx_.pushContext(kind, node, loc);
    }

    ~ScopedSemanticContext() {
        ctx_.popContext();
    }

    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext(ScopedSemanticContext&&) = delete;
    ScopedSemanticContext& operator=(ScopedSemanticContext&&) = delete;

private:
    SemaContext& ctx_;
};

/**
 * @brief RAII guard marking a TypeDeclAST as "currently being defined".
 *
 * ```cpp
 * void visitStruct(StructDeclAST* s, SemaContext& ctx) {
 *     ctx.insertType(s->name, s);      // visible to itself from here on
 *     ScopedTypeDefinition defining(ctx, s);
 *     ScopedScope genericsGuard(ctx);  // if s has generic params
 *     for (auto* g : s->genericParams) ctx.insertGenericParam(g->name, g);
 *     for (auto* f : s->fields) resolveFieldType(f, ctx);  // Node self-ref OK
 * }
 * ```
 */
struct ScopedTypeDefinition {
    ScopedTypeDefinition(SemaContext& ctx, TypeDeclAST* decl) : ctx_(ctx) {
        ctx_.beginDefiningType(decl);
    }

    ~ScopedTypeDefinition() {
        ctx_.endDefiningType();
    }

    ScopedTypeDefinition(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition& operator=(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition(ScopedTypeDefinition&&) = delete;
    ScopedTypeDefinition& operator=(ScopedTypeDefinition&&) = delete;

private:
    SemaContext& ctx_;
};

/**
 * @brief RAII guard for entering a fresh module's analysis state.
 *
 * SemaContext is shared across the whole semantic phase — every module in
 * `ctx.modules` — so whichever pass drives analysis needs each module to
 * start with clean transient state (empty scopes, empty context stack,
 * empty defining-type stack, empty per-module error list) without
 * permanently discarding whatever the previously-analyzed module
 * accumulated. This guard:
 *
 *   1. Saves the previous `currentModule`/`currentModuleTable` and all
 *      transient state.
 *   2. Calls `ctx.enterModule(module)` — switching (and, if needed,
 *      creating) the persistent ModuleTable for `module`.
 *   3. Resets `scopes`, `contextStack`, `definingTypeStack`, and the
 *      per-module error-tracking fields so `module` starts clean.
 *   4. On destruction: drains this module's `errors` into
 *      `ctx.allDiagnostics` (so they survive instead of vanishing when the
 *      previous module's state is restored), then restores everything
 *      saved in step 1.
 *
 * This is the direct semantic-phase counterpart of the parser's
 * ScopedFileContext — see that struct's doc comment in ParserContext.hpp
 * for the fuller discussion of why the drain-then-restore ordering matters
 * (in short: restoring first would silently discard this module's
 * diagnostics before they were ever saved anywhere durable).
 *
 * ## Usage
 *
 * ```cpp
 * void analyze(SemaContext& ctx) {
 *     for (ModuleAST* mod : ctx.modules) {
 *         ScopedModuleContext moduleContext(ctx, mod);
 *         analyzeModule(mod, ctx);   // starts clean; errors preserved on exit
 *     }
 * }
 * ```
 *
 * Non-copyable, non-movable: identity is tied to one specific activation.
 */
struct ScopedModuleContext {
    ScopedModuleContext(SemaContext& ctx, ModuleAST* module)
        : ctx_(ctx)
        , savedModule_(ctx.currentModule)
        , savedModuleTable_(ctx.currentModuleTable)
        , savedScopes_(std::move(ctx.scopes))
        , savedContextStack_(std::move(ctx.contextStack))
        , savedDefiningTypeStack_(std::move(ctx.definingTypeStack))
        , savedErrors_(std::move(ctx.errors))
        , savedHasErrors_(ctx.hasErrors)
        , savedConsecutiveErrors_(ctx.consecutiveErrors)
    {
        ctx_.scopes.clear();
        ctx_.contextStack.clear();
        ctx_.definingTypeStack.clear();
        ctx_.clearErrors();
        ctx_.enterModule(module);
    }

    ~ScopedModuleContext() {
        ctx_.allDiagnostics.insert(ctx_.allDiagnostics.end(),
                                    ctx_.errors.begin(), ctx_.errors.end());

        ctx_.currentModule      = savedModule_;
        ctx_.currentModuleTable = savedModuleTable_;
        ctx_.scopes             = std::move(savedScopes_);
        ctx_.contextStack       = std::move(savedContextStack_);
        ctx_.definingTypeStack  = std::move(savedDefiningTypeStack_);
        ctx_.errors             = std::move(savedErrors_);
        ctx_.hasErrors          = savedHasErrors_;
        ctx_.consecutiveErrors  = savedConsecutiveErrors_;
    }

    ScopedModuleContext(const ScopedModuleContext&) = delete;
    ScopedModuleContext& operator=(const ScopedModuleContext&) = delete;
    ScopedModuleContext(ScopedModuleContext&&) = delete;
    ScopedModuleContext& operator=(ScopedModuleContext&&) = delete;

private:
    SemaContext& ctx_;
    ModuleAST* savedModule_;
    ModuleTable* savedModuleTable_;
    std::vector<Scope> savedScopes_;
    std::vector<SemanticFrame> savedContextStack_;
    std::vector<TypeDeclAST*> savedDefiningTypeStack_;
    std::vector<Diagnostic> savedErrors_;
    bool savedHasErrors_;
    int savedConsecutiveErrors_;
};

/**
 * @brief ScopedScope / ScopedSemanticContext / ScopedModuleContext — how
 *        they differ.
 *
 * All three are RAII save/reset/restore-style guards, non-copyable and
 * non-movable for the same reason (identity tied to one specific
 * activation), and all exist to eliminate the same class of bug: state
 * that must be manually balanced across multiple exit paths, where one
 * forgotten pop/restore silently corrupts everything analyzed afterward.
 * Beyond that, they operate at different granularities:
 *
 * **ScopedScope:**
 * 1. Guards one `Scope` (values/types/genericParams for one construct).
 * 2. Pushes one Scope, pops (and discards) it on exit.
 * 3. Constructed by whichever pass function owns a construct with local
 *    names — a block, a function body, a generic parameter list.
 * 4. Frequency per module: many.
 * 5. Answers: "what names are visible *only* within this specific nested
 *    construct, forgotten the moment it's done?"
 *
 * **ScopedSemanticContext:**
 * 1. Guards `contextStack` only (not any Scope).
 * 2. Pushes one SemanticFrame, pops it on exit.
 * 3. Constructed alongside (but independently of) ScopedScope, wherever a
 *    pass enters a construct that changes what's semantically legal
 *    (return/break/await), not merely what's visible.
 * 4. Frequency per module: many, but strictly fewer than ScopedScope
 *    activations — a plain nested block opens a Scope without opening a
 *    new SemanticContext frame (see SemanticContext's own doc comment).
 * 5. Answers: "is `return`/`break`/`await` legal right here?"
 *
 * **ScopedModuleContext:**
 * 1. Guards `currentModule`/`currentModuleTable` **and** `scopes`/
 *    `contextStack`/`definingTypeStack`/`errors`/`hasErrors`/
 *    `consecutiveErrors` — i.e. everything transient, all at once.
 * 2. Saves the whole set of fields, resets them to a clean state for the
 *    new module, restores the saved values on exit.
 * 3. Constructed by the analysis driver only — exactly once per module.
 * 4. Frequency per module: exactly one — the whole module is one
 *    activation.
 * 5. Answers: "whose module's transient state is currently live in `ctx`,
 *    given `ctx` is shared across every module in the semantic phase?"
 *
 * The relationship: ScopedScope and ScopedSemanticContext frames are
 * always relative to whichever module is currently being analyzed.
 * ScopedModuleContext is what makes "currently being analyzed" a
 * well-defined, isolated notion in the first place — without it, a Scope
 * pushed while analyzing one module could end up sitting on the same stack
 * as frames from a different module, and a diagnostic discovered in one
 * module could be silently discarded the moment analysis moved to the next.
 */