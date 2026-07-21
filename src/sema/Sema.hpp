/**
 * @file Sema.hpp
 * @brief Lucid semantic analyzer – validates and annotates parsed ASTs.
 *
 * @architectural_note Namespace design
 *   The semantic phase follows the same namespace + context pattern as the
 *   parser: `sema::analyze()` is the entry point, and `SemaContext` holds
 *   the shared state. This is deliberate and consistent with the parser's
 *   `parser::parse()` / `ParserContext` design.
 *
 * @architectural_note Single entry point
 *   `sema::analyze(modules, ctx)` is the ONE entry point for semantic
 *   analysis. It processes every module in `modules` in the order provided
 *   (which should be dependency order - imports before dependents). Each
 *   module is analyzed in its own `ScopedModuleContext`, ensuring transient
 *   state doesn't leak between modules.
 *
 * @architectural_note Relationship to SemaContext
 *   Sema.hpp is to SemaContext.hpp what Parser.hpp is to ParserContext.hpp:
 *   SemaContext holds the shared state (persistent per-module symbol tables,
 *   the transient scope stack, the semantic context stack, diagnostics) and
 *   the tools to read/write it. Every function declared here drives that
 *   state — deciding, for a given AST node, when to insert, when to look up,
 *   what a missing lookup means, and which language rule was or wasn't
 *   satisfied.
 *
 * @architectural_note One traversal, not two
 *   Lucid requires an explicit type annotation on every declaration, so there
 *   is no structural need for name resolution and type checking to be separate
 *   passes. Every `analyze*`/`check*` function resolves names AND checks types
 *   in the same single traversal.
 *
 * @architectural_note Per-module flow
 *   `sema::analyze()` wraps each module's analysis in a ScopedModuleContext,
 *   then walks that module's top-level declarations in source order, calling
 *   `analyzeDecl()` for each — which inserts the declaration's own name before
 *   recursing into its internals.
 *
 * @architectural_note Self-reference
 *   `analyzeStructDecl()` (and symmetrically `analyzeEnumDecl()` /
 *   `analyzeTraitDecl()`) call `ctx.symbols.insertType()` for their own name,
 *   then open a `ScopedTypeDefinition` guard, THEN walk their fields/variants —
 *   so `Node`'s own `next ptr<Node>?` field resolves correctly.
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
#include "context/SemaContext.hpp"

#include <vector>
#include <optional>

// =============================================================================
// Public API - Single Entry Point
// =============================================================================

namespace sema {

/**
 * @brief Analyze all modules in the program.
 *
 * This is the ONE entry point for semantic analysis. It processes every
 * module in `modules` in the order provided (which should be dependency
 * order - imports before dependents), using `ctx` for shared state.
 *
 * Each module is analyzed in its own `ScopedModuleContext`, ensuring
 * transient state (scopes, context stack, defining-type stack) doesn't
 * leak between modules, while persistent state (ModuleTable) is preserved
 * across the whole compilation.
 *
 * @param modules The modules to analyze (in dependency order - imports first).
 * @param ctx     The semantic context (shared across all modules).
 *
 * @note If module A imports module B, the caller is responsible for ensuring
 *       B appears before A in `modules`. The semantic phase does not reorder
 *       modules or resolve inter-module dependencies.
 */
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx);

// =============================================================================
// Module-Level Analysis (internal - declared for implementation)
// =============================================================================

/**
 * @brief Walk a single module's top-level declarations in source order.
 *
 * Internal function used by `sema::analyze()` for each module.
 *
 * @param module The module whose `decls` span is walked.
 * @param ctx    The semantic context. Caller (`sema::analyze`) is
 *        responsible for having already entered `module` via
 *        ScopedModuleContext — this function assumes `ctx.symbols.currentModule()`
 *        and `ctx.symbols.currentModuleTable()` are already correct.
 *
 * Always processes every declaration it can, even after individual
 * declarations report errors — check `ctx.canContinue()`, not a return value,
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
 * declare-before-use both work.
 */
void analyzeDecl(DeclAST* decl, SemaContext& ctx);

void analyzeImportDecl(ImportDeclAST* decl, SemaContext& ctx);
void analyzeVarDecl(VarDeclAST* decl, SemaContext& ctx);
void analyzeFuncDecl(FuncDeclAST* decl, SemaContext& ctx);
void analyzeEnumDecl(EnumDeclAST* decl, SemaContext& ctx);
void analyzeTraitDecl(TraitDeclAST* decl, SemaContext& ctx);
void analyzeStructDecl(StructDeclAST* decl, SemaContext& ctx);

/**
 * @brief Analyze one field of a struct.
 *
 * @param field The field being analyzed.
 * @param owner The struct that declares it — needed for duplicate-name
 *        checking and `checkRecursiveFieldType()`'s self-reference check.
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
 *         whether a non-void function is missing a return.
 */
bool analyzeStmt(StmtAST* stmt, SemaContext& ctx);

/**
 * @brief Analyze a block. Opens and closes exactly one `ScopedScope`.
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

// ─── Concurrency ─────────────────────────────────────────────────────────

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
 * Every `check*` function below sets `expr->resolvedType` before returning.
 *
 * @return The expression's resolved type, or nullptr if it could not be
 *         determined (an error was already reported; callers should treat
 *         nullptr as "already handled").
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
 * Resolves `expr`'s module alias via `ctx.symbols.lookupImport()` against the
 * *current* module's table, then looks the member up in the target
 * module's own persistent `ModuleTable`.
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
 * Confirms every named type exists in the current context, tags
 * `NamedTypeAST::isGenericParam` when a name shadows a generic parameter,
 * and recurses into compound types.
 */
TypeAST* resolveType(TypeAST* type, SemaContext& ctx);

TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve a named type reference.
 *
 * Checks `ctx.symbols.lookupGenericParam()` first (a name shadows a real type
 * while it's an in-scope generic parameter) before falling back to
 * `ctx.symbols.lookupType()`. Reports an error via `resolveTypeNameOrError()`
 * if neither resolves.
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

/// Resolve a trait reference to its declaration.
TraitDeclAST* resolveTraitRef(TraitRefAST* ref, SemaContext& ctx);

/**
 * @brief Verify every generic parameter of `owner` is used by at least one
 *        field/param type. Unused parameters are a compile error.
 */
void validateGenericParamUsage(ArenaSpan<GenericParamDeclPtr> params,
                                DeclAST* owner,
                                SemaContext& ctx);

/**
 * @brief Verify `structDecl` satisfies every field `traitRef` requires:
 *        matching name, matching type, matching const-ness.
 *
 * @return true if the struct fully implements the trait.
 */
bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitRefAST* traitRef,
                                  SemaContext& ctx);

/// Verify a generic argument list's arity and constraints against `params`.
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
 */
bool isDirectSelfReference(TypeAST* fieldType, TypeDeclAST* owner, SemaContext& ctx);

/**
 * @brief Resolve `field`'s type and reject it if `isDirectSelfReference()`
 *        holds — the one validation step specific to fields of a type
 *        that's still being defined.
 */
void checkRecursiveFieldType(FieldDeclAST* field, TypeDeclAST* owner, SemaContext& ctx);

// =============================================================================
// FFI Validation
// =============================================================================

/**
 * @brief Validate an `@[foreign("C")]`-attributed function against the
 *        foreign function manifest.
 */
void validateForeignFunc(FuncDeclAST* decl, AttributeAST* foreignAttr, SemaContext& ctx);

/// True if `type` is legal at an FFI boundary.
bool isValidFFIType(TypeAST* type, SemaContext& ctx);

// =============================================================================
// Attributes
// =============================================================================

/// Validate every attribute in `attrs` against what's legal on `owner`'s kind.
void validateAttributes(ArenaSpan<AttributePtr> attrs, DeclAST* owner, SemaContext& ctx);
void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// =============================================================================
// Resolution Helpers (lookup + diagnostic on failure)
// =============================================================================

/**
 * @brief Resolve an identifier expression to its declaration, reporting an
 *        "undefined value" diagnostic if it doesn't resolve.
 */
ValueDeclAST* resolveValueOrError(IdentifierExprAST* expr, SemaContext& ctx);

/// Resolve a named type reference, reporting an "undefined type" diagnostic.
TypeDeclAST* resolveTypeNameOrError(NamedTypeAST* type, SemaContext& ctx);

/// Resolve a call's callee to the function it names, reporting a
/// "not callable" diagnostic if it resolves to a non-function value.
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx);

/**
 * @brief Get (creating if necessary) the cached self-type reference for `decl`.
 */
NamedTypeAST* selfTypeOf(TypeDeclAST* decl, SemaContext& ctx);

// =============================================================================
// Type Compatibility Helpers
// =============================================================================

/// Structural equality of two resolved types.
bool typesEqual(const TypeAST* a, const TypeAST* b);

/**
 * @brief True if a value of type `source` may be used where `target` is
 *        expected (assignment, argument passing, return).
 */
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);

/// Strip one layer of `?`/`?!`, returning the inner type.
TypeAST* unwrapNullable(TypeAST* type);

/// Strip one layer of `!`/`?!`, returning the inner type.
TypeAST* unwrapFallible(TypeAST* type);

bool isNumericType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);

} // namespace sema