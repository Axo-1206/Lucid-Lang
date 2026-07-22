/// @file Sema.hpp
/// @brief Lucid semantic analyzer – validates and annotates parsed ASTs.
/// 
/// @architectural_note Two distinct operations: REGISTER vs LOOKUP
/// 
///   REGISTER (insert into symbol table):
///     - Called when we encounter a declaration that defines a new name
///     - Happens BEFORE analyzing the declaration's internals (for self-reference)
///     - Examples: struct, enum, trait, function, variable, generic parameter
///     - Registration order matters: insert name first, then analyze internals
/// 
///   LOOKUP (search for an existing name):
///     - Called when we encounter a reference to a name
///     - Searches: generic params → local scopes → module scope
///     - Examples: type name in annotation, function call, variable reference
///     - Reports E2001/E2002 if not found
/// 
/// @architectural_note Registering generic parameters
///   Generic parameters are registered in the current scope's `genericParams` map
///   BEFORE analyzing the declaration's body. This allows `T` in `struct Box<T>`
///   to be found when resolving field types like `value T`.
/// 
/// @architectural_note Type declaration registration order
///   For `struct Node<T> { value T, next ptr<Node<T>>? }`:
///   1. Register `Node` in type namespace (so `Node` can reference itself)
///   2. Register `T` in genericParams (so `T` can be used in fields)
///   3. Analyze fields (now both `Node` and `T` are findable)
/// 
/// @architectural_note Self-reference via DefiningTypeStack
///   After registering `Node`, we push it onto DefiningTypeStack. This allows
///   `isDirectSelfReference()` to detect that `next ptr<Node<T>>?` is an
///   indirect self-reference (legal) vs `value Node<T>` (illegal, infinite size).
/// 
/// @architectural_note AST nodes are read-only
///   The parser creates and populates all AST nodes. Semantic analysis reads
///   from them and adds semantic annotations (resolved types, etc.). We never
///   modify the structure of the AST, only annotate existing nodes.
/// 

/// IMPORTANT: IMPORTANT: IMPORTANT: IMPORTANT: IMPORTANT: IMPORTANT:
/// @warning @warning @warning @warning @warning @warning @warning
/// README: README: README: README: README: README: README: README:
///
/// @note the parser already set the data (node fields) for the ast node.
/// The semantic phase will NOT MODIFY the node and only READ the node to
/// make validations

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

/// @brief Analyze all modules in the program.
/// 
/// The ONLY entry point for semantic analysis. Processes modules in
/// dependency order (imports before dependents). 
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
// EXPRESSIONS - Type checking
// =============================================================================

/**
 * @brief Type-check an expression against a required target type.
 *
 * Since Lucid requires explicit type annotations, the target type is always known.
 * This function validates that the expression is valid and its type is assignable
 * to the target type.
 *
 * @param expr The expression to check.
 * @param targetType The required target type (must not be nullptr).
 * @param ctx The semantic context.
 * @return true if the expression is valid and assignable to targetType.
 *
 * @note The `resolvedType` field is set to targetType for all valid expressions.
 *       This matches Lucid's explicit type system where every expression's type
 *       is known from context.
 */
bool checkExpr(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Literal Expressions ─────────────────────────────────────────────────

/**
 * @brief Check a literal expression against the target type.
 * 
 * Validates that the literal kind matches the target type:
 *   - int literal → integer target type
 *   - float literal → float target type
 *   - string literal → string target type
 *   - char literal → char target type
 *   - true/false → bool target type
 *   - nil → nullable target type (T?)
 *   - err → fallible target type (T!)
 */
bool checkLiteralExpr(LiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Identifier Expressions ──────────────────────────────────────────────

/**
 * @brief Check an identifier expression.
 *
 * LOOKUP: Resolves the name via `resolveValueOrError()`.
 *   - Searches: generic params → local scopes → module scope
 *   - Reports E2001 if not found
 *   - Validates that the resolved value's type is assignable to targetType
 */
bool checkIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Array and Struct Literals ───────────────────────────────────────────

/**
 * @brief Check an array literal against the target array type.
 *
 * Validates:
 *   - targetType must be ArrayTypeAST
 *   - Each element is assignable to the array's element type
 *   - For fixed arrays: element count ≤ size
 *   - For nested arrays: recursively checks inner arrays
 */
bool checkArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/**
 * @brief Check a struct literal against the target struct type.
 *
 * Validates:
 *   - targetType must be NamedTypeAST resolving to a StructDeclAST
 *   - All required fields (const without default) are initialized
 *   - Each field initializer is assignable to the field's type
 *   - No unknown fields are present
 */
bool checkStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Binary Expressions ──────────────────────────────────────────────────

/**
 * @brief Check a binary expression.
 *
 * Validates both operands against the target type, then operator-specific rules:
 *   - Arithmetic (+, -, *, /, %, **): target must be numeric
 *   - Comparison (==, !=, <, <=, >, >=): target must be bool
 *   - Logical (and, or): target must be bool
 *   - Bitwise (&, |, ^, <<, >>): target must be integer
 *
 * Nullable/Fallible Rules:
 *   - Arithmetic/Logical/Bitwise: operands must be definite (non-nullable, non-fallible)
 *   - Comparison: operands may be nullable (nil comparison allowed), but not fallible
 *   - Comparison: fallible operands are not allowed (must handle error first)
 */
bool checkBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Unary Expressions ───────────────────────────────────────────────────

/**
 * @brief Check a unary expression.
 *
 * Validates operand against the target type:
 *   - Negation (-x): target must be numeric, operand must be definite
 *   - Logical Not (not x): target must be bool, operand must be definite
 *   - Bitwise Not (~x): target must be integer, operand must be definite
 *
 * Nullable/Fallible Rules:
 *   - Unary operations on nullable/fallible are NOT allowed (must narrow first)
 */
bool checkUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Call Expressions ────────────────────────────────────────────────────

/**
 * @brief Check a function call.
 *
 * Validates:
 *   - Callee resolves to a callable function
 *   - Callee is definite (not nullable/fallible)
 *   - Arguments are assignable to parameter types
 *   - Return type is assignable to targetType
 *
 * Nullable/Fallible Rules:
 *   - Callee cannot be nullable/fallible (must narrow first)
 *   - Arguments cannot be fallible (must handle error first)
 *   - Arguments may be nullable if parameter type is nullable
 */
bool checkCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/**
 * @brief Check an intrinsic call.
 *
 * Validates the intrinsic name exists and arguments are correct.
 * The target type is the intrinsic's expected return type.
 */
bool checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Index and Slice Expressions ─────────────────────────────────────────

/**
 * @brief Check an index expression: arr[index].
 *
 * Validates:
 *   - target type is the array's element type
 *   - target is an array type
 *   - index is a definite integer type
 *
 * Nullable/Fallible Rules:
 *   - Index must be definite (non-nullable, non-fallible)
 *   - Array element may be nullable (result type inherits nullability)
 */
bool checkIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/**
 * @brief Check a slice expression: arr[start..end].
 *
 * Validates:
 *   - target type is the slice type ([_]T)
 *   - target is an array type
 *   - start/end bounds are definite integer types
 *
 * Nullable/Fallible Rules:
 *   - Bounds must be definite (non-nullable, non-fallible)
 *   - Result inherits element nullability
 */
bool checkSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Field Access Expressions ────────────────────────────────────────────

/**
 * @brief Check a field access: obj.field.
 *
 * Validates:
 *   - target type is the field's type
 *   - object is a struct or enum type
 *   - field exists on the object
 *
 * Nullable/Fallible Rules:
 *   - Object must be definite (use ?. for nullable, handle error for fallible)
 *   - Field access on nullable requires NullableChainExpr (?.)
 *   - Field access on fallible is NOT allowed
 */
bool checkFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Module Access Expressions ───────────────────────────────────────────

/**
 * @brief Check a module access: module:member.
 *
 * Validates:
 *   - target type is the member's type
 *   - module alias resolves to a valid module
 *   - member exists in the module's exports
 *
 * Module access is always read-only.
 */
bool checkModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Nullable Chain ──────────────────────────────────────────────────────

/**
 * @brief Check a nullable chain: obj?.field?.field.
 *
 * Validates:
 *   - target type is the final field's type (nullable)
 *   - Each step is a field access on a nullable type
 *   - The chain must be terminated by ?? (checked at parent)
 *
 * Nullable/Fallible Rules:
 *   - Base must be nullable (T?), not fallible (T!)
 *   - Each step must be nullable
 *   - Fallible values cannot be chained (must handle error first)
 */
bool checkNullableChainExpr(NullableChainExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Null Coalesce ───────────────────────────────────────────────────────

/**
 * @brief Check a null coalesce: value ?? fallback.
 *
 * Validates:
 *   - LHS is nullable or fallible
 *   - RHS type is assignable to the unwrapped type
 *   - target type is the RHS type (or unwrapped LHS type)
 *
 * Nullable/Fallible Rules:
 *   - LHS can be T?, T!, or T?!
 *   - ?? unwraps T? to T (handles nil)
 *   - ?? unwraps T! to T (handles err)
 *   - ?? unwraps T?! to T (handles nil and err)
 *   - RHS must be definite (not nullable/fallible) or match unwrapped type
 */
bool checkNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Assignment ──────────────────────────────────────────────────────────

/**
 * @brief Check an assignment: lhs = rhs.
 *
 * Validates:
 *   - target type is the LHS type
 *   - LHS is an assignable lvalue
 *   - RHS is assignable to LHS type
 *
 * Nullable/Fallible Rules:
 *   - RHS cannot be fallible (must handle error first)
 *   - RHS can be nullable if LHS is nullable (widening)
 *   - RHS cannot be nullable if LHS is definite (narrowing - requires ??)
 */
bool checkAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Pipeline ────────────────────────────────────────────────────────────

/**
 * @brief Check a pipeline expression: seed |> step |> step.
 *
 * Validates:
 *   - target type is the final output type
 *   - Seed type matches first step's input
 *   - Each step's output matches next step's input
 *
 * Nullable/Fallible Rules:
 *   - Pipeline short-circuits on err
 *   - Steps cannot be fallible functions (must handle error first)
 */
bool checkPipelineExpr(PipelineExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Composition ─────────────────────────────────────────────────────────

/**
 * @brief Check a composition expression: f +> g +> h.
 *
 * Validates:
 *   - target type is the composed function type
 *   - Each operand is a function type
 *   - Output of left matches input of right
 *
 * Nullable/Fallible Rules:
 *   - Fallible functions cannot be composed
 *   - Nullable functions cannot be composed (must handle first)
 */
bool checkComposeExpr(ComposeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Anonymous Function ──────────────────────────────────────────────────

/**
 * @brief Check an anonymous function expression.
 *
 * Validates:
 *   - target type is the function type
 *   - Parameters are valid
 *   - Body returns the correct type
 */
bool checkAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── If Expression ──────────────────────────────────────────────────────

/**
 * @brief Check an if expression.
 *
 * Validates:
 *   - target type is the common type of both branches
 *   - Condition is bool or coercible to bool
 *   - Both branches produce compatible types
 *
 * Nullable/Fallible Rules:
 *   - Condition must be definite (non-nullable, non-fallible)
 */
bool checkIfExpr(IfExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Range Expression ────────────────────────────────────────────────────

/**
 * @brief Check a range expression: lo..hi or lo..<hi.
 *
 * Validates:
 *   - target type is the numeric element type
 *   - Both bounds are numeric and same type
 *   - Bounds are definite (non-nullable, non-fallible)
 */
bool checkRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

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

bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx);
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