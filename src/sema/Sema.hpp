/**
 * @file Sema.hpp
 * @brief Lucid semantic analyzer – validates and annotates a parsed AST.
 *
 * @architectural_note Relationship to SemaContext
 *   Sema.hpp is to SemaContext.hpp what Parser.hpp is to ParserContext.hpp/
 *   TokenStream.hpp: SemaContext holds the shared state (persistent
 *   per-module symbol tables, the transient scope stack, the semantic
 *   context stack, diagnostics) and the tools to read/write it
 *   (insertValue/insertType, lookupValue/lookupType, pushScope/popScope,
 *   error/errorAt, ...). Every function declared here is what actually
 *   *drives* that state — deciding, for a given AST node, when to insert,
 *   when to look up, what a missing lookup means, and which language rule
 *   was or wasn't satisfied. SemaContext never calls these; these call it.
 *
 * @architectural_note One traversal, not two
 *   Lucid requires an explicit type annotation on every declaration (no
 *   inference — see VarDeclAST::type, FieldDeclAST::type, ParamAST::type
 *   in DeclAST.hpp), so there is no structural need for name resolution and
 *   type checking to be separate passes the way they are in languages that
 *   must resolve names before they can infer types. Every `analyze*`/
 *   `check*` function below therefore resolves names AND checks types in
 *   the same single traversal, consistent with SemaContext's single-flow
 *   design (see SemaContext.hpp's file-level doc comment): a symbol is
 *   inserted the moment its declaration is reached, and every lookup only
 *   ever sees what has been inserted so far. FFI validation against the
 *   foreign function manifest remains conceptually separate (a different
 *   external source of truth), but is still invoked inline as each
 *   `@[foreign("C")]`-attributed declaration is reached, not as a
 *   subsequent pass over the whole program.
 *
 * @architectural_note Per-module flow
 *   `Sema::analyze()` wraps one module's analysis in a ScopedModuleContext
 *   (see SemaContext.hpp), then walks that module's top-level declarations
 *   in source order, calling `analyzeDecl()` for each — which is what
 *   inserts the declaration's own name (so a struct can reference itself,
 *   see below) before recursing into its internals. There is no separate
 *   "collect all top-level names first" pass; see SemaContext.hpp's
 *   "Why single-flow, not collect-then-check" note for the rationale.
 *
 * @architectural_note Self-reference
 *   `analyzeStructDecl()` (and, symmetrically, `analyzeEnumDecl()` /
 *   `analyzeTraitDecl()`) call `ctx.insertType()` for their own name, then
 *   open a `ScopedTypeDefinition` guard, THEN walk their fields/variants —
 *   so `Node`'s own `next ptr<Node>?` field resolves correctly, and
 *   `checkRecursiveFieldType()` can tell (via `ctx.isDefiningType()`)
 *   whether a field's use of the enclosing type is direct (illegal,
 *   infinite size) or indirect through `ptr`/`&`/`?` (legal).
 */

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "SemaContext.hpp"

#include <vector>
#include <optional>

// =============================================================================
// Public API - Single Entry Point
// =============================================================================

/**
 * @brief Entry-point holder for the semantic phase.
 *
 * Mirrors the parser's "one and only entry point" convention (`parse()`),
 * expressed as a small static-method class rather than a free function so
 * call sites read as `Sema::analyze(module, ctx)` — see the sema/
 * directory's own architecture note for why this is spelled `Sema::analyze`
 * rather than a bare `analyze()`.
 */
class Sema {
public:
    /**
     * @brief Analyze a single module: validate and annotate its AST in place.
     *
     * This is the ONE entry point for analyzing one module. It wraps the
     * whole call in a `ScopedModuleContext`, so `ctx` starts with a clean
     * transient state (empty scope stack, empty semantic-context stack,
     * empty defining-type stack, empty per-module error list) and ends with
     * this module's diagnostics drained into `ctx.allDiagnostics` — exactly
     * once per module, regardless of how many times `analyze()` is called
     * across the whole compilation.
     *
     * @param module The module to analyze. Must be non-null; a module that
     *        failed to parse cleanly is still analyzed (check
     *        `module->hasErrors` beforehand if you want to skip that).
     * @param ctx    The semantic context (shared across every module in
     *        this compilation — see SemaContext's own doc comment).
     *
     * @note Does not decide inter-module ordering. If module A imports
     *       module B, the caller is responsible for analyzing B first (or
     *       for `ctx.registry` already holding B's export table) — this
     *       function only handles what's needed to analyze `module` itself.
     */
    static void analyze(ModuleAST* module, SemaContext& ctx);

    /**
     * @brief Analyze every module in `ctx.modules`, in the order stored there.
     *
     * A convenience default for callers that don't yet have real dependency
     * ordering wired up (see ModuleRegistry). Equivalent to calling
     * `analyze()` once per module in `ctx.modules` order. Prefer calling
     * `analyze()` directly, module by module, once real import-dependency
     * ordering exists.
     */
    static void analyzeAll(SemaContext& ctx);
};

// =============================================================================
// Module-Level Analysis
// =============================================================================

/**
 * @brief Walk a single module's top-level declarations in source order.
 *
 * @param module    The module whose `decls` span is walked.
 * @param ctx       The semantic context. Caller (`Sema::analyze`) is
 *        responsible for having already entered `module` via
 *        ScopedModuleContext — this function assumes `ctx.currentModule`
 *        and `ctx.currentModuleTable` are already correct.
 *
 * Always processes every declaration it can, even after individual
 * declarations report errors — check `ctx.hasErrors`, not a return value,
 * to tell a clean analysis from one that hit errors along the way. Stops
 * early only if `ctx.canContinue()` becomes false (too many consecutive
 * errors), mirroring the parser's own fatal-failure threshold.
 */
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx);

// =============================================================================
// Declarations
// =============================================================================

/**
 * @brief Dispatch a single declaration to its specific `analyze*` function.
 *
 * This is the function that performs the "insert this name, THEN recurse
 * into its internals" ordering that makes self-reference and
 * declare-before-use both work — see this file's own architecture notes.
 */
void analyzeDecl(DeclAST* decl, SemaContext& ctx);

void analyzeImportDecl(ImportDeclAST* decl, SemaContext& ctx);
void analyzeVarDecl(VarDeclAST* decl, SemaContext& ctx);
void analyzeFuncDecl(FuncDeclAST* decl, SemaContext& ctx);
void analyzeEnumDecl(EnumDeclAST* decl, SemaContext& ctx);
void analyzeTraitDecl(TraitDeclAST* decl, SemaContext& ctx);
void analyzeStructDecl(StructDeclAST* decl, SemaContext& ctx);

/**
 * @brief Analyze one field of a struct or (see analyzeTraitField) trait.
 *
 * @param field The field being analyzed.
 * @param owner The struct that declares it — unlike the parser's
 *        `parseFieldDecl` (pure syntax, no owner needed), semantic analysis
 *        genuinely needs the owner: duplicate-name checking within the
 *        struct, and `checkRecursiveFieldType()`'s direct-vs-indirect
 *        self-reference check, both require knowing which type this field
 *        belongs to.
 */
void analyzeFieldDecl(FieldDeclAST* field, StructDeclAST* owner, SemaContext& ctx);

/// @param owner The enum this variant belongs to (duplicate-value checking).
void analyzeEnumVariant(EnumVariantAST* variant, EnumDeclAST* owner, SemaContext& ctx);

/// @param owner The trait this field requirement belongs to.
void analyzeTraitField(TraitFieldDeclAST* field, TraitDeclAST* owner, SemaContext& ctx);

void analyzeParam(ParamAST* param, SemaContext& ctx);
void analyzeGenericParamDecl(GenericParamDeclAST* param, SemaContext& ctx);

// =============================================================================
// Statements
// =============================================================================

/**
 * @brief Analyze a statement.
 *
 * @return true if this statement is guaranteed to transfer control out of
 *         the enclosing block on every path (`return`, `break`, `continue`,
 *         or a block whose own last statement guarantees it) — false
 *         otherwise. This is what `analyzeFuncDecl()` reads to decide
 *         whether a non-void function is missing a return, and what
 *         `analyzeSwitchStmt()` reads to decide whether every case
 *         diverges. Purely a control-flow fact; it says nothing about
 *         whether the statement type-checked cleanly.
 */
bool analyzeStmt(StmtAST* stmt, SemaContext& ctx);

/**
 * @brief Analyze a block. Opens and closes exactly one `ScopedScope` (see
 *        SemaContext.hpp) around the statements it contains.
 */
bool analyzeBlock(BlockStmtAST* block, SemaContext& ctx);

bool analyzeIfStmt(IfStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchStmt(SwitchStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchCase(SwitchCaseAST* switchCase, SemaContext& ctx);
bool analyzeForStmt(ForStmtAST* stmt, SemaContext& ctx);
bool analyzeWhileStmt(WhileStmtAST* stmt, SemaContext& ctx);
bool analyzeDoWhileStmt(DoWhileStmtAST* stmt, SemaContext& ctx);
bool analyzeReturnStmt(ReturnStmtAST* stmt, SemaContext& ctx);
bool analyzeBreakStmt(BreakStmtAST* stmt, SemaContext& ctx);
bool analyzeContinueStmt(ContinueStmtAST* stmt, SemaContext& ctx);
bool analyzeExprStmt(ExprStmtAST* stmt, SemaContext& ctx);
bool analyzeDeclStmt(DeclStmtAST* stmt, SemaContext& ctx);
bool analyzeMultiVarDecl(MultiVarDeclAST* stmt, SemaContext& ctx);
bool analyzeMultiAssignStmt(MultiAssignStmtAST* stmt, SemaContext& ctx);

// ─── Concurrency ───────────────────────────────────────────────────────── // X

bool analyzeAsyncStmt(AsyncStmtAST* stmt, SemaContext& ctx);
bool analyzeAwaitStmt(AwaitStmtAST* stmt, SemaContext& ctx);
bool analyzeSpawnStmt(SpawnStmtAST* stmt, SemaContext& ctx);
bool analyzeJoinStmt(JoinStmtAST* stmt, SemaContext& ctx);

// =============================================================================
// Expressions (Type Checking)
// =============================================================================

/**
 * @brief Resolve and type-check an expression.
 *
 * Every `check*` function below sets `expr->resolvedType` (see
 * `ExprAST::resolvedType` in BaseAST.hpp) before returning, in addition to
 * returning the same pointer, so callers that only care about the
 * side-effect (annotating the AST) don't have to use the return value.
 *
 * @return The expression's resolved type, or nullptr if it could not be
 *         determined (an error was already reported; callers should treat
 *         nullptr as "already handled", not report a second diagnostic).
 */
TypeAST* checkExpr(ExprAST* expr, SemaContext& ctx);

TypeAST* checkLiteralExpr(LiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkIdentifierExpr(IdentifierExprAST* expr, SemaContext& ctx);
TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkStructLiteralExpr(StructLiteralExprAST* expr, SemaContext& ctx);

/// @param targetStruct The struct type this initializer is constructing.
void checkFieldInit(FieldInitAST* init, StructDeclAST* targetStruct, SemaContext& ctx);

TypeAST* checkBinaryExpr(BinaryExprAST* expr, SemaContext& ctx);
TypeAST* checkUnaryExpr(UnaryExprAST* expr, SemaContext& ctx);
TypeAST* checkCallExpr(CallExprAST* expr, SemaContext& ctx);
TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, SemaContext& ctx);
TypeAST* checkIndexExpr(IndexExprAST* expr, SemaContext& ctx);
TypeAST* checkSliceExpr(SliceExprAST* expr, SemaContext& ctx);
TypeAST* checkFieldAccessExpr(FieldAccessExprAST* expr, SemaContext& ctx);

/**
 * @brief Type-check `module:member` access.
 *
 * Resolves `expr`'s module alias via `ctx.lookupImport()` against the
 * *current* module's table, then looks the member up in the target
 * module's own persistent `ModuleTable` (via
 * `ctx.findModuleTable()`/`getOrCreateModuleTable()`) — never through the
 * current module's scopes, since an imported symbol is by definition not
 * local. See SemaContext.hpp's "Lookup Rules" note.
 */
TypeAST* checkModuleAccessExpr(ModuleAccessExprAST* expr, SemaContext& ctx);

TypeAST* checkNullableChainExpr(NullableChainExprAST* expr, SemaContext& ctx);
TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST* expr, SemaContext& ctx);
TypeAST* checkAssignExpr(AssignExprAST* expr, SemaContext& ctx);
TypeAST* checkPipelineExpr(PipelineExprAST* expr, SemaContext& ctx);

/// @param inputType The type flowing into this step from the previous one.
TypeAST* checkPipelineStep(PipelineStepAST* step, TypeAST* inputType, SemaContext& ctx);

TypeAST* checkComposeExpr(ComposeExprAST* expr, SemaContext& ctx);
TypeAST* checkComposeOperand(ComposeOperandAST* operand, SemaContext& ctx);
TypeAST* checkAnonFuncExpr(AnonFuncExprAST* expr, SemaContext& ctx);
TypeAST* checkIfExpr(IfExprAST* expr, SemaContext& ctx);
TypeAST* checkRangeExpr(RangeExprAST* expr, SemaContext& ctx);

// =============================================================================
// Types (Type Resolution)
// =============================================================================

/**
 * @brief Resolve a type annotation as written in source.
 *
 * See TypeAST.hpp's file-level note: TypeAST nodes represent types *as
 * written*; resolution here means confirming every named type actually
 * exists in the current context (`ctx.lookupType()`), tagging
 * `NamedTypeAST::isGenericParam` when a name shadows a generic parameter
 * instead of a real type (see NamedTypeAST's own doc comment), and
 * recursing into compound types (array element, ref/ptr inner type,
 * function parameter/return types). Does not allocate a distinct "resolved
 * type" node — the same TypeAST pointer is annotated and returned.
 */
TypeAST* resolveType(TypeAST* type, SemaContext& ctx);

TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve a named type reference.
 *
 * Checks `ctx.lookupGenericParam()` first (a name shadows a real type while
 * it's an in-scope generic parameter — see NamedTypeAST's `isGenericParam`
 * doc comment in TypeAST.hpp) before falling back to `ctx.lookupType()`.
 * Reports an error via `resolveTypeNameOrError()` if neither resolves.
 */
TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx);

TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx);
TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx);
TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx);
TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx);
TypeAST* resolveRefType(RefTypeAST* type, SemaContext& ctx);
TypeAST* resolvePtrType(PtrTypeAST* type, SemaContext& ctx);
TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx);

// =============================================================================
// Generics & Traits
// =============================================================================

/// Resolve a trait reference (`: Vector2`, `<T : Container<int>>`) to its declaration.
TraitDeclAST* resolveTraitRef(TraitRefAST* ref, SemaContext& ctx);

/**
 * @brief Verify every generic parameter of `owner` is used by at least one
 *        field/param type. Unused parameters are a compile error — see
 *        StructDeclAST's and TraitDeclAST's own "Semantic Analysis Notes".
 */
void validateGenericParamUsage(ArenaSpan<GenericParamDeclPtr> params,
                                DeclAST* owner,
                                SemaContext& ctx);

/**
 * @brief Verify `structDecl` satisfies every field `traitRef` requires:
 *        matching name, matching type, matching const-ness. See
 *        StructDeclAST's "Semantic Analysis Notes" 1–4 in DeclAST.hpp.
 *
 * @return true if the struct fully implements the trait.
 */
bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitRefAST* traitRef,
                                  SemaContext& ctx);

/// Verify a generic argument list's arity (and, where constrained, that each
/// argument satisfies its parameter's trait bounds) against `params`.
void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       BaseAST* useSite,
                       SemaContext& ctx);

// =============================================================================
// Self-Reference / Recursive Type Validation
// =============================================================================

/**
 * @brief True if `fieldType` refers directly to `owner`'s own type — i.e.
 *        by value, with no `ptr`/`&`/array/nullable indirection breaking
 *        the cycle — which would make `owner` an infinite-size type.
 *
 * Reads `ctx.isDefiningType(owner)` to know whether `owner` is still being
 * defined (see SemaContext.hpp's `definingTypeStack`); a resolved type that
 * happens to equal `owner` after `owner` has already finished being defined
 * is a completed type used normally, not a same-definition self-reference,
 * so this only flags the in-progress case.
 */
bool isDirectSelfReference(TypeAST* fieldType, TypeDeclAST* owner, SemaContext& ctx);

/**
 * @brief Resolve `field`'s type and reject it if `isDirectSelfReference()`
 *        holds — the one validation step specific to fields of a type
 *        that's still being defined. Called from `analyzeFieldDecl()`.
 */
void checkRecursiveFieldType(FieldDeclAST* field, TypeDeclAST* owner, SemaContext& ctx);

// =============================================================================
// FFI Validation                                                        // X
// =============================================================================

/**
 * @brief Validate an `@[foreign("C")]`-attributed function against the
 *        foreign function manifest.
 *
 * @note Full behavior depends on FFIValidator's manifest format
 *       (`lge_ffi.lfi`), not yet finalized — signature settled so callers
 *       (`analyzeFuncDecl()`) can wire the call site up now.
 */
void validateForeignFunc(FuncDeclAST* decl, AttributeAST* foreignAttr, SemaContext& ctx); // X

/// True if `type` is legal at an FFI boundary (see PtrTypeAST's "Valid
/// contexts for PtrTypeAST" note in TypeAST.hpp for the pointer-specific
/// half of this rule).
bool isValidFFIType(TypeAST* type, SemaContext& ctx); // X

// =============================================================================
// Attributes
// =============================================================================

/// Validate every attribute in `attrs` against what's legal on `owner`'s kind
/// of declaration (e.g. `@[foreign("C")]` only on functions, `@[export]` only
/// at module top level).
void validateAttributes(ArenaSpan<AttributePtr> attrs, DeclAST* owner, SemaContext& ctx);
void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// =============================================================================
// Resolution Helpers (lookup + diagnostic on failure)
// =============================================================================

/**
 * @brief Resolve an identifier expression to its declaration, reporting an
 *        "undefined value" diagnostic if it doesn't resolve.
 *
 * This is the "decide what a missing lookup means" half of resolution that
 * `SemaContext::lookupValue()` deliberately does not do itself (it just
 * returns nullptr) — see the discussion on why NameResolver-level logic
 * can't live in SemaContext.
 */
ValueDeclAST* resolveValueOrError(IdentifierExprAST* expr, SemaContext& ctx);

/// Resolve a named type reference, reporting an "undefined type" diagnostic
/// if it doesn't resolve as either a real type or a generic parameter.
TypeDeclAST* resolveTypeNameOrError(NamedTypeAST* type, SemaContext& ctx);

/// Resolve a call's callee to the function it names, reporting a
/// "not callable" diagnostic if it resolves to a non-function value.
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx);

/**
 * @brief Get (creating if necessary) the cached self-type reference for
 *        `decl` — see `TypeDeclAST::selfType`'s own doc comment in
 *        BaseAST.hpp for why this is cached rather than rebuilt on every
 *        use (e.g. `int("42")`-style conversions, or a method-less struct's
 *        name appearing where a value is expected).
 */
NamedTypeAST* selfTypeOf(TypeDeclAST* decl, SemaContext& ctx);

// =============================================================================
// Type Compatibility Helpers
// =============================================================================

/// Structural equality of two resolved types (same kind, same named target,
/// same array kind/size, etc.) — NOT identity comparison.
bool typesEqual(const TypeAST* a, const TypeAST* b);

/**
 * @brief True if a value of type `source` may be used where `target` is
 *        expected (assignment, argument passing, return).
 *
 * Handles the qualifier rules documented on NullableTypeAST/FallibleTypeAST/
 * CombinedTypeAST in TypeAST.hpp — e.g. `T` is assignable to `T?` but not
 * vice versa without narrowing.
 */
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);

/// Strip one layer of `?`/`?!`, returning the inner type (or `type` itself
/// if it wasn't nullable/combined).
TypeAST* unwrapNullable(TypeAST* type);

/// Strip one layer of `!`/`?!`, returning the inner type (or `type` itself
/// if it wasn't fallible/combined).
TypeAST* unwrapFallible(TypeAST* type);

bool isNumericType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);