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

// =============================================================================
// DECLARATIONS - REGISTER names, then analyze internals
// =============================================================================

/**
 * @brief Dispatch a declaration to its specific analyzer.
 *
 * IMPORTANT: Every declaration analyzer follows this pattern:
 *   1. REGISTER the declaration's name in the symbol table
 *   2. Push appropriate context guard (ScopedTypeDefinition for types)
 *   3. Analyze the declaration's internals (fields, body, etc.)
 *   4. Pop context guard
 *
 * This ordering enables self-reference: the name is findable while analyzing
 * the declaration's own internals.
 */
void analyzeDecl(DeclAST* decl, SemaContext& ctx);

// ─── Type Declarations (REGISTER in type namespace) ──────────────────────

/**
 * @brief Register a struct declaration and analyze its fields.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl->name, decl)` - registers in type namespace
 *   - Generic params registered via analyzeGenericParamDecl() BEFORE fields
 *   - Pushes ScopedTypeDefinition for self-reference detection
 *
 * ORDER:
 *   1. Register struct name (for self-reference)
 *   2. Register generic parameters (for use in fields)
 *   3. Push ScopedTypeDefinition
 *   4. Analyze fields (now can find both struct and generic params)
 *   5. Pop ScopedTypeDefinition
 */
void analyzeStructDecl(StructDeclAST* decl, SemaContext& ctx);

/**
 * @brief Register an enum declaration and analyze its variants.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl->name, decl)` - registers in type namespace
 *   - Variants are registered as values in the enum's scope
 */
void analyzeEnumDecl(EnumDeclAST* decl, SemaContext& ctx);

/**
 * @brief Register a trait declaration and analyze its fields.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl->name, decl)` - registers in type namespace
 *   - Generic params registered via analyzeGenericParamDecl() BEFORE fields
 */
void analyzeTraitDecl(TraitDeclAST* decl, SemaContext& ctx);

// ─── Value Declarations (REGISTER in value namespace) ────────────────────

/**
 * @brief Register a variable declaration.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertValue(decl->name, decl)` - registers in value namespace
 *   - For const declarations, marks isConst = true
 */
void analyzeVarDecl(VarDeclAST* decl, SemaContext& ctx);

/**
 * @brief Register a function declaration.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertValue(decl->name, decl)` - registers in value namespace
 *   - Generic params registered via analyzeGenericParamDecl() BEFORE body
 *
 * ORDER:
 *   1. Register function name (for recursion)
 *   2. Register generic parameters (for use in params/return/body)
 *   3. Push ScopedSemanticContext(FuncBody)
 *   4. Analyze parameters, return type, and body
 *   5. Pop context
 */
void analyzeFuncDecl(FuncDeclAST* decl, SemaContext& ctx);

/**
 * @brief Register an import declaration.
 *
 * REGISTRATION:
 *   - `ctx.symbols.addImportAlias(alias, module)` - registers import alias
 *   - This allows `module:member` syntax in expressions
 */
void analyzeImportDecl(ImportDeclAST* decl, SemaContext& ctx);

// ─── Field/Variant/Param (REGISTER in their parent's scope) ──────────────

/**
 * @brief Analyze a struct field.
 *
 * REGISTRATION:
 *   - Fields are registered in the struct's own scope
 *   - `ctx.symbols.insertValue(field->name, field)` - registers in value namespace
 *
 * SELF-REFERENCE:
 *   - Uses ctx.definingTypes.isDefining(owner) to detect direct self-reference
 *   - `value Node<T>` → illegal (infinite size)
 *   - `next ptr<Node<T>>?` → legal (ptr breaks the cycle)
 */
void analyzeFieldDecl(FieldDeclAST* field, StructDeclAST* owner, SemaContext& ctx);

/**
 * @brief Analyze an enum variant.
 *
 * REGISTRATION:
 *   - Variants are registered in the enum's scope
 *   - `ctx.symbols.insertValue(variant->name, variant)` - registers in value namespace
 *
 * DUPLICATE CHECK:
 *   - Verifies no two variants have the same value
 */
void analyzeEnumVariant(EnumVariantAST* variant, EnumDeclAST* owner, SemaContext& ctx);

/**
 * @brief Analyze a trait field requirement.
 *
 * REGISTRATION:
 *   - Trait fields are NOT registered in any namespace
 *   - They are requirements, not actual values
 *   - Only stored in the trait's fields span
 */
void analyzeTraitField(TraitFieldDeclAST* field, TraitDeclAST* owner, SemaContext& ctx);

/**
 * @brief Analyze a function parameter.
 *
 * REGISTRATION:
 *   - Parameters are registered in the function's scope
 *   - `ctx.symbols.insertValue(param->name, param)` - registers in value namespace
 *   - Parameters shadow outer variables
 */
void analyzeParam(ParamAST* param, SemaContext& ctx);

// ─── Generic Parameters (REGISTER in genericParams map) ───────────────────

/**
 * @brief Register a generic parameter.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertGenericParam(param->name, param)` - registers in
 *     the current scope's genericParams map (transient, not module-level)
 *
 * PRIORITY:
 *   - Generic parameters have the HIGHEST lookup priority
 *   - They shadow type names in the current scope
 *   - Example: In `struct Box<T>`, `T` shadows any global type named `T`
 *
 * SCOPE:
 *   - Generic parameters are only valid in the scope they're registered in
 *   - They are popped when the scope is popped
 */
void analyzeGenericParamDecl(GenericParamDeclAST* param, SemaContext& ctx);

// =============================================================================
// STATEMENTS - Control flow analysis
// =============================================================================

/**
 * @brief Analyze a statement.
 *
 * @return true if this statement guarantees control transfer out of the block
 *         (return, break, continue, or a block with such a last statement).
 */
bool analyzeStmt(StmtAST* stmt, SemaContext& ctx);

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
// EXPRESSIONS - Type checking (LOOKUP names)
// =============================================================================

/**
 * @brief Type-check an expression.
 *
 * For identifier expressions, this LOOKS UP the name in the symbol table.
 * Sets expr->resolvedType.
 */
TypeAST* checkExpr(ExprAST* expr, SemaContext& ctx);

TypeAST* checkLiteralExpr(LiteralExprAST* expr, SemaContext& ctx);

/**
 * @brief Check an identifier expression.
 *
 * LOOKUP: `ctx.symbols.lookupValue(expr->name)`
 *   - Searches: generic params → local scopes → module scope
 *   - Reports E2001 if not found
 *   - Sets resolvedType to the declaration's valueType
 */
TypeAST* checkIdentifierExpr(IdentifierExprAST* expr, SemaContext& ctx);

TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkStructLiteralExpr(StructLiteralExprAST* expr, SemaContext& ctx);
TypeAST* checkBinaryExpr(BinaryExprAST* expr, SemaContext& ctx);
TypeAST* checkUnaryExpr(UnaryExprAST* expr, SemaContext& ctx);

/**
 * @brief Check a function call.
 *
 * LOOKUP: Uses resolveCalleeOrError() to find the function declaration.
 *   - Plain call: LOOKUP name in value namespace
 *   - Module call: LOOKUP module alias, then LOOKUP member in module's table
 *   - Other callees: Check callee's resolvedType is a FuncTypeAST
 */
TypeAST* checkCallExpr(CallExprAST* expr, SemaContext& ctx);

TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, SemaContext& ctx);
TypeAST* checkIndexExpr(IndexExprAST* expr, SemaContext& ctx);
TypeAST* checkSliceExpr(SliceExprAST* expr, SemaContext& ctx);

/**
 * @brief Check field access.
 *
 * LOOKUP: Resolve object's type, then LOOKUP field name in that type's scope.
 *   - For structs: LOOKUP field in struct's fields
 *   - For enums: LOOKUP variant in enum's variants
 */
TypeAST* checkFieldAccessExpr(FieldAccessExprAST* expr, SemaContext& ctx);

/**
 * @brief Check module access (module:member).
 *
 * LOOKUP:
 *   1. `ctx.symbols.lookupImport(moduleName)` - find imported module
 *   2. `moduleTable->values.find(memberName)` - LOOKUP member in module's table
 */
TypeAST* checkModuleAccessExpr(ModuleAccessExprAST* expr, SemaContext& ctx);

TypeAST* checkNullableChainExpr(NullableChainExprAST* expr, SemaContext& ctx);
TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST* expr, SemaContext& ctx);
TypeAST* checkAssignExpr(AssignExprAST* expr, SemaContext& ctx);
TypeAST* checkPipelineExpr(PipelineExprAST* expr, SemaContext& ctx);
TypeAST* checkComposeExpr(ComposeExprAST* expr, SemaContext& ctx);
TypeAST* checkAnonFuncExpr(AnonFuncExprAST* expr, SemaContext& ctx);
TypeAST* checkIfExpr(IfExprAST* expr, SemaContext& ctx);
TypeAST* checkRangeExpr(RangeExprAST* expr, SemaContext& ctx);

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
TypeAST* resolveType(TypeAST* type, SemaContext& ctx);

/** Primitive types are always valid (built-in). */
TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx);

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
TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx);

/** Recursively resolve array element type. */
TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx);

/** Recursively resolve inner type. */
TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx);
TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx);
TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve reference type.
 *
 * Checks Downward Flow Rule:
 *   - Cannot store &T in struct fields (uses ctx.definingTypes.current())
 *   - Cannot store &T in arrays
 *   - Cannot return &T from functions
 */
TypeAST* resolveRefType(RefTypeAST* type, SemaContext& ctx);

/** Resolve pointer type - always valid (sealed conduit). */
TypeAST* resolvePtrType(PtrTypeAST* type, SemaContext& ctx);

/** Recursively resolve parameter and return types. */
TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx);

// =============================================================================
// GENERICS & TRAITS - Validation
// =============================================================================

/** Resolve a trait reference via LOOKUP in type namespace. */
TraitDeclAST* resolveTraitRef(NamedTypeAST* ref, SemaContext& ctx);

/** Verify all generic parameters are used. */
void validateGenericParamUsage(ArenaSpan<GenericParamDeclPtr> params,
                                DeclAST* owner,
                                SemaContext& ctx);

/** Verify struct implements all trait fields. */
bool validateTraitImplementation(StructDeclAST* structDecl,
                                  NamedTypeAST* traitRef,
                                  SemaContext& ctx);

/** Check generic argument arity matches parameters. */
void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       BaseAST* useSite,
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
bool isDirectSelfReference(TypeAST* fieldType, TypeDeclAST* owner, SemaContext& ctx);

/** Resolve field type and reject direct self-references. */
void checkRecursiveFieldType(FieldDeclAST* field, TypeDeclAST* owner, SemaContext& ctx);

// =============================================================================
// FFI VALIDATION
// =============================================================================

/** Validate @[foreign("C")] function against FFI manifest. */
void validateForeignFunc(FuncDeclAST* decl, AttributeAST* foreignAttr, SemaContext& ctx);

/** True if type is legal at FFI boundary. */
bool isValidFFIType(TypeAST* type, SemaContext& ctx);

// =============================================================================
// ATTRIBUTES
// =============================================================================

/** Validate attributes on a declaration. */
void validateAttributes(ArenaSpan<AttributePtr> attrs, DeclAST* owner, SemaContext& ctx);
void validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

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
GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx);

// ─── Value Lookup ─────────────────────────────────────────────────────────

/**
 * @brief Look up a value declaration by name.
 *
 * Searches: generic params → local scopes → module scope
 * Generic params are NOT values, so they don't match here.
 */
ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx);

/**
 * @brief Look up a value and report E2001 if not found.
 */
ValueDeclAST* resolveValueOrError(IdentifierExprAST* expr, SemaContext& ctx);

/**
 * @brief Look up a function by name.
 *
 * Convenience wrapper that checks the resolved value is a FuncDeclAST.
 */
FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx);

// ─── Type Lookup ──────────────────────────────────────────────────────────

/**
 * @brief Look up a type declaration by name.
 *
 * Searches: local scopes → module scope
 * Generic parameters are NOT type declarations (they shadow, but are separate).
 */
TypeDeclAST* lookupType(InternedString name, SemaContext& ctx);

/**
 * @brief Look up a type with proper priority (generic params shadow types).
 *
 * This is the main type resolution function. It handles:
 *   1. Check if it's a generic parameter (returns nullptr, no error)
 *   2. Look up as concrete type (returns TypeDeclAST*)
 *   3. Not found (reports E2002, returns nullptr)
 */
TypeDeclAST* resolveTypeOrError(NamedTypeAST* type, SemaContext& ctx);

/**
 * @brief Resolve a named type reference, reporting E2002 on failure.
 *
 * Alias for resolveTypeOrError() for consistency.
 */
TypeDeclAST* resolveTypeNameOrError(NamedTypeAST* type, SemaContext& ctx);

// ─── Module Member Lookup ─────────────────────────────────────────────────

/**
 * @brief Look up a member in a module's table.
 *
 * Used for module:member access. The module must already be resolved.
 */
ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx);

/**
 * @brief Resolve a module alias and look up a member, with error reporting.
 */
ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx);

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
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx);

// ─── Self-Type Cache ──────────────────────────────────────────────────────

/**
 * @brief Get (creating if necessary) the cached self-type reference for decl.
 *
 * Cached in TypeDeclAST::selfType. Lazy-created on first access.
 * Used when a type name appears as a value (e.g., `int("42")`).
 */
NamedTypeAST* selfTypeOf(TypeDeclAST* decl, SemaContext& ctx);

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
bool validateConstFieldType(TypeAST* type, SemaContext& ctx);

/**
 * @brief Validate that a trait field is not nullable or fallible.
 *
 * From DeclAST.hpp:
 *   Trait fields must not be nullable or fallible.
 */
bool validateTraitFieldType(TypeAST* type, SemaContext& ctx);

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
bool validateRefContext(RefTypeAST* type, SemaContext& ctx);

} // namespace sema