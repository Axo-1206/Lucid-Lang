/**
 * @file Sema.hpp
 * @brief Lucid semantic analyzer – validates and annotates parsed ASTs.
 *
 * @architectural_note Two distinct operations: REGISTER vs LOOKUP
 *
 *   REGISTER (insert into symbol table):
 *     - Called when we encounter a declaration that defines a new name
 *     - Happens BEFORE analyzing the declaration's internals (for self-reference)
 *     - Examples: struct, enum, trait, function, variable, generic parameter
 *     - Registration order matters: insert name first, then analyze internals
 *
 *   LOOKUP (search for an existing name):
 *     - Called when we encounter a reference to a name
 *     - Searches: generic params → local scopes → module scope
 *     - Examples: type name in annotation, function call, variable reference
 *     - Reports E2001/E2002 if not found
 *
 * @architectural_note Registering generic parameters
 *   Generic parameters are registered in the current scope's `genericParams` map
 *   BEFORE analyzing the declaration's body. This allows `T` in `struct Box<T>`
 *   to be found when resolving field types like `value T`.
 *
 * @architectural_note Type declaration registration order
 *   For `struct Node<T> { value T, next ptr<Node<T>>? }`:
 *   1. Register `Node` in type namespace (so `Node` can reference itself)
 *   2. Register `T` in genericParams (so `T` can be used in fields)
 *   3. Analyze fields (now both `Node` and `T` are findable)
 *
 * @architectural_note Self-reference via DefiningTypeStack
 *   After registering `Node`, we push it onto DefiningTypeStack. This allows
 *   `isDirectSelfReference()` to detect that `next ptr<Node<T>>?` is an
 *   indirect self-reference (legal) vs `value Node<T>` (illegal, infinite size).
 *
 * @architectural_note AST nodes are read-only
 *   The parser creates and populates all AST nodes. Semantic analysis reads
 *   from them and adds semantic annotations (resolved types, etc.). We never
 *   modify the structure of the AST, only annotate existing nodes.
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
#include "debug/DebugUtils.hpp"
#include "context/SemaContext.hpp"

#include <vector>
#include <optional>

namespace sema {

// =============================================================================
// Public API - Single Entry Point
// =============================================================================

/**
 * @brief Analyze all modules in the program.
 *
 * The ONLY entry point for semantic analysis. Processes modules in
 * dependency order (imports before dependents).
 */
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx);

// =============================================================================
// Module-Level Analysis
// =============================================================================

/** Analyze a module's top-level declarations in source order. */
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx);


void analyzeDecl(const DeclAST* decl, SemaContext& ctx);
void analyzeStructDecl(const StructDeclAST* decl, SemaContext& ctx);
void analyzeEnumDecl(const EnumDeclAST* decl, SemaContext& ctx);
void analyzeTraitDecl(const TraitDeclAST* decl, SemaContext& ctx);

void analyzeImportDecl(const ImportDeclAST* decl, SemaContext& ctx);
void analyzeVarDecl(const VarDeclAST* decl, SemaContext& ctx);
void analyzeFuncDecl(const FuncDeclAST* decl, SemaContext& ctx);
void analyzeParam(const ParamAST* param, SemaContext& ctx);
void analyzeGenericParamDecl(const GenericParamDeclAST* param, SemaContext& ctx);

// =============================================================================
// STATEMENTS - Control flow analysis
// =============================================================================

/**
 * @brief Analyze a statement.
 *
 * @return true if this statement guarantees control transfer out of the block
 *         (return, break, continue, or a block with such a last statement).
 */
bool analyzeStmt(const StmtAST* stmt, SemaContext& ctx);

bool analyzeBlock(const BlockStmtAST* block, SemaContext& ctx);
bool analyzeIfStmt(const IfStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx);
bool analyzeForStmt(const ForStmtAST* stmt, SemaContext& ctx);
bool analyzeWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx);
bool analyzeDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx);
bool analyzeReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx);
bool analyzeBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx);
bool analyzeContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx);
bool analyzeExprStmt(const ExprStmtAST* stmt, SemaContext& ctx);
bool analyzeDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx);
bool analyzeMultiVarDecl(const MultiVarDeclAST* stmt, SemaContext& ctx);
bool analyzeMultiAssignStmt(const MultiAssignStmtAST* stmt, SemaContext& ctx);

// ─── Concurrency ─────────────────────────────────────────────────────────

bool analyzeAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx);
bool analyzeAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx);
bool analyzeSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx);
bool analyzeJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx);

// =============================================================================
// EXPRESSIONS - Type checking (LOOKUP names)
// =============================================================================

/**
 * @brief Type-check an expression.
 *
 * For identifier expressions, this LOOKS UP the name in the symbol table.
 * Sets expr->resolvedType.
 */
TypeAST* checkExpr(const ExprAST* expr, SemaContext& ctx);

TypeAST* checkLiteralExpr(const LiteralExprAST* expr, SemaContext& ctx);

/**
 * @brief Check an identifier expression.
 *
 * LOOKUP: `ctx.symbols.lookupValue(expr->name)`
 *   - Searches: generic params → local scopes → module scope
 *   - Reports E2001 if not found
 *   - Sets resolvedType to the declaration's valueType
 */
TypeAST* checkIdentifierExpr(const IdentifierExprAST* expr, SemaContext& ctx);

TypeAST* checkArrayLiteralExpr(const ArrayLiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkStructLiteralExpr(const StructLiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkBinaryExpr(const BinaryExprAST* expr, SemaContext& ctx);
TypeAST* checkUnaryExpr(const UnaryExprAST* expr, SemaContext& ctx);

/**
 * @brief Check a function call.
 *
 * LOOKUP: Uses resolveCalleeOrError() to find the function declaration.
 *   - Plain call: LOOKUP name in value namespace
 *   - Module call: LOOKUP module alias, then LOOKUP member in module's table
 *   - Other callees: Check callee's resolvedType is a FuncTypeAST
 */
TypeAST* checkCallExpr(const CallExprAST* expr, SemaContext& ctx);

TypeAST* checkIntrinsicCallExpr(const IntrinsicCallExprAST* expr, SemaContext& ctx);
TypeAST* checkIndexExpr(const IndexExprAST* expr, SemaContext& ctx);
TypeAST* checkSliceExpr(const SliceExprAST* expr, SemaContext& ctx);

/**
 * @brief Check field access.
 *
 * LOOKUP: Resolve object's type, then LOOKUP field name in that type's scope.
 *   - For structs: LOOKUP field in struct's fields
 *   - For enums: LOOKUP variant in enum's variants
 */
TypeAST* checkFieldAccessExpr(const FieldAccessExprAST* expr, SemaContext& ctx);

/**
 * @brief Check module access (module:member).
 *
 * LOOKUP:
 *   1. `ctx.symbols.lookupImport(moduleName)` - find imported module
 *   2. `moduleTable->values.find(memberName)` - LOOKUP member in module's table
 */
TypeAST* checkModuleAccessExpr(const ModuleAccessExprAST* expr, SemaContext& ctx);

TypeAST* checkNullableChainExpr(const NullableChainExprAST* expr, SemaContext& ctx);
TypeAST* checkNullCoalesceExpr(const NullCoalesceExprAST* expr, SemaContext& ctx);
TypeAST* checkAssignExpr(const AssignExprAST* expr, SemaContext& ctx);
TypeAST* checkPipelineExpr(const PipelineExprAST* expr, SemaContext& ctx);
TypeAST* checkComposeExpr(const ComposeExprAST* expr, SemaContext& ctx);
TypeAST* checkAnonFuncExpr(const AnonFuncExprAST* expr, SemaContext& ctx);
TypeAST* checkIfExpr(const IfExprAST* expr, SemaContext& ctx);
TypeAST* checkRangeExpr(const RangeExprAST* expr, SemaContext& ctx);

// =============================================================================
// TYPES - LOOKUP names in type namespace
// =============================================================================

/**
 * @brief Resolve a type annotation.
 *
 * For NamedTypeAST: LOOKUP the name.
 * For compound types: recursively resolve inner types.
 *
 * The parser already created all TypeAST nodes. This just validates they exist.
 */
TypeAST* resolveType(const TypeAST* type, SemaContext& ctx);

/** Primitive types are always valid (built-in). */
TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve a named type.
 *
 * LOOKUP PRIORITY (highest to lowest):
 *   1. `ctx.symbols.lookupGenericParam(name)` - generic parameter in current scope
 *   2. `ctx.symbols.lookupType(name)` - type in local scopes
 *   3. `ctx.symbols.lookupType(name)` - type in module scope (fallback)
 *
 * Reports E2002 if not found in any tier.
 *
 * @note Generic parameters have highest priority and shadow type names.
 *       Example: In `struct Box<T>`, `T` is a generic param, not a type.
 */
TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx);

/** Recursively resolve array element type. */
TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx);

/** Recursively resolve inner type. */
TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx);
TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx);
TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve reference type.
 *
 * Checks Downward Flow Rule:
 *   - Cannot store &T in struct fields (uses ctx.definingTypes.current())
 *   - Cannot store &T in arrays
 *   - Cannot return &T from functions
 */
TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx);

/** Resolve pointer type - always valid (sealed conduit). */
TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx);

/** Recursively resolve parameter and return types. */
TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx);

// =============================================================================
// GENERICS & TRAITS - Validation
// =============================================================================

/** Resolve a trait reference via LOOKUP in type namespace. */
const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx);

/** Verify all generic parameters are used. */
void validateGenericParamUsage(const DeclAST* owner, SemaContext& ctx);

/** Verify struct implements all trait fields. */
bool validateTraitImplementation(const StructDeclAST* structDecl, SemaContext& ctx);

/** Check generic argument arity matches parameters. */
void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       const BaseAST* useSite,
                       SemaContext& ctx);

// =============================================================================
// SELF-REFERENCE DETECTION - Uses DefiningTypeStack
// =============================================================================

/**
 * @brief True if fieldType refers directly to owner's own type (by value).
 *
 * Uses ctx.definingTypes.isDefining(owner) to detect if the owner is
 * currently being defined. This works for any depth of nesting because
 * DefiningTypeStack tracks the entire chain of types being defined.
 *
 * Example:
 *   struct Node<T> {
 *       value Node<T>          → true (illegal, infinite size)
 *       next ptr<Node<T>>?     → false (ptr breaks the cycle)
 *       children [*]Node<T>    → true (array is direct storage)
 *       parent &Node<T>        → false (reference breaks the cycle)
 *   }
 */
bool isDirectSelfReference(const TypeAST* fieldType, const TypeDeclAST* owner, SemaContext& ctx);

/** Resolve field type and reject direct self-references. */
void checkRecursiveFieldType(const FieldDeclAST* field, const TypeDeclAST* owner, SemaContext& ctx);

// =============================================================================
// FFI VALIDATION
// =============================================================================

/** Validate @[foreign("C")] function against FFI manifest. */
void validateForeignFunc(const FuncDeclAST* decl, const AttributeAST* foreignAttr, SemaContext& ctx);

/** True if type is legal at FFI boundary. */
bool isValidFFIType(const TypeAST* type, SemaContext& ctx);

// =============================================================================
// ATTRIBUTES
// =============================================================================

/** Validate attributes on a declaration. */
void validateAttributes(ArenaSpan<AttributePtr> attrs, const DeclAST* owner, SemaContext& ctx);
void validateAttribute(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

// =============================================================================
// LOOKUP HELPERS - All name lookup logic
// =============================================================================

// ─── Generic Parameter Lookup ─────────────────────────────────────────────

/**
 * @brief Check if a name is a generic parameter in the current scope.
 *
 * Generic parameters have the HIGHEST priority and shadow type names.
 */
bool isGenericParam(InternedString name, SemaContext& ctx);

/**
 * @brief Look up a generic parameter by name.
 *
 * @return The GenericParamDeclAST if found, nullptr otherwise.
 */
const GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx);

// ─── Value Lookup ─────────────────────────────────────────────────────────

/**
 * @brief Look up a value declaration by name.
 *
 * Searches: generic params → local scopes → module scope
 * Generic params are NOT values, so they don't match here.
 */
const ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx);

/**
 * @brief Look up a value and report E2001 if not found.
 */
const ValueDeclAST* resolveValueOrError(const IdentifierExprAST* expr, SemaContext& ctx);

/**
 * @brief Look up a function by name.
 *
 * Convenience wrapper that checks the resolved value is a FuncDeclAST.
 */
const FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx);

// ─── Type Lookup ──────────────────────────────────────────────────────────

/**
 * @brief Look up a type declaration by name.
 *
 * Searches: local scopes → module scope
 * Generic parameters are NOT type declarations (they shadow, but are separate).
 */
const TypeDeclAST* lookupType(InternedString name, SemaContext& ctx);

/**
 * @brief Look up a type with proper priority (generic params shadow types).
 *
 * This is the main type resolution function. It handles:
 *   1. Check if it's a generic parameter (returns nullptr, no error)
 *   2. Look up as lookupModuleMember type (returns TypeDeclAST*)
 *   3. Not found (reports E2002, returns nullptr)
 */
const TypeDeclAST* resolveTypeOrError(const NamedTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve a named type reference, reporting E2002 on failure.
 *
 * Alias for resolveTypeOrError() for consistency.
 */
const TypeDeclAST* resolveTypeNameOrError(const NamedTypeAST* type, SemaContext& ctx);

// ─── Redeclaration Checkers ──────────────────────────────────────────────

bool isValueRedeclared(InternedString name, SemaContext& ctx);
bool isTypeRedeclared(InternedString name, SemaContext& ctx);
bool isGenericParamRedeclared(InternedString name, SemaContext& ctx);
bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx);

bool reportValueRedeclaration(InternedString name, const BaseAST* node, SemaContext& ctx);
bool reportTypeRedeclaration(InternedString name, const BaseAST* node, SemaContext& ctx);
bool reportGenericParamRedeclaration(InternedString name, const BaseAST* node, SemaContext& ctx);
bool reportImportAliasRedeclaration(InternedString alias, const BaseAST* node, SemaContext& ctx);

// ─── Module Member Lookup ─────────────────────────────────────────────────

/**
 * @brief Look up a member in a module's table.
 *
 * Used for module:member access. The module must already be resolved.
 */
const ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx);

/**
 * @brief Resolve a module alias and look up a member, with error reporting.
 */
const ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx);

// ─── Callee Resolution ────────────────────────────────────────────────────

/**
 * @brief Resolve a call expression's callee to the FuncDeclAST it names.
 *
 * Handles two callee shapes:
 *   - IdentifierExprAST: Look up in value namespace
 *   - ModuleAccessExprAST: Look up module alias, then member
 *
 * Any other callee shape (curried call, function literal) returns nullptr
 * silently - the caller must check the callee's resolved type instead.
 */
const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx);

// =============================================================================
// TYPE COMPATIBILITY HELPERS
// =============================================================================

/** Structural equality of two types. */
bool typesEqual(const TypeAST* a, const TypeAST* b);

/** True if source value can be used where target is expected. */
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);

/** Strip ?/?!, return inner type. */
TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

bool isNumericType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);

// ─── Type Validation ──────────────────────────────────────────────────────

/**
 * @brief Validate that a const field's type is not nullable or fallible.
 *
 * From DeclAST.hpp:
 *   A const field may NOT be nullable (T?) or fallible (T!).
 */
bool validateConstFieldType(const TypeAST* type, SemaContext& ctx);

/**
 * @brief Validate that a trait field is not nullable or fallible.
 *
 * From DeclAST.hpp:
 *   Trait fields must not be nullable or fallible.
 */
bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx);

/**
 * @brief Validate reference type context (Downward Flow Rule).
 *
 * From TypeAST.hpp:
 *   References (&T) can only appear as:
 *     - Function parameters
 *     - Local variable aliases
 *
 *   Invalid contexts:
 *     - Struct fields (infinite size)
 *     - Array/Slice storage
 *     - Function returns
 */
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx);

} // namespace sema