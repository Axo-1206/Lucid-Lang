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
/// NULLABLE AND FALLIBLE TYPE RULES
/// ============================================================================
/// 
/// Lucid has three special type modifiers:
///   - T?  : Nullable - can be nil or T
///   - T!  : Fallible - can be err or T
///   - T?! : Combined - can be nil, err, or T
/// 
/// ┌─────────────────────────────────────────────────────────────────────────────┐
/// │ EXPRESSION KIND          │ NULLABLE (T?)    │ FALLIBLE (T!)   │ COMBINED  │
/// │                          │                   │                 │  (T?!)    │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Arithmetic (+, -, *, /)  │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Comparison (==, !=,      │ ✅ Allowed        │ ❌ Not allowed  │ ❌ Not    │
/// │ <, <=, >, >=)            │ (nil comparison)  │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Logical (and, or)        │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Bitwise (&, |, ^, <<, >>)│ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Unary Negation (-x)      │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Logical Not (not x)      │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Bitwise Not (~x)         │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Function Call (callee)   │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (must narrow)     │                 │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Function Call (argument) │ ✅ Allowed        │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (if param is T?)  │ (must handle)   │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Field Access (obj.field) │ ❌ Not allowed    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ (use ?. instead)  │ (must handle)   │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Nullable Chain (?.)      │ ✅ Allowed        │ ❌ Not allowed  │ ❌ Not    │
/// │                          │                   │ (must handle)   │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Null Coalesce (??)       │ ✅ Allowed        │ ✅ Allowed      │ ✅ Allowed│
/// │                          │ (handles nil)     │ (handles err)   │ (handles  │
/// │                          │                   │                 │ both)     │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Array Index (arr[i])     │ ✅ Element can    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ be nullable       │ (must handle)   │ allowed   │
/// │                          │ ❌ Index must be  │                 │           │
/// │                          │ definite          │                 │           │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Slice Bounds (start..end)│ ❌ Bounds must    │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ be definite       │ (must handle)   │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Assignment (RHS → LHS)   │ ✅ T → T? allowed │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ ❌ T? → T not     │ (must handle)   │ allowed   │
/// │                          │ allowed           │                 │           │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Return Statement         │ ✅ If return      │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ type is T?        │ (must handle)   │ allowed   │
/// ├──────────────────────────┼───────────────────┼─────────────────┼───────────┤
/// │ Condition (if, while)    │ ❌ Must be        │ ❌ Not allowed  │ ❌ Not    │
/// │                          │ definite bool     │ (must handle)   │ allowed   │
/// └──────────────────────────┴───────────────────┴─────────────────┴───────────┘
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
///   - int/hex/binary literal → integer target type
///   - float literal → float target type
///   - string/rawstring literal → string target type
///   - char literal → char target type
///   - true/false → bool target type
///   - nil → nullable target type (T?)
///   - err → fallible target type (T!)
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
        return true;
    }

    if (expr->kind == LiteralKind::Err) {
        if (!isFallibleType(targetType)) {
            ctx.error(expr, DiagCode::E3003,
                      "err literal requires a fallible target type (T!), got ",
                      debug::typeToString(targetType, ctx.pool()));
            return false;
        }
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
            // For now, just validate the type category
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
            return true;
        }

        case LiteralKind::Char: {
            if (targetKind != PrimitiveKind::Char) {
                ctx.error(expr, DiagCode::E3003,
                          "char literal requires char target type, got ",
                          debug::typeToString(targetType, ctx.pool()));
                return false;
            }
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
bool checkIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    const ValueDeclAST* decl = resolveValueOrError(expr, ctx);
    if (!decl) return false;

    // Check if the resolved type is assignable to the target type
    if (decl->type && !isAssignable(targetType, decl->type, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ", debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()),
                  ", got ", debug::typeToString(decl->type, ctx.pool()));
        return false;
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

    // Check each element against the element type
    for (ExprAST* elem : expr->elements) {
        if (!checkExpr(elem, elemTarget, ctx)) {
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

    // For empty array literals, no further validation needed
    // The type is already known from targetType

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
/// Validates both operands against the target type, then operator-specific rules.
///
/// Nullable/Fallible Rules:
///   - Arithmetic/Logical/Bitwise: operands must be definite (non-nullable, non-fallible)
///   - Comparison: operands may be nullable (nil comparison allowed), but not fallible
bool checkBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check both operands against the target type
    if (!checkExpr(expr->left, targetType, ctx)) return false;
    if (!checkExpr(expr->right, targetType, ctx)) return false;

    // Now validate operator-specific rules
    switch (expr->op) {
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
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operands must be definite (non-nullable, non-fallible)
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator requires definite operands, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            return true;
        }

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
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Comparison allows nullable (nil comparison) but not fallible
            if (isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "comparison operator does not allow fallible operands");
                return false;
            }
            return true;
        }

        case BinaryOp::And:
        case BinaryOp::Or: {
            // Logical operators require bool target type
            if (!targetType->isBoolType()) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator requires bool target type, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operands must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator requires definite operands");
                return false;
            }
            return true;
        }

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            // Bitwise operators require integer target type
            if (!targetType->isIntegerType()) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator requires integer target type, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operands must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator requires definite operands");
                return false;
            }
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
/// Nullable/Fallible Rules:
///   - Unary operations on nullable/fallible are NOT allowed (must narrow first)
bool checkUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check operand against the target type
    if (!checkExpr(expr->operand, targetType, ctx)) return false;

    // Validate operator-specific rules
    switch (expr->op) {
        case UnaryOp::Neg: {
            // Negation requires numeric target type
            if (!targetType->isNumericType()) {
                ctx.error(expr, DiagCode::E3003,
                          "negation requires numeric target type, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operand must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "negation requires definite operand");
                return false;
            }
            return true;
        }

        case UnaryOp::Not: {
            // Logical Not requires bool target type
            if (!targetType->isBoolType()) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not requires bool target type, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operand must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not requires definite operand");
                return false;
            }
            return true;
        }

        case UnaryOp::BitNot: {
            // Bitwise Not requires integer target type
            if (!targetType->isIntegerType()) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not requires integer target type, got ",
                          debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()));
                return false;
            }
            // Operand must be definite
            if (isNullableType(targetType) || isFallibleType(targetType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not requires definite operand");
                return false;
            }
            return true;
        }
    }

    return true;
}

// =============================================================================
// checkCallExpr
// =============================================================================

/// @brief Type-check a function call: resolve the callee, check arguments,
///        and return the function's return type.
///
/// Validates:
///   - Callee resolves to a callable function
///   - Callee is definite (not nullable/fallible)
///   - Arguments are assignable to parameter types
///   - Return type is assignable to targetType
///
/// Nullable/Fallible Rules:
///   - Callee cannot be nullable/fallible (must narrow first)
///   - Arguments cannot be fallible (must handle error first)
///   - Arguments may be nullable if parameter type is nullable
bool checkCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Type-check the callee - must be definite
    if (!checkExpr(expr->callee, targetType, ctx)) return false;

    // Try to resolve the callee to a function declaration
    const FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);

    if (funcDecl) {
        // Named function call
        // Check generic arguments
        if (!expr->genericArgs.empty()) {
            // TODO: Check generic argument arity and constraints against the
            //       function's generic parameters
        }

        // Check argument count
        // TODO: Calculate expected argument count from the function's parameter groups
        // TODO: Check expr->args size matches expectedArgCount

        // Check each argument type - arguments cannot be fallible
        // TODO: Check each argument is assignable to the corresponding parameter type

        // Return type must be assignable to targetType
        if (funcDecl->type && !isAssignable(targetType, funcDecl->type, ctx)) {
            ctx.error(expr, DiagCode::E3003,
                      "return type mismatch: expected ",
                      debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()),
                      ", got ", debug::typeToString(funcDecl->type, ctx.pool()));
            return false;
        }

        return true;
    } else {
        // Callee is an expression that produces a function value
        // TODO: Check callee type is a function type and validate arguments
        // TODO: Validate return type is assignable to targetType
        return false;
    }
}

// =============================================================================
// checkIntrinsicCallExpr
// =============================================================================

/// @brief Type-check an intrinsic call: validate the intrinsic name and
///        arguments, and return the intrinsic's return type.
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
///        the index is an integer.
///
/// Validates:
///   - target type is the array's element type
///   - target is an array type
///   - index is a definite integer type
///
/// Nullable/Fallible Rules:
///   - Index must be definite (non-nullable, non-fallible)
///   - Array element may be nullable (result type inherits nullability)
bool checkIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check target is an array type
    if (!checkExpr(expr->target, targetType, ctx)) return false;

    // Check index is a definite integer type
    if (!checkExpr(expr->index, targetType, ctx)) return false;

    // TODO: Verify index type is integer and definite
    // TODO: Verify index is within bounds for fixed arrays

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
///
/// Nullable/Fallible Rules:
///   - Bounds must be definite (non-nullable, non-fallible)
///   - Result inherits element nullability
bool checkSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check target is an array type
    if (!checkExpr(expr->target, targetType, ctx)) return false;

    // Type-check start and end bounds if present
    if (expr->start) {
        if (!checkExpr(expr->start, targetType, ctx)) return false;
        // TODO: Verify start is definite integer type
    }

    if (expr->end) {
        if (!checkExpr(expr->end, targetType, ctx)) return false;
        // TODO: Verify end is definite integer type
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
///
/// Nullable/Fallible Rules:
///   - Object must be definite (use ?. for nullable, handle error for fallible)
///   - Field access on nullable requires NullableChainExpr (?.)
///   - Field access on fallible is NOT allowed
bool checkFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Type-check the object - must be definite
    if (!checkExpr(expr->object, targetType, ctx)) return false;

    // Resolve the object type to its declaration
    // TODO: Resolve object type and find the field
    // TODO: Verify field type is assignable to targetType

    return true;
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
///
/// Module access is always read-only.
bool checkModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Look up the module alias
    ModuleAST* module = ctx.symbols.lookupImport(expr->moduleName);
    if (!module) {
        ctx.error(expr, DiagCode::E2001,
                  "undefined module alias '", ctx.pool().lookup(expr->moduleName), "'");
        return false;
    }

    // Get the module's table
    ModuleTable* table = ctx.symbols.findModuleTable(module);
    if (!table) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName), "' has not been analyzed");
        return false;
    }

    // Look up the member in the module's value namespace
    auto it = table->values.find(expr->memberName);
    if (it == table->values.end()) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName),
                  "' has no exported member '", ctx.pool().lookup(expr->memberName), "'");
        return false;
    }

    const ValueDeclAST* decl = it->second;

    // Mark the access as read-only (module members are always read-only)
    expr->isModuleMember = true;

    // Check generic arguments if present
    if (!expr->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints
    }

    // Verify member type is assignable to targetType
    if (decl->type && !isAssignable(targetType, decl->type, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ", debug::typeToString(const_cast<TypeAST*>(targetType), ctx.pool()),
                  ", got ", debug::typeToString(decl->type, ctx.pool()));
        return false;
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
///
/// Nullable/Fallible Rules:
///   - Base must be nullable (T?), not fallible (T!)
///   - Each step must be nullable
///   - Fallible values cannot be chained (must handle error first)
bool checkNullableChainExpr(NullableChainExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E3003, "empty nullable chain");
        return false;
    }

    // TODO: Walk through each step, validating nullability
    // TODO: Verify final type is assignable to targetType

    return true;
}

// =============================================================================
// checkNullCoalesceExpr
// =============================================================================

/// @brief Type-check a null coalesce expression: the LHS must be nullable or
///        fallible, and the RHS must be assignable to the unwrapped type.
///
/// Validates:
///   - LHS is nullable or fallible
///   - RHS type is assignable to the unwrapped type
///   - target type is the RHS type (or unwrapped LHS type)
///
/// Nullable/Fallible Rules:
///   - LHS can be T?, T!, or T?!
///   - ?? unwraps T? to T (handles nil)
///   - ?? unwraps T! to T (handles err)
///   - ?? unwraps T?! to T (handles nil and err)
///   - RHS must be definite (not nullable/fallible) or match unwrapped type
bool checkNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check LHS - must be nullable or fallible
    if (!checkExpr(expr->value, targetType, ctx)) return false;

    // Check RHS - must be definite and assignable to unwrapped type
    if (!checkExpr(expr->fallback, targetType, ctx)) return false;

    // TODO: Verify LHS is nullable/fallible
    // TODO: Verify RHS matches unwrapped LHS type

    return true;
}

// =============================================================================
// checkAssignExpr
// =============================================================================

/// @brief Type-check an assignment: verify the LHS is an assignable lvalue
///        and the RHS is assignable to the LHS type.
///
/// Validates:
///   - target type is the LHS type
///   - LHS is an assignable lvalue
///   - RHS is assignable to LHS type
///
/// Nullable/Fallible Rules:
///   - RHS cannot be fallible (must handle error first)
///   - RHS can be nullable if LHS is nullable (widening)
///   - RHS cannot be nullable if LHS is definite (narrowing - requires ??)
bool checkAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // Check LHS - must be an assignable lvalue
    if (!checkExpr(expr->lhs, targetType, ctx)) return false;

    // Check RHS - must be assignable to LHS type
    if (!checkExpr(expr->rhs, targetType, ctx)) return false;

    // TODO: Verify LHS is assignable lvalue
    // TODO: Check if LHS is const (reject assignment)
    // TODO: For compound assignments (+=, -=, etc.), verify operator validity

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
    TypeAST* currentType = const_cast<TypeAST*>(targetType);
    for (PipelineStepAST* step : expr->steps) {
        if (!checkPipelineStep(step, currentType, targetType, ctx)) return false;
        // TODO: Update currentType to step's output type
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
bool checkPipelineStep(PipelineStepAST* step, TypeAST* inputType, const TypeAST* targetType, SemaContext& ctx) {
    if (!step || !inputType || !targetType) return false;

    // Type-check the callable
    if (!checkExpr(step->callable, targetType, ctx)) return false;

    // TODO: Verify callable is a function type
    // TODO: Verify first parameter matches inputType
    // TODO: Verify return type matches targetType

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
    if (!checkComposeOperand(expr->left, targetType, ctx)) return false;

    // Walk through each right operand
    for (ComposeOperandAST* operand : expr->operands) {
        if (!checkComposeOperand(operand, targetType, ctx)) return false;
        // TODO: Verify output of previous matches input of current
    }

    // TODO: Verify composed function type matches targetType

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
// checkAnonFuncExpr
// =============================================================================

/// @brief Type-check an anonymous function expression: resolve its type
///        and analyze its body.
///
/// Validates:
///   - target type is the function type
///   - Parameters are valid
///   - Body returns the correct type
bool checkAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    // TODO: Verify targetType is a function type
    // TODO: Resolve all parameter and return types
    // TODO: Analyze the body inside a FuncBody context
    // TODO: Verify body returns the correct type

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