/// @file SemaExpr.cpp
/// @brief Implements Sema.hpp's "Expressions (Type Checking)" section.
///
/// ============================================================================
/// DESIGN PHILOSOPHY: Target Type + Boolean Return
/// ============================================================================
/// 
/// ─── Why Target Type? ─────────────────────────────────────────────────────────
/// 
/// Lucid REQUIRES explicit type annotations on all declarations. This means the
/// type of every expression is known from its context BEFORE we even look at the
/// expression itself. Examples:
/// 
///   let x int = 5 + 3     → target type is `int`
///   const add (a int)(b int) -> int = { return a + b }  → return type is `int`
///   struct Point { x float, y float }  → field `x` has target type `float`
/// 
/// Since the target type is always known, we pass it as a parameter to checkExpr().
/// This eliminates the need to "infer" types from expressions - we just validate
/// that the expression produces a value of the expected type.
/// 
/// ─── Why Boolean Return? ─────────────────────────────────────────────────────
/// 
/// Because the target type is known, we don't need checkExpr() to compute and
/// return a type. The type is already known from context. Instead, checkExpr()
/// returns a boolean indicating whether the expression is valid.
/// 
/// Benefits:
///   1. No type creation - We never allocate new TypeAST nodes during checking
///   2. Simpler API - Just check, don't compute
///   3. Matches Lucid's explicit type system
///   4. Better performance - No unnecessary allocations
/// 
/// ─── How resolvedType is Set ─────────────────────────────────────────────────
/// 
/// On success, `expr->resolvedType` is set to the target type. This gives:
///   - Codegen: Direct access to the expression's type
///   - Parent expressions: Type information for further validation
///   - Debugging: Clear type information in AST
/// 
/// ─── Example Flow ────────────────────────────────────────────────────────────
/// 
/// For `let x int = 5 + 3`:
///   1. analyzeVarDecl sees target type = `int`
///   2. checkExpr(init, int) is called
///   3. checkBinaryExpr checks left operand against `int` → 5 is int ✅
///   4. checkBinaryExpr checks right operand against `int` → 3 is int ✅
///   5. checkBinaryExpr validates operator-specific rules: Add requires numeric → int is numeric ✅
///   6. Returns true, sets expr->resolvedType = int
///   7. analyzeVarDecl sees success, completes validation
/// 
/// Notice that no type was ever computed or allocated - everything was validated
/// against the known target type.
/// 
/// ============================================================================
/// NULLABLE VS FALLIBLE: DESIGN RATIONALE
/// ============================================================================
/// 
/// Both nullable (`T?`) and fallible (`T!`) types share a common rule:
///   ❌ They are REJECTED from all operations EXCEPT comparison (`==`, `!=`, etc.)
/// 
/// This forces explicit handling of `nil` and `err` before performing operations.
/// The `??` operator is used for narrowing both types.
/// 
/// ─── The Key Difference ─────────────────────────────────────────────────────
/// 
/// ┌─────────────┬────────────────────────────────────────────────────────────┐
/// │ Aspect      │ Nullable (`T?`)          │ Fallible (`T!`)               │
/// ├─────────────┼──────────────────────────┼───────────────────────────────┤
/// │ Purpose     │ Absence of value (`nil`) │ Error state (`err`)            │
/// ├─────────────┼──────────────────────────┼───────────────────────────────┤
/// │ Operations  │ ❌ Rejected (except cmp) │ ❌ Rejected (except cmp)      │
/// ├─────────────┼──────────────────────────┼───────────────────────────────┤
/// │ Narrowing   │ `??` provides fallback   │ `??` provides fallback        │
/// ├─────────────┼──────────────────────────┼───────────────────────────────┤
/// │ Error       │ ❌ Cannot receive `nil`  │ ✅ Can receive `err` from     │
/// │ Recovery    │ from failed operations   │    failed operations          │
/// ├─────────────┼──────────────────────────┼───────────────────────────────┤
/// │ Source of   │ User explicit (`nil`)    │ Compiler generated on failure │
/// │ Value       │ or literal               │ or user explicit (`err`)      │
/// └─────────────┴──────────────────────────┴───────────────────────────────┘
/// 
/// ─── Why Only Comparison Is Allowed ────────────────────────────────────────
/// 
/// Comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`) is the ONLY operation that
/// can be performed on nullable/fallible types. This is because:
///   1. You need to check for `nil` or `err` before narrowing
///   2. `x == nil` and `x != nil` are fundamental operations for null checking
///   3. `x == err` and `x != err` are fundamental for error checking
///   4. Comparisons between nullable types are well-defined (`nil == nil` is true)
/// 
/// ─── Example: Why Operations Are Rejected ──────────────────────────────────
/// 
/// ```lucid
/// let x int? = 5
/// let y int? = nil
/// 
/// // ❌ Rejected: arithmetic on nullable
/// let z int? = x + y
/// // Error: arithmetic cannot be used with nullable operands.
/// // Use `??` to handle nil first.
/// 
/// // ✅ Correct: narrow first
/// let z int? = (x ?? 0) + (y ?? 0)
/// 
/// // ✅ Allowed: comparison for null checking
/// if x == nil { ... }
/// if y != nil { ... }
/// ```
/// 
/// ─── Example: Fallible Error Recovery ──────────────────────────────────────
/// 
/// ```lucid
/// let result int! = parse("42")
/// 
/// // ❌ Rejected: arithmetic on fallible
/// let doubled int! = result * 2
/// // Error: arithmetic cannot be used with fallible operands.
/// // Use `??` to handle err first.
/// 
/// // ✅ Correct: narrow first
/// let doubled int! = (result ?? 0) * 2
/// 
/// // ✅ Error recovery: if parse fails, result is err
/// let result int! = parse("invalid")  // result = err
/// let doubled int! = (result ?? 0) * 2  // doubled = err (propagates)
/// ```
/// 
/// ─── Comparison with Nullable ──────────────────────────────────────────────
/// 
/// ```lucid
/// let x int? = 5
/// let y int? = nil
/// 
/// // ✅ Allowed: comparison
/// let a bool = x == y    // false (5 == nil)
/// let b bool = x != nil  // true (5 != nil)
/// let c bool = y == nil  // true (nil == nil)
/// 
/// // ❌ Rejected: arithmetic
/// let z int? = x + y     // ERROR
/// 
/// // ✅ Correct: narrow with ??
/// let z int? = (x ?? 0) + (y ?? 0)  // z = 5
/// ```
/// 
/// ─── The Fallible Exception: Error Propagation ────────────────────────────
/// 
/// The ONLY place where nullable and fallible differ is in error recovery:
/// 
/// ```lucid
/// // Fallible: can receive err from failed operations
/// let result int! = parse("invalid")  // result = err (compiler generated)
/// 
/// // Nullable: cannot receive nil from failed operations
/// let result int? = parse("invalid")  // ERROR: parse returns int!, not int?
/// ```
/// 
/// This is why fallible types exist: they allow the compiler to automatically
/// propagate error states through expressions, while nullable types require
/// explicit `nil` assignment.
/// 
/// ─── Summary ────────────────────────────────────────────────────────────────
/// 
/// 1. Both nullable and fallible types are REJECTED from operations (except comparison)
/// 2. Both require `??` to narrow before operations
/// 3. Fallible types can receive `err` from failed operations (compiler generated)
/// 4. Nullable types cannot receive `nil` from operations (user explicit only)
/// 5. This distinction justifies keeping separate names (`?` vs `!`)
/// 
/// For index expressions, this means:
///   - Index must be definite (non-nullable, non-fallible)
///   - Nullable index → rejected
///   - Fallible index → rejected
///   - Array element can be nullable/fallible (result type inherits)
/// 
/// ─── Type Narrowing ──────────────────────────────────────────────────────────
/// 
/// Nullable and fallible types can be narrowed using:
///   - `if x != nil { ... }`   → x is T inside the branch
///   - `if x != err { ... }`   → x is T inside the branch
///   - `if x == nil { ... }`   → x is nil inside the branch (handled separately)
///   - `x ?? fallback`          → Evaluates to T (handles nil/err)
/// 
/// ─── Helper Functions ────────────────────────────────────────────────────────
/// 
/// The following type predicate helpers are available:
///   - isNullableType(type)     : T? or T?!
///   - isFallibleType(type)     : T! or T?!
///   - isDefiniteType(type)     : Not nullable and not fallible
///   - unwrapNullable(type)     : Strips ?/?! to get inner type
///   - unwrapFallible(type)     : Strips !/?! to get inner type
///   - unwrapDefinite(type)     : Strips all modifiers to get inner type

#include "../Sema.hpp"
#include "DebugUtils.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "../support/IntrinsicRegistry.hpp"
#include "core/memory/InternedString.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

// =============================================================================
// checkExpr - Dispatch
// =============================================================================

/// @brief Dispatch an expression to its specific check*Expr() function.
/// 
/// We validate if the target expression returns the type we want,
/// @note even there's no type we still need to verify if the expression is
/// logically correct
bool checkExpr(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::ArrayLiteralExpr:   return checkArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), targetType, ctx);
        case ASTKind::StructLiteralExpr:  return checkStructLiteralExpr(expr->as<StructLiteralExprAST>(), targetType, ctx);
        case ASTKind::BinaryExpr:         return checkBinaryExpr(expr->as<BinaryExprAST>(), targetType, ctx);
        case ASTKind::UnaryExpr:          return checkUnaryExpr(expr->as<UnaryExprAST>(), targetType, ctx);
        case ASTKind::CallExpr:           return checkCallExpr(expr->as<CallExprAST>(), targetType, ctx);
        case ASTKind::IntrinsicCallExpr:  return checkIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), targetType, ctx);
        case ASTKind::IndexExpr:          return checkIndexExpr(expr->as<IndexExprAST>(), targetType, ctx);
        case ASTKind::SliceExpr:          return checkSliceExpr(expr->as<SliceExprAST>(), targetType, ctx);
        case ASTKind::FieldAccessExpr:    return checkFieldAccessExpr(expr->as<FieldAccessExprAST>(), targetType, ctx);
        case ASTKind::ModuleAccessExpr:   return checkModuleAccessExpr(expr->as<ModuleAccessExprAST>(), targetType, ctx);
        case ASTKind::NullableChainExpr:  return checkNullableChainExpr(expr->as<NullableChainExprAST>(), targetType, ctx);
        case ASTKind::NullCoalesceExpr:   return checkNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), targetType, ctx);
        case ASTKind::AssignExpr:         return checkAssignExpr(expr->as<AssignExprAST>(), targetType, ctx);
        case ASTKind::PipelineExpr:       return checkPipelineExpr(expr->as<PipelineExprAST>(), targetType, ctx);
        case ASTKind::ComposeExpr:        return checkComposeExpr(expr->as<ComposeExprAST>(), targetType, ctx);
        case ASTKind::AnonFuncExpr:       return checkAnonFuncExpr(expr->as<AnonFuncExprAST>(), targetType, ctx);
        case ASTKind::IfExpr:             return checkIfExpr(expr->as<IfExprAST>(), targetType, ctx);
        case ASTKind::RangeExpr:          return checkRangeExpr(expr->as<RangeExprAST>(), targetType, ctx);
        case ASTKind::IdentifierExpr:     return checkIdentifierExpr(expr->as<IdentifierExprAST>(), targetType, ctx);
        default:
            expr->resolvedType = nullptr;
            return false;
    }
}

// =============================================================================
// checkLiteralExpr
// =============================================================================

/// @brief Type-check a literal expression against the target type.
/// 
/// Validates that the literal kind matches the target type:
///   - int/hex/binary literal → integer target type → ValueState::Definite
///   - float literal → float target type → ValueState::Definite
///   - string/rawstring literal → string target type → ValueState::Definite
///   - char literal → char target type → ValueState::Definite
///   - true/false → bool target type → ValueState::Definite
///   - nil → nullable target type (T?) → ValueState::Nil
///   - err → fallible target type (T!) → ValueState::Err
bool checkLiteralExpr(LiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // nil and err are special: they require the target type to be nullable/fallible
    // and they don't have a specific primitive kind to check against
    if (expr->kind == LiteralKind::Nil) {
        if (!isNullableType(targetType)) {
            ctx.error(expr, DiagCode::E3003,
                      "nil literal requires a nullable target type (T?), got ",
                      debug::typeToString(targetType, ctx.pool()));
            return false;
        }
        expr->valueState = ValueState::Nil;  // Mark as nil
        return true;
    }

    if (expr->kind == LiteralKind::Err) {
        if (!isFallibleType(targetType)) {
            ctx.error(expr, DiagCode::E3003,
                      "err literal requires a fallible target type (T!), got ",
                      debug::typeToString(targetType, ctx.pool()));
            return false;
        }
        expr->valueState = ValueState::Err;  // Mark as err
        return true;
    }

    // For all other literals, the target type must be a primitive type
    if (!targetType->isa<PrimitiveTypeAST>()) {
        ctx.error(expr, DiagCode::E3003,
                  "literal requires a primitive target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        return false;
    }

    const PrimitiveTypeAST* primTarget = targetType->as<PrimitiveTypeAST>();
    PrimitiveKind targetKind = primTarget->primitiveKind;

    // Map literal kind to expected primitive kind
    switch (expr->kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            // Integer literal can be assigned to any integer type
            if (!isIntegerType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "integer literal requires an integer target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
            // TODO: Check if the literal value fits in the target integer type
            expr->valueState = ValueState::Definite;
            return true;
        }

        case LiteralKind::Float: {
            // Float literal can be assigned to any float type
            if (!isFloatType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "float literal requires a float target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
            expr->valueState = ValueState::Definite;
            return true;
        }

        case LiteralKind::String:
        case LiteralKind::RawString: {
            if (targetKind != PrimitiveKind::String) {
                ctx.error(expr, DiagCode::E3003,
                          "string literal requires string target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
            expr->valueState = ValueState::Definite;
            return true;
        }

        case LiteralKind::Char: {
            if (targetKind != PrimitiveKind::Char) {
                ctx.error(expr, DiagCode::E3003,
                          "char literal requires char target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
            expr->valueState = ValueState::Definite;
            return true;
        }

        case LiteralKind::True:
        case LiteralKind::False: {
            if (targetKind != PrimitiveKind::Bool) {
                ctx.error(expr, DiagCode::E3003,
                          "boolean literal requires bool target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
            expr->valueState = ValueState::Definite;
            return true;
        }

        case LiteralKind::Nil:
        case LiteralKind::Err:
            // These are handled above
            return false;

        default:
            ctx.error(expr, DiagCode::E3003,
                      "unknown literal kind");
            return false;
    }
}

// =============================================================================
// checkIdentifierExpr
// =============================================================================

/// @brief Check an identifier expression.
///
/// LOOKUP: Resolves the name via `resolveValueOrError()`.
///   - Searches: generic params → local scopes → module scope
///   - Reports E2001 if not found
///   - Validates that the resolved value's type is assignable to targetType
///   - Propagates nil/err state from the resolved declaration
bool checkIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    const ValueDeclAST* decl = resolveValueOrError(expr, ctx);
    if (!decl) return false;

    // Check if the resolved type is assignable to the target type
    if (decl->type && !isAssignable(targetType, decl->type, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ", debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(decl->type, ctx.pool()));
        return false;
    }

    // Propagate value state from the declaration if known
    // For variables, we don't know the value state at compile-time
    // unless it's a constant
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* varDecl = decl->as<VarDeclAST>();
        // If the variable is const and has a literal initializer, we might know its value
        // For now, mark as Unknown (runtime evaluation needed)
        expr->valueState = ValueState::Unknown;
    } else if (decl->isa<EnumVariantAST>()) {
        // Enum variants are definite values
        expr->valueState = ValueState::Definite;
    } else {
        // Other declarations (functions, parameters, fields) are unknown at compile-time
        expr->valueState = ValueState::Unknown;
    }

    return true;
}

// =============================================================================
// checkArrayLiteralExpr
// =============================================================================

/// @brief Type-check an array literal: verify all elements match the target type.
/// 
/// Validates:
///   - targetType must be ArrayTypeAST
///   - Each element is checked against the array's element type
///   - For fixed arrays: element count must not exceed the declared size
///   - For nested arrays: recursively checks inner arrays
///   - If any element is err, the entire array literal is err
///
/// @note Empty array literals `[]` are valid and type is inferred from targetType.
///       The declaration site enforces size limits for fixed arrays.
bool checkArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Target type must be an array type
    if (!targetType->isa<ArrayTypeAST>()) {
        ctx.error(expr, DiagCode::E3003,
                  "array literal requires an array target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        return false;
    }

    const ArrayTypeAST* arrTarget = targetType->as<ArrayTypeAST>();
    const TypeAST* elemTarget = arrTarget->element;

    // Track if any element is err
    bool hasErr = false;

    // Check each element against the element type
    for (ExprAST* elem : expr->elements) {
        if (!checkExpr(elem, elemTarget, ctx)) {
            return false;
        }
        // Propagate err state
        if (elem->valueState == ValueState::Err) {
            hasErr = true;
        }
        // Nil elements are allowed in nullable arrays
        if (elem->valueState == ValueState::Nil && !isNullableType(elemTarget)) {
            ctx.error(elem, DiagCode::E3003,
                      "nil element in non-nullable array");
            return false;
        }
    }

    // For fixed arrays, check size limit
    if (arrTarget->isFixed()) {
        if (expr->elements.size() > arrTarget->size) {
            ctx.error(expr, DiagCode::E3001,
                      "array literal has ", std::to_string(expr->elements.size()),
                      " elements, but fixed array expects at most ",
                      std::to_string(arrTarget->size));
            return false;
        }
    }

    // Set the value state of the array literal
    if (hasErr) {
        expr->valueState = ValueState::Err;
    } else if (expr->elements.empty()) {
        // Empty array is definite (no elements to evaluate)
        expr->valueState = ValueState::Definite;
    } else {
        // Check if all elements are definite
        bool allDefinite = true;
        for (ExprAST* elem : expr->elements) {
            if (elem->valueState != ValueState::Definite) {
                allDefinite = false;
                break;
            }
        }
        expr->valueState = allDefinite ? ValueState::Definite : ValueState::Unknown;
    }

    return true;
}

// =============================================================================
// checkStructLiteralExpr
// =============================================================================

/// @brief Type-check a struct literal: resolve the struct type and validate
///        all field initializers.
///
/// Initialization Rules:
///   - Fields without default values MUST be explicitly initialized
///   - Fields with default values MAY be omitted (default is used)
///   - Nullable fields (T?) without default: can be omitted → defaults to nil
///   - Fallible fields (T!) without default: can be omitted → defaults to err
///   - Combined fields (T?!) without default: MUST be explicitly initialized (no implicit default)
///   - `const` modifier only prevents modification after init, doesn't affect initialization rules
///   - If any field initializer is err, the entire struct literal is err
///
/// Const Field Rules (from DeclAST.hpp):
///   - A const field may NOT be nullable (T?) or fallible (T!)
///   - Const fields cannot be assigned nil or err (must be definite values)
///
/// Validates:
///   - targetType must be NamedTypeAST resolving to a StructDeclAST
///   - All fields without defaults are initialized (except nullable/fallible which can default)
///   - Combined (T?!) fields MUST always be explicitly initialized
///   - Const fields cannot be assigned nil or err (must be definite values)
///   - Each field initializer is assignable to the field's type
///   - No unknown fields are present
///   - Generic arguments are resolved and validated
bool checkStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Target type must be a named type (struct)
    if (!targetType->isa<NamedTypeAST>()) {
        ctx.error(expr, DiagCode::E3003,
                  "struct literal requires a struct target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        return false;
    }

    const NamedTypeAST* namedTarget = targetType->as<NamedTypeAST>();

    // Resolve the struct declaration
    const TypeDeclAST* typeDecl = resolveTypeNameOrError(namedTarget, ctx);
    if (!typeDecl) return false;

    // Verify it's a struct
    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.error(expr, DiagCode::E2002,
                  "expected struct type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        return false;
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Check generic arguments
    if (!expr->genericArgs.empty()) {
        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) return false;
        }
        // TODO: Check generic argument arity and constraints
    }

    // Track which fields have been initialized
    std::unordered_set<InternedString> initializedFields;

    // Map field names to their declaration for quick lookup
    std::unordered_map<InternedString, const FieldDeclAST*> fieldMap;
    for (const FieldDeclAST* field : structDecl->fields) {
        fieldMap[field->name] = field;
    }

    // Track if any field initializer is err
    bool hasErr = false;
    bool allDefinite = true;

    // Check each field initializer
    for (const FieldInitAST* init : expr->inits) {
        // Find the field in the struct
        auto it = fieldMap.find(init->name);
        if (it == fieldMap.end()) {
            ctx.error(init, DiagCode::E2001,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' has no field named '", ctx.pool().lookup(init->name), "'");
            return false;
        }

        const FieldDeclAST* field = it->second;

        // ─── Const Field Special Validation ────────────────────────────────
        // Const fields cannot be assigned nil or err, regardless of type
        // We check this BEFORE type checking to catch errors even if the
        // field's type declaration is broken/corrupted
        if (field->isConst) {
            // Check if the initializer is nil or err literal
            if (init->value->isa<LiteralExprAST>()) {
                const LiteralExprAST* literal = init->value->as<LiteralExprAST>();
                if (literal->kind == LiteralKind::Nil || literal->kind == LiteralKind::Err) {
                    ctx.error(init, DiagCode::E3004,
                              "const field '", ctx.pool().lookup(field->name),
                              "' cannot be assigned '", 
                              (literal->kind == LiteralKind::Nil ? "nil" : "err"),
                              "' (const fields must have definite values)");
                    return false;
                }
            }
        }

        // ─── Type Check ─────────────────────────────────────────────────────
        // Check the initializer against the field type
        // If the field type is broken (from struct declaration), this will
        // still attempt to check, but may report additional errors
        if (!checkExpr(init->value, field->type, ctx)) {
            // Error already reported by checkExpr
            // Continue to mark as initialized for error recovery
        }

        // Track value state for propagation
        if (init->value->valueState == ValueState::Err) {
            hasErr = true;
        }
        if (init->value->valueState != ValueState::Definite) {
            allDefinite = false;
        }

        // Mark this field as initialized
        initializedFields.insert(init->name);
    }

    // Verify all required fields are initialized
    for (const FieldDeclAST* field : structDecl->fields) {
        // Skip fields that were explicitly initialized
        if (initializedFields.find(field->name) != initializedFields.end()) {
            continue;
        }

        // Check if the field has a default value
        if (field->defaultVal) {
            continue;
        }

        // No default value - check if it can be implicitly defaulted
        if (isNullableType(field->type)) {
            continue;
        }

        if (isFallibleType(field->type)) {
            continue;
        }

        // Combined type T?! requires explicit initialization
        if (field->type->isa<CombinedTypeAST>()) {
            ctx.error(expr, DiagCode::E3002,
                      "combined field '", ctx.pool().lookup(field->name),
                      "' (T?!) must be explicitly initialized (no implicit default)");
            return false;
        }

        // No default, not nullable, not fallible, not combined - must initialize
        ctx.error(expr, DiagCode::E3002,
                  "field '", ctx.pool().lookup(field->name),
                  "' must be initialized in struct literal (no default value)");
        return false;
    }

    // Set the value state of the struct literal
    if (hasErr) {
        expr->valueState = ValueState::Err;
    } else if (allDefinite && !expr->inits.empty()) {
        expr->valueState = ValueState::Definite;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    return true;
}

// =============================================================================
// checkBinaryExpr
// =============================================================================

/// @brief Check a binary expression.
///
/// The type depends on the operator:
///   - Arithmetic (+, -, *, /, %, **): numeric → numeric
///   - Comparison (==, !=, <, <=, >, >=): any → bool
///   - Logical (and, or): any (coerced to bool) → bool
///   - Bitwise (&, |, ^, <<, >>): integer → integer
///
/// Rules:
///   - Comparisons (==, !=, <, <=, >, >=): allow nullable/fallible operands
///   - Arithmetic (+, -, *, /, %, **): require definite operands
///   - Logical (and, or): require definite bool operands
///   - Bitwise (&, |, ^, <<, >>): require definite integer operands
///
/// Nullable/Fallible Rules:
///   - Comparison: allows nullable (nil == nil, nil != T, etc.)
///   - Comparison: allows fallible (err == err, err != T, etc.)
///   - Arithmetic/Logical/Bitwise: operands must be definite (non-nullable, non-fallible)
///   - If target type is fallible and an operation on fallible operands is attempted,
///     the result is `err` (matching the target type) - but this is a RUNTIME behavior,
///     not compile-time. At compile-time, we still reject the operation.
///
/// Type Narrowing (If Condition Context):
///   - When inside an if condition (ctx.contexts.isIfConditionCtx()), detect patterns:
///     - x == nil   → x is nil in then branch, non-nullable in inverse
///     - x != nil   → x is non-nullable in then branch
///     - x == err   → x is err in then branch, non-fallible in inverse
///     - x != err   → x is non-fallible in then branch
///   - Stores narrowing info in ctx.contexts for analyzeIfStmt to use
///
/// Examples:
///   // ✅ Comparison with nullable
///   let x float? = 5.0
///   let y float? = nil
///   let result bool = x == y  // ALLOWED: compiles, runtime: false
///
///   // ✅ Comparison with fallible
///   let a int! = 42
///   let b int! = err
///   let result bool = a == b  // ALLOWED: compiles, runtime: false
///
///   // ❌ Arithmetic with nullable (compile-time error)
///   let x float? = 5.0
///   let y float? = nil
///   let z float? = x + y  // ERROR: arithmetic cannot be used with nullable operands
///
///   // ✅ Correct: use ?? to handle nil
///   let z float? = (x ?? 0.0) + (y ?? 0.0)  // ALLOWED: z = 5.0
///
///   // ❌ Arithmetic with fallible (compile-time error)
///   let a int! = 42
///   let b int! = err
///   let c int! = a + b  // ERROR: arithmetic cannot be used with fallible operands
///
///   // ✅ Correct: use ?? to handle err
///   let c int! = (a ?? 0) + (b ?? 0)  // ALLOWED: c = 42
///
///   // ✅ Type narrowing in if condition
///   if x != nil {
///       // x is int here (narrowed)
///   }
///
///   // ✅ Type narrowing with fallible
///   if a != err {
///       // a is int here (narrowed)
///   }
///
///   // ✅ Inverse narrowing with early exit
///   if a == nil { return }
///   // a is int here (inverse narrowing applied)
bool checkBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check operands ──────────────────────────────────────────
    if (!checkExpr(expr->left, targetType, ctx)) return false;
    if (!checkExpr(expr->right, targetType, ctx)) return false;

    // ─── Step 2: Get value states ───────────────────────────────────────
    ValueState leftState = expr->left->valueState;
    ValueState rightState = expr->right->valueState;

    bool leftIsErr = leftState == ValueState::Err;
    bool rightIsErr = rightState == ValueState::Err;
    bool leftIsNil = leftState == ValueState::Nil;
    bool rightIsNil = rightState == ValueState::Nil;

    // ─── Step 3: Check if we're in an if condition context ─────────────
    if (ctx.contexts.isIfConditionCtx()) {
        // ✅ Now this function is implemented!
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.contexts.setPendingNarrowing(info);
            expr->valueState = ValueState::Definite;
            return true;
        }
        // No narrowing pattern - continue with normal validation
    }

    // ─── Step 4: Validate operator-specific rules ──────────────────────
    switch (expr->op) {
        // ─── Arithmetic Operators ──────────────────────────────────────────
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod: {
            // Arithmetic operators require numeric target type
            if (!targetType->isNumericType()) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator requires numeric target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Check for nil operands - never allowed in arithmetic
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator cannot be used with nil. Use `??` to handle nil first.");
                return false;
            }

            // Check for err operands
            if (leftIsErr || rightIsErr) {
                // If target is fallible, we can propagate err
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                // Not fallible - reject
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator cannot be used with err. Use `??` to handle err first.");
                return false;
            }

            // Arithmetic operands must be definite (non-nullable, non-fallible)
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator cannot be used with nullable or fallible operands. "
                          "Use `??` to handle nil/err first.");
                return false;
            }

            // Both operands are definite
            expr->valueState = ValueState::Definite;
            return true;
        }

        // ─── Comparison Operators ──────────────────────────────────────────
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge: {
            // Comparison operators require bool target type
            if (!targetType->isBoolType()) {
                ctx.error(expr, DiagCode::E3003,
                          "comparison operator requires bool target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Comparison allows nil and err operands
            // Result is always a definite bool
            expr->valueState = ValueState::Definite;
            return true;
        }

        // ─── Logical Operators ─────────────────────────────────────────────
        case BinaryOp::And:
        case BinaryOp::Or: {
            // Logical operators require bool target type
            if (!targetType->isBoolType()) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator requires bool target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Logical operands must be definite
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator cannot be used with nil");
                return false;
            }

            if (leftIsErr || rightIsErr) {
                // If target is fallible, propagate err
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                ctx.error(expr, DiagCode::E3003,
                          "logical operator cannot be used with err");
                return false;
            }

            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator cannot be used with nullable or fallible operands");
                return false;
            }

            // Both operands are definite
            expr->valueState = ValueState::Definite;
            return true;
        }

        // ─── Bitwise Operators ─────────────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            // Bitwise operators require integer target type
            if (!targetType->isIntegerType()) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator requires integer target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Bitwise operands must be definite
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator cannot be used with nil");
                return false;
            }

            if (leftIsErr || rightIsErr) {
                // If target is fallible, propagate err
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator cannot be used with err");
                return false;
            }

            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator cannot be used with nullable or fallible operands");
                return false;
            }

            // Both operands are definite
            expr->valueState = ValueState::Definite;
            return true;
        }
    }

    return true;
}

// =============================================================================
// checkUnaryExpr
// =============================================================================

/// @brief Type-check a unary expression.
///
/// Validates operand against the target type:
///   - Negation (-x): target must be numeric, operand must be definite
///   - Logical Not (not x): target must be bool, operand must be definite
///   - Bitwise Not (~x): target must be integer, operand must be definite
///
/// Restrictions:
///   - Unary operations are only valid on primitive types (numeric, bool, integer)
///   - Cannot be used on structs, enums, arrays, functions, or traits
///   - Operand must be definite (non-nullable, non-fallible)
///
/// Nullable/Fallible Rules:
///   - Unary operations on nullable/fallible are NOT allowed (must narrow first)
///   - If operand is err and target is fallible, propagate err
///   - If operand is nil, reject (must use ?? first)
///
/// Value State Propagation:
///   - If operand is `err` and target is fallible → result is `err` (recoverable)
///   - If operand is `nil` → rejected for all unary operators
///   - For valid operations, result is `Definite`
///
/// Examples:
///   // ✅ Valid unary operations
///   let x int = -5          // OK: negation on definite int
///   let y bool = not true   // OK: logical not on definite bool
///   let z int = ~0b1010     // OK: bitwise not on definite int
///
///   // ✅ Negation with float
///   let f float = -3.14     // OK: negation on definite float
///
///   // ❌ Invalid: unary on struct
///   struct Point { x float, y float }
///   let p Point = Point { x = 1, y = 2 }
///   let q Point = -p        // ERROR: unary operator cannot be used on struct
///
///   // ❌ Invalid: negation on nullable
///   let x int? = 5
///   let y int? = -x         // ERROR: negation cannot be used with nullable
///
///   // ✅ Correct: narrow first
///   let y int? = -(x ?? 0)  // OK: x is narrowed to int
///
///   // ❌ Invalid: negation on fallible
///   let a int! = 42
///   let b int! = -a         // ERROR: negation cannot be used with fallible
///
///   // ✅ Correct: handle err first
///   let b int! = -(a ?? 0)  // OK: err is handled
///
///   // ✅ Propagate err with fallible target
///   let c int! = -someErr   // OK: result is err (recoverable)
bool checkUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check operand against the target type
    if (!checkExpr(expr->operand, targetType, ctx)) return false;

    // Get operand value state
    ValueState operandState = expr->operand->valueState;
    bool isNil = operandState == ValueState::Nil;
    bool isErr = operandState == ValueState::Err;
    bool isDefinite = operandState == ValueState::Definite;

    // Validate operator-specific rules
    switch (expr->op) {
        // ─── Arithmetic Negation (-x) ─────────────────────────────────────
        case UnaryOp::Neg: {
            // Negation requires numeric target type
            if (!targetType->isNumericType()) {
                ctx.error(expr, DiagCode::E3003,
                          "negation (-) requires numeric target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Check for nil - never allowed
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "negation cannot be used with nil. Use `??` to handle nil first.");
                return false;
            }

            // Check for err - propagate if fallible
            if (isErr) {
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                ctx.error(expr, DiagCode::E3003,
                          "negation cannot be used with err. Use `??` to handle err first.");
                return false;
            }

            // Operand must be definite (non-nullable, non-fallible)
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "negation cannot be used with nullable or fallible operands. "
                          "Use `??` to narrow first.");
                return false;
            }

            // Operand is definite
            expr->valueState = ValueState::Definite;
            return true;
        }

        // ─── Logical Not (not x) ──────────────────────────────────────────
        case UnaryOp::Not: {
            // Logical Not requires bool target type
            if (!targetType->isBoolType()) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not (not) requires bool target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Check for nil - never allowed
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not cannot be used with nil. Use `??` to handle nil first.");
                return false;
            }

            // Check for err - propagate if fallible
            if (isErr) {
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                ctx.error(expr, DiagCode::E3003,
                          "logical not cannot be used with err. Use `??` to handle err first.");
                return false;
            }

            // Operand must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not cannot be used with nullable or fallible operands");
                return false;
            }

            expr->valueState = ValueState::Definite;
            return true;
        }

        // ─── Bitwise Not (~x) ─────────────────────────────────────────────
        case UnaryOp::BitNot: {
            // Bitwise Not requires integer target type
            if (!targetType->isIntegerType()) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not (~) requires integer target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }

            // Check for nil - never allowed
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not cannot be used with nil. Use `??` to handle nil first.");
                return false;
            }

            // Check for err - propagate if fallible
            if (isErr) {
                if (isFallibleType(targetType)) {
                    expr->valueState = ValueState::Err;
                    return true;
                }
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not cannot be used with err. Use `??` to handle err first.");
                return false;
            }

            // Operand must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not cannot be used with nullable or fallible operands");
                return false;
            }

            expr->valueState = ValueState::Definite;
            return true;
        }
    }

    return true;
}

// =============================================================================
// checkCallExpr
// =============================================================================

/// @brief Type-check a function call: resolve the callee, check arguments,
///        and propagate the return type.
///
/// Validates:
///   - Callee resolves to a callable function
///   - Callee is definite (not nullable/fallible)
///   - Generic arguments resolve to valid types
///   - Arguments are assignable to parameter types
///   - Return type is assignable to targetType
///
/// Nullable/Fallible Rules:
///   - Callee cannot be nullable/fallible (must narrow first)
///   - Arguments cannot be fallible (must handle error first)
///   - Arguments may be nullable if parameter type is nullable
///   - If a fallible argument is passed and target is fallible, propagate err
///
/// Generic Rules:
///   - Generic arguments must resolve to valid types
///   - Arity must match the function's generic parameters
///   - Constraints must be satisfied (if any)
///
/// Value State Propagation:
///   - If callee is `err` → reject (cannot call err)
///   - If any argument is `err` → result is `err` (if target is fallible)
///   - If any argument is `nil` → allowed if parameter is nullable
///   - Otherwise, result is the function's return type state
///
/// Examples:
///   // ✅ Simple function call
///   const add (a int)(b int) -> int = { return a + b }
///   let result int = add(5)(3)  // OK
///
///   // ✅ Generic function call
///   const identity<T> (v T) -> T = { return v }
///   let x int = identity<int>(42)  // OK
///
///   // ❌ Generic argument mismatch
///   let x int = identity<string>(42)  // ERROR: type mismatch
///
///   // ❌ Callee is nullable
///   let f (int)->int? = someFunction
///   let result int = f(5)  // ERROR: cannot call nullable callee
///
///   // ✅ Call with nullable argument
///   const process (v int?) -> string = { ... }
///   let x int? = 5
///   let s string = process(x)  // OK: parameter accepts nullable
///
///   // ❌ Call with fallible argument (non-fallible target)
///   const process (v int) -> string = { ... }
///   let x int! = 42
///   let s string = process(x)  // ERROR: cannot pass fallible to non-fallible
///
///   // ✅ Call with fallible argument (fallible target)
///   const process (v int) -> string! = { ... }
///   let x int! = 42
///   let s string! = process(x)  // OK: result is err if x is err
bool checkCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check Callee ─────────────────────────────────────────────
    // Type-check the callee expression
    if (!checkExpr(expr->callee, targetType, ctx)) return false;

    // Callee must be definite (non-nullable, non-fallible)
    if (expr->callee->valueState == ValueState::Nil) {
        ctx.error(expr->callee, DiagCode::E3003,
                  "cannot call nil value. Use `??` to handle nil first.");
        return false;
    }

    if (expr->callee->valueState == ValueState::Err) {
        ctx.error(expr->callee, DiagCode::E3003,
                  "cannot call err value. Use `??` to handle err first.");
        return false;
    }

    // ─── Step 2: Resolve Callee ────────────────────────────────────────────
    // Try to resolve the callee to a function declaration
    const FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);

    if (funcDecl) {
        // ─── Named Function Call ───────────────────────────────────────────

        // Step 2a: Check generic arguments
        if (!expr->genericArgs.empty()) {
            // Resolve each generic argument type
            for (const TypeAST* arg : expr->genericArgs) {
                if (!resolveType(arg, ctx)) {
                    ctx.error(expr, DiagCode::E3002,
                              "invalid generic argument type");
                    return false;
                }
            }

            // Check arity matches the function's generic parameters
            // TODO: Compare expr->genericArgs.size() with funcDecl->genericParams.size()
            // TODO: Check constraints are satisfied
            // TODO: Instantiate the generic function type
        }

        // Step 2b: Check argument count
        // Calculate expected argument count from the function's parameter groups
        size_t expectedArgCount = 0;
        // TODO: Walk through funcDecl->funcType and count parameters
        // TODO: Check expr->args.size() matches expectedArgCount

        // Step 2c: Check each argument type
        bool hasErrArg = false;
        for (size_t i = 0; i < expr->args.size(); ++i) {
            ExprAST* arg = expr->args[i];

            // Get the parameter type for this argument
            // TODO: Get param type from funcDecl->funcType->params[i]
            const TypeAST* paramType = nullptr; // Placeholder

            if (!checkExpr(arg, paramType, ctx)) {
                return false;
            }

            // Track err propagation
            if (arg->valueState == ValueState::Err) {
                hasErrArg = true;
            }

            // Check if argument is fallible and parameter is not nullable/fallible
            if (arg->valueState == ValueState::Err && !isFallibleType(paramType)) {
                ctx.error(arg, DiagCode::E3003,
                          "cannot pass err to non-fallible parameter");
                return false;
            }

            // Check if argument is nil and parameter is not nullable
            if (arg->valueState == ValueState::Nil && !isNullableType(paramType)) {
                ctx.error(arg, DiagCode::E3003,
                          "cannot pass nil to non-nullable parameter");
                return false;
            }
        }

        // Step 2d: Check return type assignability
        if (funcDecl->type && !isAssignable(targetType, funcDecl->type, ctx)) {
            ctx.error(expr, DiagCode::E3003,
                      "return type mismatch: expected ",
                      debug::typeToString(targetType, ctx.pool()),
                      ", got ", debug::typeToString(funcDecl->type, ctx.pool()));
            return false;
        }

        // Step 2e: Propagate value state
        if (hasErrArg) {
            // If any argument is err and target is fallible, propagate err
            if (isFallibleType(targetType)) {
                expr->valueState = ValueState::Err;
            } else {
                // Should have been caught above, but just in case
                expr->valueState = ValueState::Unknown;
            }
        } else {
            // Check if return type is nullable/fallible
            if (isNullableType(targetType)) {
                expr->valueState = ValueState::Unknown; // Could be nil at runtime
            } else if (isFallibleType(targetType)) {
                expr->valueState = ValueState::Unknown; // Could be err at runtime
            } else {
                expr->valueState = ValueState::Definite;
            }
        }

        return true;

    } else {
        // ─── Callee is an expression that produces a function value ──────
        // (e.g., a curried call, an anonymous function, or a function pointer)

        // Verify the callee type is a function type
        if (!expr->callee->resolvedType || !expr->callee->resolvedType->isa<FuncTypeAST>()) {
            ctx.error(expr->callee, DiagCode::E2003,
                      "expression is not callable");
            return false;
        }

        FuncTypeAST* funcType = expr->callee->resolvedType->as<FuncTypeAST>();

        // Check argument count
        size_t expectedArgCount = funcType->params.size();
        if (expr->args.size() != expectedArgCount) {
            ctx.error(expr, DiagCode::E3001,
                      "wrong number of arguments: expected ",
                      std::to_string(expectedArgCount), ", found ",
                      std::to_string(expr->args.size()));
            return false;
        }

        // Check each argument type
        bool hasErrArg = false;
        for (size_t i = 0; i < expr->args.size(); ++i) {
            ExprAST* arg = expr->args[i];
            const ParamAST* param = funcType->params[i];

            if (!checkExpr(arg, param->type, ctx)) {
                return false;
            }

            // Track err propagation
            if (arg->valueState == ValueState::Err) {
                hasErrArg = true;
            }

            // Check if argument is fallible and parameter is not fallible
            if (arg->valueState == ValueState::Err && !isFallibleType(param->type)) {
                ctx.error(arg, DiagCode::E3003,
                          "cannot pass err to non-fallible parameter");
                return false;
            }

            // Check if argument is nil and parameter is not nullable
            if (arg->valueState == ValueState::Nil && !isNullableType(param->type)) {
                ctx.error(arg, DiagCode::E3003,
                          "cannot pass nil to non-nullable parameter");
                return false;
            }
        }

        // Check return type assignability
        if (!funcType->returnType) {
            // Void function
            if (targetType != nullptr) {
                ctx.error(expr, DiagCode::E3003,
                          "void function called in non-void context");
                return false;
            }
            expr->valueState = ValueState::Definite;
            return true;
        }

        const TypeAST* returnType = funcType->returnType;
        if (!isAssignable(targetType, returnType, ctx)) {
            ctx.error(expr, DiagCode::E3003,
                      "return type mismatch: expected ",
                      debug::typeToString(targetType, ctx.pool()),
                      ", got ", debug::typeToString(returnType, ctx.pool()));
            return false;
        }

        // Propagate value state
        if (hasErrArg) {
            if (isFallibleType(targetType)) {
                expr->valueState = ValueState::Err;
            } else {
                expr->valueState = ValueState::Unknown;
            }
        } else {
            if (isNullableType(targetType)) {
                expr->valueState = ValueState::Unknown;
            } else if (isFallibleType(targetType)) {
                expr->valueState = ValueState::Unknown;
            } else {
                expr->valueState = ValueState::Definite;
            }
        }

        return true;
    }
}

// =============================================================================
// checkIntrinsicCallExpr
// =============================================================================

/// @brief Type-check an intrinsic call: validate the intrinsic name and
///        arguments, and return the intrinsic's return type.
/// @note consider improve intrinsic registry to make this function simpiler
bool checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Look up the intrinsic in the registry
    const IntrinsicInfo* info = IntrinsicRegistry::getInstance(ctx.pool())
        .getIntrinsicInfo(expr->intrinsicName);

    if (!info) {
        ctx.error(expr, DiagCode::E3101,
                  "unknown intrinsic '", ctx.pool().lookup(expr->intrinsicName), "'");
        return false;
    }

    // Check argument count
    if (!IntrinsicRegistry::getInstance(ctx.pool())
        .validateArgCount(expr->intrinsicName, expr->args.size())) {
        ctx.error(expr, DiagCode::E3001,
                  "wrong number of arguments for intrinsic '",
                  ctx.pool().lookup(expr->intrinsicName), "'");
        return false;
    }

    // Type-check each argument
    for (ExprAST* arg : expr->args) {
        if (!checkExpr(arg, targetType, ctx)) return false;
        // TODO: Validate argument types for specific intrinsics
    }

    // Store the LLVM intrinsic ID for codegen
    if (info->isValid()) {
        expr->intrinsicID = info->id;
    }

    return true;
}

// =============================================================================
// checkIndexExpr
// =============================================================================

/// @brief Type-check an index expression: verify the target is an array and
///        the index is a definite integer.
///
/// Validates:
///   - target type is the array's element type
///   - target is an array type
///   - index is a definite integer type (not nullable, not fallible)
///   - index must be >= 0 (positive integer, 0 is allowed)
///   - For fixed arrays: index must be within bounds (compile-time check)
///
/// Nullable/Fallible Rules (Same as Arithmetic Operators):
///   - Index must be definite (non-nullable, non-fallible)
///   - Nullable types (T?) are rejected for indexing (must use ?? first)
///   - Fallible types (T!) are rejected for indexing (must use ?? first)
///   - Array element may be nullable (result type inherits nullability)
///
/// Value State Propagation:
///   - If index is `err` → rejected (even with fallible target)
///   - If index is `nil` → rejected
///   - Otherwise, result type is the array's element type
///
/// Examples:
///   // ✅ Valid indexing
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[0]    // OK: index 0 is valid
///   let y int = arr[1]    // OK: index 1 is valid
///
///   // ❌ Index is nullable type (compile-time error)
///   let idx int? = 5
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[idx]  // ERROR: index cannot be nullable. Use `??` to narrow first.
///
///   // ❌ Index is fallible type (compile-time error)
///   let idx int! = 5
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[idx]  // ERROR: index cannot be fallible. Use `??` to handle err first.
///
///   // ✅ Correct: handle nullable/fallible first
///   let x int = arr[idx ?? 0]  // OK: idx is narrowed to int
///
///   // ❌ Index out of bounds (compile-time)
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[3]    // ERROR: index 3 is out of bounds (max 2)
///
///   // ❌ Negative index
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[-1]   // ERROR: index must be >= 0
///
///   // ✅ Runtime index (no compile-time bounds check)
///   let idx int = getUserInput()
///   let arr [3]int = [1, 2, 3]
///   let x int = arr[idx]  // OK: runtime check will catch out-of-bounds
///
///   // ✅ Array with nullable elements (result is nullable)
///   let arr [3]int? = [1, nil, 3]
///   let x int? = arr[1]   // OK: result is int? (nil)
bool checkIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check Target ─────────────────────────────────────────────
    // Type-check the target expression
    if (!checkExpr(expr->target, targetType, ctx)) return false;

    // Target must be an array type
    if (!expr->target->resolvedType || !expr->target->resolvedType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                  "indexing requires an array target type, got ",
                  debug::typeToString(expr->target->resolvedType, ctx.pool()));
        return false;
    }

    const ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();

    // ─── Step 2: Check Index Type (Not Value) ─────────────────────────────
    // The index type must be definite (non-nullable, non-fallible)
    // This is the same rule as arithmetic operators

    // Check if index type is nullable
    if (expr->index->resolvedType && isNullableType(expr->index->resolvedType)) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be nullable. Use `??` to narrow first.");
        return false;
    }

    // Check if index type is fallible
    if (expr->index->resolvedType && isFallibleType(expr->index->resolvedType)) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be fallible. Use `??` to handle err first.");
        return false;
    }

    // ─── Step 3: Type-check Index Expression ──────────────────────────────
    // Check the index expression against the target type
    // This will also check value state for nil/err at the expression level
    if (!checkExpr(expr->index, targetType, ctx)) return false;

    // After checkExpr, we can check value state
    if (expr->index->valueState == ValueState::Nil) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be nil. Use `??` to handle nil first.");
        return false;
    }

    if (expr->index->valueState == ValueState::Err) {
        // Unlike arithmetic, we DO NOT propagate err for indexing
        // Index must be definite - reject err completely
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be err. Use `??` to handle err first.");
        return false;
    }

    // Index must be an integer type
    if (!expr->index->resolvedType || !expr->index->resolvedType->isIntegerType()) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index must be an integer type, got ",
                  debug::typeToString(expr->index->resolvedType, ctx.pool()));
        return false;
    }

    // ─── Step 4: Compile-time Index Value Validation ──────────────────────
    // Try to evaluate the index at compile-time if it's a literal
    if (expr->index->isa<LiteralExprAST>()) {
        const LiteralExprAST* litIndex = expr->index->as<LiteralExprAST>();
        
        // Only check if it's an integer literal
        if (litIndex->kind == LiteralKind::Int || 
            litIndex->kind == LiteralKind::Hex || 
            litIndex->kind == LiteralKind::Binary) {
            
            // Parse the integer value
            // TODO: Actually parse the literal value from litIndex->value
            // For now, this is a placeholder - we need proper integer parsing
            int64_t indexValue = 0; // Placeholder
            
            // Check: index must be >= 0
            if (indexValue < 0) {
                ctx.error(expr->index, DiagCode::E3003,
                          "index must be >= 0, got ", std::to_string(indexValue));
                return false;
            }
            
            // Check: fixed array bounds
            if (arrayType->isFixed()) {
                if (indexValue >= static_cast<int64_t>(arrayType->size)) {
                    ctx.error(expr->index, DiagCode::E3003,
                              "index ", std::to_string(indexValue),
                              " is out of bounds for array of size ",
                              std::to_string(arrayType->size));
                    return false;
                }
            }
        }
    }

    // ─── Step 5: Validate Target Type ─────────────────────────────────────
    // The result type is the array's element type
    // Check if element type is assignable to targetType
    if (!isAssignable(targetType, arrayType->element, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "element type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(arrayType->element, ctx.pool()));
        return false;
    }

    // ─── Step 6: Propagate Value State ────────────────────────────────────
    // Index is definite, result depends on element type
    if (isNullableType(arrayType->element)) {
        expr->valueState = ValueState::Unknown; // Could be nil at runtime
    } else if (isFallibleType(arrayType->element)) {
        // Note: We still reject fallible index types, but element can be fallible
        // This is a different case - the array element itself is fallible
        expr->valueState = ValueState::Unknown; // Could be err at runtime
    } else {
        expr->valueState = ValueState::Definite;
    }

    return true;
}

// =============================================================================
// checkSliceExpr
// =============================================================================

/// @brief Type-check a slice expression: verify the target is an array and
///        the bounds are valid.
///
/// Validates:
///   - target type is the slice type ([_]T)
///   - target is an array type
///   - start/end bounds are definite integer types
///   - start must be >= 0 (if provided)
///   - end must be >= 0 (if provided)
///   - start <= end (if both provided)
///   - end <= array length (for fixed arrays, compile-time check)
///
/// Nullable/Fallible Rules (Same as Index/Arithmetic):
///   - Bounds must be definite (non-nullable, non-fallible)
///   - Bounds cannot be nil or err (must use ?? first)
///   - Result inherits element nullability/fallibility
///
/// Value State Propagation:
///   - If bounds are `err` → rejected (no propagation)
///   - If bounds are `nil` → rejected
///   - Otherwise, result is a slice type with the array's element type
///
/// Examples:
///   // ✅ Valid slicing
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s1 [_]int = arr[0..3]    // OK: [1, 2, 3]
///   let s2 [_]int = arr[1..4]    // OK: [2, 3, 4]
///   let s3 [_]int = arr[0..<5]   // OK: [1, 2, 3, 4, 5] (exclusive end)
///   let s4 [_]int = arr[2..]     // OK: [3, 4, 5] (end defaults to length)
///   let s5 [_]int = arr[..3]     // OK: [1, 2, 3] (start defaults to 0)
///   let s6 [_]int = arr[..]      // OK: entire array
///
///   // ❌ Negative start
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[-1..3]    // ERROR: slice start must be >= 0
///
///   // ❌ Negative end
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[0..-1]    // ERROR: slice end must be >= 0
///
///   // ❌ Start > End
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[3..1]     // ERROR: slice start must be <= end
///
///   // ❌ End out of bounds (fixed array, compile-time)
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[0..6]     // ERROR: slice end 6 is out of bounds (max 5)
///
///   // ❌ Start is nullable
///   let idx int? = 2
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[idx..4]   // ERROR: slice start cannot be nullable
///
///   // ✅ Correct: handle nullable first
///   let s [_]int = arr[(idx ?? 0)..4]  // OK
///
///   // ❌ Start is fallible
///   let idx int! = 2
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[idx..4]   // ERROR: slice start cannot be fallible
///
///   // ✅ Array with nullable elements (result inherits nullability)
///   let arr [5]int? = [1, nil, 3, nil, 5]
///   let s [_]int? = arr[1..3]    // OK: result is [_]int? (contains nil)
///
///   // ✅ Runtime bounds (no compile-time check)
///   let start int = getUserInput()
///   let end int = getUserInput()
///   let arr [5]int = [1, 2, 3, 4, 5]
///   let s [_]int = arr[start..end]  // OK: runtime check will catch invalid bounds
bool checkSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check Target ─────────────────────────────────────────────
    // Type-check the target expression
    if (!checkExpr(expr->target, targetType, ctx)) return false;

    // Target must be an array type
    if (!expr->target->resolvedType || !expr->target->resolvedType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                  "slicing requires an array target type, got ",
                  debug::typeToString(expr->target->resolvedType, ctx.pool()));
        return false;
    }

    const ArrayTypeAST* arrayType = expr->target->resolvedType->as<ArrayTypeAST>();

    // ─── Step 2: Check Start Bounds ────────────────────────────────────────
    // Start is optional (nullptr means 0)
    if (expr->start) {
        // Check if start type is nullable (reject)
        if (expr->start->resolvedType && isNullableType(expr->start->resolvedType)) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be nullable. Use `??` to narrow first.");
            return false;
        }

        // Check if start type is fallible (reject)
        if (expr->start->resolvedType && isFallibleType(expr->start->resolvedType)) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be fallible. Use `??` to handle err first.");
            return false;
        }

        // Type-check the start expression
        if (!checkExpr(expr->start, targetType, ctx)) return false;

        // Check value state
        if (expr->start->valueState == ValueState::Nil) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be nil. Use `??` to handle nil first.");
            return false;
        }

        if (expr->start->valueState == ValueState::Err) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be err. Use `??` to handle err first.");
            return false;
        }

        // Start must be an integer type
        if (!expr->start->resolvedType || !expr->start->resolvedType->isIntegerType()) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start must be an integer type, got ",
                      debug::typeToString(expr->start->resolvedType, ctx.pool()));
            return false;
        }

        // Compile-time validation for literal starts
        if (expr->start->isa<LiteralExprAST>()) {
            const LiteralExprAST* litStart = expr->start->as<LiteralExprAST>();
            if (litStart->kind == LiteralKind::Int || 
                litStart->kind == LiteralKind::Hex || 
                litStart->kind == LiteralKind::Binary) {
                
                int64_t startValue = 0; // TODO: Parse literal value
                
                // Start must be >= 0
                if (startValue < 0) {
                    ctx.error(expr->start, DiagCode::E3003,
                              "slice start must be >= 0, got ", std::to_string(startValue));
                    return false;
                }

                // For fixed arrays, start must be <= length
                if (arrayType->isFixed()) {
                    if (startValue > static_cast<int64_t>(arrayType->size)) {
                        ctx.error(expr->start, DiagCode::E3003,
                                  "slice start ", std::to_string(startValue),
                                  " is out of bounds for array of size ",
                                  std::to_string(arrayType->size));
                        return false;
                    }
                }
            }
        }
    }

    // ─── Step 3: Check End Bounds ──────────────────────────────────────────
    // End is optional (nullptr means array length)
    if (expr->end) {
        // Check if end type is nullable (reject)
        if (expr->end->resolvedType && isNullableType(expr->end->resolvedType)) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be nullable. Use `??` to narrow first.");
            return false;
        }

        // Check if end type is fallible (reject)
        if (expr->end->resolvedType && isFallibleType(expr->end->resolvedType)) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be fallible. Use `??` to handle err first.");
            return false;
        }

        // Type-check the end expression
        if (!checkExpr(expr->end, targetType, ctx)) return false;

        // Check value state
        if (expr->end->valueState == ValueState::Nil) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be nil. Use `??` to handle nil first.");
            return false;
        }

        if (expr->end->valueState == ValueState::Err) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be err. Use `??` to handle err first.");
            return false;
        }

        // End must be an integer type
        if (!expr->end->resolvedType || !expr->end->resolvedType->isIntegerType()) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end must be an integer type, got ",
                      debug::typeToString(expr->end->resolvedType, ctx.pool()));
            return false;
        }

        // Compile-time validation for literal ends
        if (expr->end->isa<LiteralExprAST>()) {
            const LiteralExprAST* litEnd = expr->end->as<LiteralExprAST>();
            if (litEnd->kind == LiteralKind::Int || 
                litEnd->kind == LiteralKind::Hex || 
                litEnd->kind == LiteralKind::Binary) {
                
                int64_t endValue = 0; // TODO: Parse literal value
                
                // End must be >= 0
                if (endValue < 0) {
                    ctx.error(expr->end, DiagCode::E3003,
                              "slice end must be >= 0, got ", std::to_string(endValue));
                    return false;
                }

                // For fixed arrays, end must be <= length
                if (arrayType->isFixed()) {
                    if (endValue > static_cast<int64_t>(arrayType->size)) {
                        ctx.error(expr->end, DiagCode::E3003,
                                  "slice end ", std::to_string(endValue),
                                  " is out of bounds for array of size ",
                                  std::to_string(arrayType->size));
                        return false;
                    }
                }
            }
        }
    }

    // ─── Step 4: Validate Start <= End (if both provided) ──────────────────
    if (expr->start && expr->end) {
        // Only check if both are compile-time literals
        if (expr->start->isa<LiteralExprAST>() && expr->end->isa<LiteralExprAST>()) {
            const LiteralExprAST* litStart = expr->start->as<LiteralExprAST>();
            const LiteralExprAST* litEnd = expr->end->as<LiteralExprAST>();
            
            if ((litStart->kind == LiteralKind::Int || 
                 litStart->kind == LiteralKind::Hex || 
                 litStart->kind == LiteralKind::Binary) &&
                (litEnd->kind == LiteralKind::Int || 
                 litEnd->kind == LiteralKind::Hex || 
                 litEnd->kind == LiteralKind::Binary)) {
                
                int64_t startValue = 0; // TODO: Parse literal value
                int64_t endValue = 0;   // TODO: Parse literal value
                
                // Check: start must be <= end
                if (startValue > endValue) {
                    ctx.error(expr, DiagCode::E3003,
                              "slice start (", std::to_string(startValue),
                              ") must be <= slice end (", std::to_string(endValue), ")");
                    return false;
                }
            }
        }
    }

    // ─── Step 5: Validate Target Type ─────────────────────────────────────
    // The result is always a slice type ([_]T)
    // Create a slice type with the array's element type
    TypeAST* sliceType = ctx.arena().makeType<ArrayTypeAST>(
        ArrayKind::Slice, 0, arrayType->element
    );

    // Check if slice type is assignable to targetType
    if (!isAssignable(targetType, sliceType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "slice type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(sliceType, ctx.pool()));
        return false;
    }

    // ─── Step 6: Propagate Value State ────────────────────────────────────
    // Bounds are definite, result is a slice
    // Value state depends on element type
    if (isNullableType(arrayType->element)) {
        expr->valueState = ValueState::Unknown; // Could contain nil at runtime
    } else if (isFallibleType(arrayType->element)) {
        expr->valueState = ValueState::Unknown; // Could contain err at runtime
    } else {
        expr->valueState = ValueState::Definite;
    }

    return true;
}

// =============================================================================
// checkFieldAccessExpr
// =============================================================================

/// @brief Type-check a field access: verify the object has the field and
///        return the field's type.
///
/// Validates:
///   - target type is the field's type
///   - object is a struct or enum type
///   - field exists on the object
///   - object must be definite (non-nullable, non-fallible)
///   - Accessing fields on nullable requires NullableChainExpr (?.)
///   - Accessing fields on fallible is NOT allowed (must handle err first)
///
/// Nullable/Fallible Rules:
///   - Object must be definite (use ?. for nullable, handle error for fallible)
///   - Field access on nullable requires NullableChainExpr (?.)
///   - Field access on fallible is NOT allowed (must use ?? first)
///   - If object is `err` → rejected (no propagation)
///   - If object is `nil` → rejected (use ?. instead)
///
/// Value State Propagation:
///   - If object is `err` → rejected (must handle first)
///   - If object is `nil` → rejected (use ?. instead)
///   - Otherwise, result type is the field's type
///
/// Examples:
///   // ✅ Valid field access
///   struct Point { x float, y float }
///   let p Point = Point { x = 1.0, y = 2.0 }
///   let x float = p.x   // OK: 1.0
///   let y float = p.y   // OK: 2.0
///
///   // ❌ Field access on nullable (use ?. instead)
///   struct Point { x float, y float }
///   let p Point? = Point { x = 1.0, y = 2.0 }
///   let x float = p.x   // ERROR: cannot access field on nullable type. Use `?.` instead.
///
///   // ✅ Correct: use ?. for nullable
///   let x float? = p?.x // OK: result is float?
///
///   // ❌ Field access on fallible (must handle first)
///   struct Point { x float, y float }
///   let p Point! = Point { x = 1.0, y = 2.0 }
///   let x float = p.x   // ERROR: cannot access field on fallible type. Use `??` first.
///
///   // ✅ Correct: handle err first
///   let p Point! = somePoint
///   let x float = (p ?? Point { x = 0, y = 0 }).x  // OK
///
///   // ❌ Object is nil
///   struct Point { x float, y float }
///   let p Point? = nil
///   let x float = p.x   // ERROR: cannot access field on nil. Use `?.` instead.
///
///   // ✅ Enum variant access
///   enum Direction { North = 0, South = 1, East = 2, West = 3 }
///   let dir Direction = Direction.North  // OK: access variant
///
///   // ❌ Field doesn't exist
///   struct Point { x float, y float }
///   let p Point = Point { x = 1.0, y = 2.0 }
///   let z float = p.z   // ERROR: struct 'Point' has no field named 'z'
bool checkFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check Object ─────────────────────────────────────────────
    // Type-check the object expression
    if (!checkExpr(expr->object, targetType, ctx)) return false;

    // Object must be definite (non-nullable, non-fallible)
    if (expr->object->valueState == ValueState::Nil) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on nil. Use `?.` for nullable access.");
        return false;
    }

    if (expr->object->valueState == ValueState::Err) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on err. Use `??` to handle err first.");
        return false;
    }

    // Check if object type is nullable (reject - must use ?.)
    if (expr->object->resolvedType && isNullableType(expr->object->resolvedType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on nullable type. Use `?.` for nullable access.");
        return false;
    }

    // Check if object type is fallible (reject - must handle first)
    if (expr->object->resolvedType && isFallibleType(expr->object->resolvedType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on fallible type. Use `??` to handle err first.");
        return false;
    }

    // ─── Step 2: Resolve Object Type ──────────────────────────────────────
    // Object must resolve to a named type (struct or enum)
    if (!expr->object->resolvedType || !expr->object->resolvedType->isa<NamedTypeAST>()) {
        ctx.error(expr->object, DiagCode::E2002,
                  "field access requires a struct or enum type, got ",
                  debug::typeToString(expr->object->resolvedType, ctx.pool()));
        return false;
    }

    const NamedTypeAST* namedType = expr->object->resolvedType->as<NamedTypeAST>();
    
    // Look up the type declaration by name (it should be registered already)
    const TypeDeclAST* typeDecl = lookupType(namedType->name, ctx);
    if (!typeDecl) {
        ctx.error(expr, DiagCode::E2002,
                  "undefined type '", ctx.pool().lookup(namedType->name), "'");
        return false;
    }

    // ─── Step 3: Handle Struct Type ───────────────────────────────────────
    if (typeDecl->isa<StructDeclAST>()) {
        const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        // Find the field
        const FieldDeclAST* field = nullptr;
        for (const FieldDeclAST* f : structDecl->fields) {
            if (f->name == expr->fieldName) {
                field = f;
                break;
            }
        }

        if (!field) {
            ctx.error(expr, DiagCode::E2001,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' has no field named '", ctx.pool().lookup(expr->fieldName), "'");
            return false;
        }

        // Check if field type is assignable to targetType
        if (!isAssignable(targetType, field->type, ctx)) {
            ctx.error(expr, DiagCode::E3003,
                      "field type mismatch: expected ",
                      debug::typeToString(targetType, ctx.pool()),
                      ", got ", debug::typeToString(field->type, ctx.pool()));
            return false;
        }

        // ─── Step 4: Check Const Field Assignment ────────────────────────
        // If this is a module member access, it's always read-only
        if (expr->isModuleMember) {
            // Module member access is always read-only
            // TODO: Mark the result as const
        }

        // ─── Step 5: Propagate Value State ────────────────────────────────
        // Object is definite, result depends on field type
        if (isNullableType(field->type)) {
            expr->valueState = ValueState::Unknown; // Could be nil at runtime
        } else if (isFallibleType(field->type)) {
            expr->valueState = ValueState::Unknown; // Could be err at runtime
        } else {
            expr->valueState = ValueState::Definite;
        }

        return true;
    }

    // ─── Step 4: Handle Enum Type ─────────────────────────────────────────
    if (typeDecl->isa<EnumDeclAST>()) {
        const EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();

        // Find the variant
        const EnumVariantAST* variant = nullptr;
        for (const EnumVariantAST* v : enumDecl->variants) {
            if (v->name == expr->fieldName) {
                variant = v;
                break;
            }
        }

        if (!variant) {
            ctx.error(expr, DiagCode::E2001,
                      "enum '", ctx.pool().lookup(enumDecl->name),
                      "' has no variant named '", ctx.pool().lookup(expr->fieldName), "'");
            return false;
        }

        // Enum variant's type is the enum itself
        // Since the enum is registered, we can just create a NamedTypeAST
        // that references it by name (or use the enum declaration directly)
        const TypeAST* variantType = ctx.arena().makeType<NamedTypeAST>(enumDecl->name);

        // Check if variant type is assignable to targetType
        if (!isAssignable(targetType, variantType, ctx)) {
            ctx.error(expr, DiagCode::E3003,
                      "variant type mismatch: expected ",
                      debug::typeToString(targetType, ctx.pool()),
                      ", got ", debug::typeToString(variantType, ctx.pool()));
            return false;
        }

        // Enum variants are definite values
        expr->valueState = ValueState::Definite;
        return true;
    }

    // ─── Step 5: Unknown Type ─────────────────────────────────────────────
    ctx.error(expr, DiagCode::E2002,
              "field access on unsupported type: ",
              debug::typeToString(expr->object->resolvedType, ctx.pool()));
    return false;
}

// =============================================================================
// checkModuleAccessExpr
// =============================================================================

/// @brief Type-check a module access expression: resolve the module and
///        member, returning the member's type.
///
/// Validates:
///   - target type is the member's type
///   - module alias resolves to a valid module
///   - member exists in the module's exports
///   - Member type is assignable to targetType
///
/// Module access is always read-only.
/// Module members are always definite (module-level values are fully resolved).
///
/// Nullable/Fallible Rules:
///   - Module members can be nullable/fallible (the module itself is definite)
///   - The result type inherits the member's nullability/fallibility
///   - No special restrictions on module access
///
/// Value State Propagation:
///   - Module access is always definite (module exists at compile-time)
///   - Result state depends on the member's type
///   - If member is nullable → result is `Unknown` (could be nil at runtime)
///   - If member is fallible → result is `Unknown` (could be err at runtime)
///   - Otherwise → result is `Definite`
///
/// Examples:
///   // ✅ Valid module access
///   import std.math as math
///   let pi float = math:PI        // OK: access exported constant
///   let result float = math:sqrt(16.0)  // OK: call exported function
///
///   // ✅ Module with nullable member
///   import std.optional as opt
///   let maybe int? = opt:getValue()  // OK: result is int?
///
///   // ✅ Module with fallible member
///   import std.io as io
///   let content string! = io:readFile("file.txt")  // OK: result is string!
///
///   // ❌ Module alias doesn't exist
///   let x = unknown:member  // ERROR: undefined module alias 'unknown'
///
///   // ❌ Member doesn't exist in module
///   import std.math as math
///   let x = math:unknown  // ERROR: module 'math' has no exported member 'unknown'
///
///   // ❌ Member is not exported (not in module table)
///   import std.internal as internal
///   let x = internal:secret  // ERROR: module 'internal' has no exported member 'secret'
bool checkModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Look up the module alias ─────────────────────────────────
    // The module alias should have been registered during import analysis
    ModuleAST* module = ctx.symbols.lookupImport(expr->moduleName);
    if (!module) {
        ctx.error(expr, DiagCode::E2001,
                  "undefined module alias '", ctx.pool().lookup(expr->moduleName), "'");
        return false;
    }

    // ─── Step 2: Get the module's table ───────────────────────────────────
    // The module table should exist (module was analyzed before this)
    ModuleTable* table = ctx.symbols.findModuleTable(module);
    if (!table) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName), "' has not been analyzed");
        return false;
    }

    // ─── Step 3: Look up the member ──────────────────────────────────────
    // Members are in the value namespace (top-level values: const, functions)
    auto it = table->values.find(expr->memberName);
    if (it == table->values.end()) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName),
                  "' has no exported member '", ctx.pool().lookup(expr->memberName), "'");
        return false;
    }

    const ValueDeclAST* decl = it->second;

    // ─── Step 4: Mark as module member (read-only) ──────────────────────
    // Module members are always read-only from outside the module
    expr->isModuleMember = true;

    // ─── Step 5: Check generic arguments if present ──────────────────────
    if (!expr->genericArgs.empty()) {
        // Resolve each generic argument type
        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.error(expr, DiagCode::E3002,
                          "invalid generic argument type in module access");
                return false;
            }
        }
        // TODO: Check generic argument arity and constraints against the
        //       member's generic parameters (if the member is generic)
    }

    // ─── Step 6: Verify member type is assignable to targetType ──────────
    if (!decl->type) {
        ctx.error(expr, DiagCode::E3003,
                  "member '", ctx.pool().lookup(expr->memberName),
                  "' has no type information");
        return false;
    }

    if (!isAssignable(targetType, decl->type, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(decl->type, ctx.pool()));
        return false;
    }

    // ─── Step 7: Propagate Value State ────────────────────────────────────
    // Module access is definite (module exists at compile-time)
    // Result state depends on the member's type
    if (isNullableType(decl->type)) {
        expr->valueState = ValueState::Unknown; // Could be nil at runtime
    } else if (isFallibleType(decl->type)) {
        expr->valueState = ValueState::Unknown; // Could be err at runtime
    } else {
        expr->valueState = ValueState::Definite;
    }

    return true;
}

// =============================================================================
// checkNullableChainExpr
// =============================================================================

/// @brief Type-check a nullable chain expression: each step is only evaluated
///        if the previous value is non-nil.
///
/// Validates:
///   - target type is the final field's type (nullable)
///   - Each step is a field access on a nullable type
///   - The chain must be terminated by ?? (checked at parent)
///   - Base must be nullable (T?), not fallible (T!)
///   - Each step must be nullable
///
/// Nullable/Fallible Rules:
///   - Base must be nullable (T?), not fallible (T!)
///   - Each step must be nullable
///   - Fallible values cannot be chained (must handle error first)
///   - The result is always nullable (if any step is nil, result is nil)
///
/// Value State Propagation:
///   - If any step is `nil` → result is `nil` (short-circuit)
///   - If base is `err` → rejected (fallible cannot be chained)
///   - Otherwise, result is nullable (could be nil at runtime)
///
/// Examples:
///   // ✅ Valid nullable chain
///   struct Player { name string, weapon Weapon? }
///   struct Weapon { damage int }
///   let p Player? = getPlayer()
///   let damage int? = p?.weapon?.damage  // OK: result is int?
///
///   // ❌ Base is fallible (not allowed)
///   struct Player { name string, weapon Weapon? }
///   let p Player! = getPlayer()
///   let damage int? = p?.weapon?.damage  // ERROR: ?. cannot be used on fallible type
///
///   // ❌ Step is fallible (not allowed)
///   struct Player { name string, weapon Weapon! }
///   let p Player? = getPlayer()
///   let damage int? = p?.weapon?.damage  // ERROR: ?. step cannot be fallible
///
///   // ✅ Chain must be terminated by ?? (checked at parent)
///   let damage int? = p?.weapon?.damage ?? 0  // OK: terminated by ??
///
///   // ❌ Chain without ?? (compile-time error)
///   let damage int? = p?.weapon?.damage  // ERROR: ?. chain must be terminated by ??
///
///   // ✅ Multiple steps with nullable types
///   struct A { b B? }
///   struct B { c C? }
///   struct C { value int }
///   let a A? = getA()
///   let result int? = a?.b?.c?.value  // OK: all steps are nullable
///
///   // ❌ Step is not nullable (compile-time error)
///   struct A { b B }  // B is not nullable
///   let a A? = getA()
///   let result int? = a?.b?.value  // ERROR: step 'b' must be nullable
bool checkNullableChainExpr(NullableChainExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E3003, "empty nullable chain");
        return false;
    }

    // ─── Step 1: Check Base Object ────────────────────────────────────────
    // Type-check the base expression
    if (!checkExpr(expr->object, targetType, ctx)) return false;

    // Base must be nullable (T?), not fallible (T!)
    if (expr->object->resolvedType && !isNullableType(expr->object->resolvedType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain requires a nullable base type (T?), got ",
                  debug::typeToString(expr->object->resolvedType, ctx.pool()));
        return false;
    }

    // Base cannot be fallible (even if it's also nullable, i.e., T?!)
    if (expr->object->resolvedType && isFallibleType(expr->object->resolvedType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain cannot be used on fallible type. Use `??` to handle err first.");
        return false;
    }

    // Base cannot be err
    if (expr->object->valueState == ValueState::Err) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain cannot be used on err. Use `??` to handle err first.");
        return false;
    }

    // ─── Step 2: Walk Through Each Step ──────────────────────────────────
    // The chain result is nullable (if any step is nil, the result is nil)
    // We track the current type as we walk through the chain
    const TypeAST* currentType = expr->object->resolvedType;
    bool hasNilStep = false;

    for (const InternedString& step : expr->steps) {
        // The current type must be nullable
        if (!currentType || !isNullableType(currentType)) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step requires nullable type, got ",
                      debug::typeToString(currentType, ctx.pool()));
            return false;
        }

        // Unwrap the nullable to get the inner type
        const TypeAST* innerType = unwrapNullable(const_cast<TypeAST*>(currentType));
        if (!innerType) {
            ctx.error(expr, DiagCode::E3003,
                      "cannot unwrap nullable type");
            return false;
        }

        // The inner type must be a struct or enum (to access a field)
        if (!innerType->isa<NamedTypeAST>()) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step requires struct or enum type, got ",
                      debug::typeToString(innerType, ctx.pool()));
            return false;
        }

        // Resolve the field access
        const NamedTypeAST* namedType = innerType->as<NamedTypeAST>();
        const TypeDeclAST* typeDecl = lookupType(namedType->name, ctx);
        if (!typeDecl) {
            ctx.error(expr, DiagCode::E2002,
                      "undefined type '", ctx.pool().lookup(namedType->name), "'");
            return false;
        }

        // Find the field
        const FieldDeclAST* field = nullptr;
        if (typeDecl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
            for (const FieldDeclAST* f : structDecl->fields) {
                if (f->name == step) {
                    field = f;
                    break;
                }
            }
        } else if (typeDecl->isa<EnumDeclAST>()) {
            // Enum variants can also be accessed via ?.
            const EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();
            for (const EnumVariantAST* v : enumDecl->variants) {
                if (v->name == step) {
                    // Enum variant's type is the enum itself
                    const TypeAST* variantType = ctx.arena().makeType<NamedTypeAST>(enumDecl->name);
                    currentType = variantType;
                    break;
                }
            }
            // If variant not found, we'll handle below
        }

        if (!field) {
            ctx.error(expr, DiagCode::E2001,
                      "type has no field named '", ctx.pool().lookup(step), "'");
            return false;
        }

        // The field type must be nullable (to continue the chain)
        if (!isNullableType(field->type)) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step '", ctx.pool().lookup(step),
                      "' must be nullable, got ",
                      debug::typeToString(field->type, ctx.pool()));
            return false;
        }

        // Update current type to the field's type for the next step
        currentType = field->type;
    }

    // ─── Step 3: Final Type Validation ────────────────────────────────────
    // The final result is nullable (if any step was nil, result is nil)
    // Check if the final type is assignable to targetType
    if (!isAssignable(targetType, currentType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(currentType, ctx.pool()));
        return false;
    }

    // ─── Step 4: Propagate Value State ────────────────────────────────────
    // The chain result is always nullable (could be nil at runtime)
    // But if we can determine at compile-time that a step is nil, we can propagate
    if (hasNilStep) {
        expr->valueState = ValueState::Nil;
    } else {
        // Result is nullable - could be nil at runtime
        expr->valueState = ValueState::Unknown;
    }

    return true;
}

// =============================================================================
// checkNullCoalesceExpr
// =============================================================================

/// @brief Type-check a null coalesce expression: value ?? fallback.
///
/// The null coalesce operator provides a fallback value when the LHS is nil or err.
///
/// Validates:
///   - LHS is nullable or fallible (T?, T!, or T?!)
///   - RHS type is assignable to the unwrapped type
///   - target type is the RHS type (or unwrapped LHS type)
///
/// Nullable/Fallible Rules:
///   - LHS can be T?, T!, or T?!
///   - ?? unwraps T? to T (handles nil)
///   - ?? unwraps T! to T (handles err)
///   - ?? unwraps T?! to T (handles nil and err)
///   - RHS must be definite (not nullable/fallible) or match unwrapped type
///
/// Value State Propagation:
///   - If LHS is `nil` and RHS is definite → result is RHS (definite)
///   - If LHS is `err` and RHS is definite → result is RHS (definite)
///   - If LHS is definite → result is LHS (definite)
///   - If LHS is unknown → result is unknown (could be nil/err at runtime)
///
/// Examples:
///   // ✅ Nullable with fallback
///   let x int? = 5
///   let y int = x ?? 0  // OK: y = 5 (x is not nil)
///
///   // ✅ Nullable with nil
///   let x int? = nil
///   let y int = x ?? 0  // OK: y = 0 (x is nil, use fallback)
///
///   // ✅ Fallible with fallback
///   let x int! = 42
///   let y int = x ?? 0  // OK: y = 42 (x is not err)
///
///   // ✅ Fallible with err
///   let x int! = err
///   let y int = x ?? 0  // OK: y = 0 (x is err, use fallback)
///
///   // ✅ Combined T?! with fallback (handles both nil and err)
///   let x int?! = nil
///   let y int = x ?? 0  // OK: y = 0
///
///   // ❌ LHS is not nullable or fallible
///   let x int = 5
///   let y int = x ?? 0  // ERROR: ?? requires nullable or fallible LHS
///
///   // ❌ RHS type mismatch
///   let x int? = 5
///   let y string = x ?? "default"  // ERROR: type mismatch (int vs string)
///
///   // ✅ RHS is fallible (propagates err)
///   let x int? = 5
///   let y int! = x ?? err  // OK: y is int!
///
///   // ✅ RHS is nullable (propagates nil)
///   let x int? = 5
///   let y int? = x ?? nil  // OK: y is int?
///
///   // ✅ Chained null coalesce
///   let a int? = nil
///   let b int? = 5
///   let c int = a ?? b ?? 0  // OK: c = 5 (a is nil, b is 5)
bool checkNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check LHS ──────────────────────────────────────────────────
    // The LHS must be nullable or fallible
    if (!checkExpr(expr->value, targetType, ctx)) return false;

    // LHS must be nullable or fallible (type-level check)
    if (!expr->value->resolvedType) {
        ctx.error(expr->value, DiagCode::E3003,
                  "?? requires nullable or fallible LHS (T?, T!, or T?!)");
        return false;
    }

    if (!isNullableType(expr->value->resolvedType) && 
        !isFallibleType(expr->value->resolvedType)) {
        ctx.error(expr->value, DiagCode::E3003,
                  "?? requires nullable or fallible LHS, got ",
                  debug::typeToString(expr->value->resolvedType, ctx.pool()));
        return false;
    }

    // ─── Step 2: Unwrap LHS Type ────────────────────────────────────────────
    // Unwrap both nullable and fallible to get the inner type
    const TypeAST* lhsInner = expr->value->resolvedType;
    if (isNullableType(lhsInner)) {
        lhsInner = unwrapNullable(const_cast<TypeAST*>(lhsInner));
    }
    if (isFallibleType(lhsInner)) {
        lhsInner = unwrapFallible(const_cast<TypeAST*>(lhsInner));
    }

    if (!lhsInner) {
        ctx.error(expr, DiagCode::E3003,
                  "cannot unwrap LHS type");
        return false;
    }

    // ─── Step 3: Check RHS ──────────────────────────────────────────────────
    // The RHS must be assignable to the unwrapped LHS type
    if (!checkExpr(expr->fallback, lhsInner, ctx)) return false;

    // RHS type must be assignable to the unwrapped LHS type
    if (!expr->fallback->resolvedType) {
        ctx.error(expr->fallback, DiagCode::E3003,
                  "fallback has no type information");
        return false;
    }

    if (!isAssignable(lhsInner, expr->fallback->resolvedType, ctx)) {
        ctx.error(expr->fallback, DiagCode::E3003,
                  "fallback type mismatch: expected ",
                  debug::typeToString(lhsInner, ctx.pool()),
                  ", got ", debug::typeToString(expr->fallback->resolvedType, ctx.pool()));
        return false;
    }

    // ─── Step 4: Check Target Type ─────────────────────────────────────────
    // The result type is the RHS type (or the unwrapped LHS type if RHS is absent)
    // In Lucid, ?? always has both LHS and RHS, so we use the RHS type
    const TypeAST* resultType = expr->fallback->resolvedType;

    if (!isAssignable(targetType, resultType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "result type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(resultType, ctx.pool()));
        return false;
    }

    // ─── Step 5: Propagate Value State ─────────────────────────────────────
    // The value state of the null coalesce expression depends on both operands
    ValueState lhsState = expr->value->valueState;
    ValueState rhsState = expr->fallback->valueState;

    // If LHS is nil or err, result is RHS
    if (lhsState == ValueState::Nil || lhsState == ValueState::Err) {
        expr->valueState = rhsState;
    }
    // If LHS is definite, result is LHS (definite)
    else if (lhsState == ValueState::Definite) {
        expr->valueState = ValueState::Definite;
    }
    // If LHS is unknown, result is unknown (depends on runtime)
    else {
        expr->valueState = ValueState::Unknown;
    }

    return true;
}

// =============================================================================
// checkAssignExpr
// =============================================================================

/// @brief Type-check an assignment: verify the LHS and RHS types are compatible.
///
/// This function only checks type compatibility between LHS and RHS.
/// Context-specific rules (const, module members, etc.) are handled by callers.
///
/// Validates:
///   - RHS type is assignable to LHS type
///   - For compound assignments (+=, -=, etc.): operator is valid on the type
///
/// Nullable/Fallible Rules:
///   - RHS cannot be fallible if LHS is not fallible
///   - RHS can be nullable if LHS is nullable (widening)
///   - RHS cannot be nullable if LHS is definite (narrowing - requires ??)
///
/// Value State Propagation:
///   - Assignment produces the value of the RHS (or the LHS value for compound)
///   - If RHS is `err` and LHS is fallible → propagate `err`
///   - If RHS is `nil` and LHS is nullable → propagate `nil`
///
/// @param expr The assignment expression.
/// @param targetType The LHS type (target of assignment).
/// @param ctx The semantic context.
/// @return true if the assignment is type-compatible.
///
/// Examples:
///   // ✅ Valid assignment
///   let x int = 5
///   x = 10  // OK: int = int
///
///   // ✅ Assignment with widening (int → int?)
///   let x int? = nil
///   x = 5  // OK: int is assignable to int?
///
///   // ❌ Assignment with narrowing (int? → int)
///   let x int = 5
///   let y int? = 10
///   x = y  // ERROR: int? is not assignable to int (use ??)
///
///   // ❌ RHS is fallible (non-fallible LHS)
///   let x int = 5
///   let y int! = 10
///   x = y  // ERROR: int! is not assignable to int
///
///   // ✅ RHS is fallible (fallible LHS)
///   let x int! = 5
///   let y int! = 10
///   x = y  // OK: int! = int!
///
///   // ✅ Compound assignment (requires numeric type)
///   let x int = 5
///   x += 3  // OK: int += int
///
///   // ❌ Compound assignment on non-numeric type
///   struct Point { x float, y float }
///   let p Point = Point { x = 1.0, y = 2.0 }
///   p += Point { x = 1.0, y = 1.0 }  // ERROR: += not supported for struct
bool checkAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── Step 1: Check LHS Expression ──────────────────────────────────────
    // LHS must be a valid expression (type-check it)
    if (!checkExpr(expr->lhs, targetType, ctx)) return false;

    // ─── Step 2: Check RHS Expression ──────────────────────────────────────
    // Type-check the RHS against the LHS type
    if (!checkExpr(expr->rhs, targetType, ctx)) return false;

    // RHS must have a resolved type
    if (!expr->rhs->resolvedType) {
        ctx.error(expr->rhs, DiagCode::E3003,
                  "RHS has no type information");
        return false;
    }

    // ─── Step 3: Check Type Assignability ──────────────────────────────────
    // RHS type must be assignable to LHS type
    if (!isAssignable(targetType, expr->rhs->resolvedType, ctx)) {
        ctx.error(expr->rhs, DiagCode::E3003,
                  "type mismatch: expected ",
                  debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(expr->rhs->resolvedType, ctx.pool()));
        return false;
    }

    // ─── Step 4: Compound Assignment Operator Validation ──────────────────
    // For compound assignments (+=, -=, *=, etc.), the operator must be valid
    // on the LHS type
    if (expr->op != AssignOp::Assign) {
        switch (expr->op) {
            case AssignOp::AddAssign:
            case AssignOp::SubAssign:
            case AssignOp::MulAssign:
            case AssignOp::DivAssign:
            case AssignOp::PowAssign:
            case AssignOp::ModAssign: {
                // Arithmetic compound assignments require numeric type
                if (!targetType->isNumericType()) {
                    ctx.error(expr, DiagCode::E3003,
                              "arithmetic compound assignment requires numeric type, got ",
                              debug::typeToString(targetType, ctx.pool()));
                    return false;
                }
                break;
            }

            case AssignOp::BitAndAssign:
            case AssignOp::BitOrAssign:
            case AssignOp::BitXorAssign:
            case AssignOp::ShlAssign:
            case AssignOp::ShrAssign: {
                // Bitwise compound assignments require integer type
                if (!targetType->isIntegerType()) {
                    ctx.error(expr, DiagCode::E3003,
                              "bitwise compound assignment requires integer type, got ",
                              debug::typeToString(targetType, ctx.pool()));
                    return false;
                }
                break;
            }

            default:
                // Unknown compound operator
                ctx.error(expr, DiagCode::E3003,
                          "unknown compound assignment operator");
                return false;
        }

        // For compound assignments, RHS must also be compatible with the operator
        // The RHS type must match the LHS type (or be assignable)
        // This is already checked by isAssignable above
    }

    // ─── Step 5: Propagate Value State ─────────────────────────────────────
    // Assignment produces the value of the RHS (or the LHS value for compound)
    // For simple assignment, result is RHS
    if (expr->op == AssignOp::Assign) {
        expr->valueState = expr->rhs->valueState;
    } else {
        // Compound assignment: result is the LHS value after operation
        // For simplicity, use the RHS state
        expr->valueState = expr->rhs->valueState;
    }

    return true;
}

// =============================================================================
// checkPipelineStep
// =============================================================================

/// @brief Type-check a single pipeline step: verify the step is callable
///        with the input type and return the output type.
///
/// @param step The pipeline step.
/// @param inputType The type flowing into this step.
/// @param targetType The expected output type.
/// @param ctx The semantic context.
bool checkPipelineStep(PipelineStepAST* step, const TypeAST* inputType, const TypeAST* targetType, SemaContext& ctx) {
    if (!step || !inputType || !targetType) return false;

    // Type-check the callable
    if (!checkExpr(step->callable, targetType, ctx)) return false;

    // TODO: Verify callable is a function type
    // TODO: Verify first parameter matches inputType
    // TODO: Verify return type matches targetType

    return true;
}

// =============================================================================
// checkPipelineExpr
// =============================================================================

/// @brief Type-check a pipeline expression: the seed type flows through
///        each step, and each step must be callable with the input type.
///
/// Validates:
///   - target type is the final output type
///   - Seed type matches first step's input
///   - Each step's output matches next step's input
///
/// Nullable/Fallible Rules:
///   - Pipeline short-circuits on err
///   - Steps cannot be fallible functions (must handle error first)
bool checkPipelineExpr(PipelineExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E1107, "pipeline has no steps");
        return false;
    }

    // Check seed against first step's input type
    if (!checkExpr(expr->seed, targetType, ctx)) return false;

    // Walk through each step
    const TypeAST* currentType = targetType;
    for (PipelineStepAST* step : expr->steps) {
        if (!checkPipelineStep(step, currentType, targetType, ctx)) return false;
        // TODO: Update currentType to step's output type
    }

    return true;
}

// =============================================================================
// checkComposeOperand
// =============================================================================

/// @brief Type-check a composition operand: resolve the callable and
///        return its type.
bool checkComposeOperand(ComposeOperandAST* operand, const TypeAST* targetType, SemaContext& ctx) {
    if (!operand || !targetType) return false;

    // Type-check the callable
    if (!checkExpr(operand->callable, targetType, ctx)) return false;

    // Check generic arguments if present
    if (!operand->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints
    }

    // TODO: Verify callable is a function type
    // TODO: Verify function type matches targetType

    return true;
}

// =============================================================================
// checkComposeExpr
// =============================================================================

/// @brief Type-check a composition expression: f +> g +> h
///
/// The output type of each operand must match the input type of the next.
///
/// Validates:
///   - target type is the composed function type
///   - Each operand is a function type
///   - Output of left matches input of right
///
/// Nullable/Fallible Rules:
///   - Fallible functions cannot be composed
///   - Nullable functions cannot be composed (must handle first)
bool checkComposeExpr(ComposeExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    if (expr->operands.empty()) {
        ctx.error(expr, DiagCode::E3003, "composition has no operands");
        return false;
    }

    // Start with the left operand
    if (!checkComposeOperand(expr->left->as<ComposeOperandAST>(), targetType, ctx)) return false;

    // Walk through each right operand
    for (ComposeOperandAST* operand : expr->operands) {
        if (!checkComposeOperand(operand, targetType, ctx)) return false;
        // TODO: Verify output of previous matches input of current
    }

    // TODO: Verify composed function type matches targetType

    return true;
}

/// @brief Type-check an anonymous function expression: resolve its type
///        and analyze its body.
///
/// Validates:
///   - target type is the function type
///   - Parameters are valid
///   - Body returns the correct type
bool checkAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // ─── 1. Verify target type is a function type ─────────────────────────
    if (!targetType->isa<FuncTypeAST>()) {
        ctx.error(expr, DiagCode::E3003,
                  "anonymous function requires a function target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        return false;
    }

    // ─── 2. Resolve the function type ─────────────────────────────────────
    // Checks if all parameter and return types exist in scope
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(targetType->as<FuncTypeAST>());
    resolveFuncType(funcType, ctx);

    // ─── 3. Check if the function type matches the expr's funcType ──────
    // The expr already has a funcType from the parser, ensure it matches
    if (!typesEqual(funcType, expr->funcType)) {
        ctx.error(expr, DiagCode::E3003,
                  "function type mismatch: expected ",
                  debug::typeToString(funcType, ctx.pool()),
                  ", got ", debug::typeToString(expr->funcType, ctx.pool()));
        return false;
    }

    // ─── 4. Analyze parameters ────────────────────────────────────────────
    // Parameters are registered in the function's scope
    // Walk through all curry groups
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            analyzeParam(param, ctx);
        }
    }

    // ─── 5. Push function context with return requirements ───────────────
    ctx.contexts.pushAnonFunction(expr, funcType, expr->loc);

    if (!expr->body) {
        ctx.error(expr, DiagCode::E3003, "anonymous function has no body");
        ctx.contexts.pop();
        return false;
    }

    // ─── 6. Analyze the body ──────────────────────────────────────────────
    bool bodyReturns = false;
    if (expr->body->isa<BlockStmtAST>()) {
        bodyReturns = analyzeBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body->isa<ReturnStmtAST>()) {
        bodyReturns = analyzeReturnStmt(expr->body->as<ReturnStmtAST>(), ctx);
    } else {
        ctx.error(expr, DiagCode::E3003, 
                  "anonymous function has invalid body type");
        ctx.contexts.pop();
        return false;
    }

    // ─── 7. Verify return paths ──────────────────────────────────────────
    // Check if requirements are satisfied
    if (bodyReturns && !ctx.contexts.returnRequirementsSatisfied()) {
        ctx.error(expr, DiagCode::E3005,
                  "anonymous function has missing nested return");
    }

    // ─── 8. Pop function context ──────────────────────────────────────────
    ctx.contexts.pop();

    // ─── 9. Set resolved type and value state ────────────────────────────
    expr->resolvedType = const_cast<TypeAST*>(targetType);
    
    // Check if return type is nullable/fallible
    if (isNullableType(targetType)) {
        expr->valueState = ValueState::Unknown; // Could be nil at runtime
    } else if (isFallibleType(targetType)) {
        expr->valueState = ValueState::Unknown; // Could be err at runtime
    } else {
        expr->valueState = ValueState::Definite;
    }

    return true;
}

// =============================================================================
// checkIfExpr
// =============================================================================

/// @brief Type-check an if expression: both branches must produce compatible types.
///
/// Validates:
///   - target type is the common type of both branches
///   - Condition is bool or coercible to bool
///   - Both branches produce compatible types
///
/// Nullable/Fallible Rules:
///   - Condition must be definite (non-nullable, non-fallible)
bool checkIfExpr(IfExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check condition - must be definite bool
    if (!checkExpr(expr->condition, targetType, ctx)) return false;

    // Check then branch
    if (!checkExpr(expr->thenBranch, targetType, ctx)) return false;

    // Check else branch
    if (!checkExpr(expr->elseBranch, targetType, ctx)) return false;

    // TODO: Verify condition is bool
    // TODO: Verify both branches produce compatible types
    // TODO: Verify common type matches targetType

    return true;
}

// =============================================================================
// checkRangeExpr
// =============================================================================

/// @brief Type-check a range expression: verify both bounds are numeric.
///
/// Ranges don't have a standalone type; they're only used in for loops,
/// slices, and switch cases.
///
/// Validates:
///   - target type is the numeric element type
///   - Both bounds are numeric and same type
///   - Bounds are definite (non-nullable, non-fallible)
bool checkRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check lower bound
    if (!checkExpr(expr->lo, targetType, ctx)) return false;

    // Check upper bound
    if (!checkExpr(expr->hi, targetType, ctx)) return false;

    // TODO: Verify both bounds are numeric and same type
    // TODO: Verify bounds are definite

    return true;
}

} // namespace sema