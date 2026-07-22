/**
 * @file SemaExpr.cpp
 * @brief Implements Sema.hpp's "Expressions (Type Checking)" section —
 *        checkExpr() and every specific check*Expr() function.
 *
 * @architectural_note Every check*Expr() function:
 *   1. Resolves names (via ctx.lookupValue()/lookupType() or helpers like
 *      resolveValueOrError())
 *   2. Type-checks sub-expressions
 *   3. Computes the expression's type
 *   4. Sets expr->resolvedType before returning
 *
 *   If a sub-expression fails to type-check (returns nullptr), the parent
 *   should propagate nullptr but NOT report a second diagnostic (the
 *   sub-expression already reported its own error).
 *
 * @architectural_note Type of an expression
 *   The resolved type is always a TypeAST pointer. For expressions that
 *   produce no value (e.g., void function calls), resolvedType is nullptr.
 */

#include "../Sema.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "../support/IntrinsicRegistry.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

// =============================================================================
// checkExpr — Dispatch
// =============================================================================

/**
 * @brief Dispatch an expression to its specific check*Expr() function.
 *
 * Sets expr->resolvedType before returning. Returns nullptr if the expression
 * could not be type-checked (an error was already reported).
 *
 * @param expr The expression to check (may be nullptr — returns nullptr).
 * @param ctx  The semantic context.
 * @return The expression's resolved type, or nullptr on error.
 */
TypeAST* checkExpr(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* result = nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = checkLiteralExpr(expr->as<LiteralExprAST>(), ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = checkIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = checkArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
            break;
        case ASTKind::StructLiteralExpr:
            result = checkStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
            break;
        case ASTKind::BinaryExpr:
            result = checkBinaryExpr(expr->as<BinaryExprAST>(), ctx);
            break;
        case ASTKind::UnaryExpr:
            result = checkUnaryExpr(expr->as<UnaryExprAST>(), ctx);
            break;
        case ASTKind::CallExpr:
            result = checkCallExpr(expr->as<CallExprAST>(), ctx);
            break;
        case ASTKind::IntrinsicCallExpr:
            result = checkIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx);
            break;
        case ASTKind::IndexExpr:
            result = checkIndexExpr(expr->as<IndexExprAST>(), ctx);
            break;
        case ASTKind::SliceExpr:
            result = checkSliceExpr(expr->as<SliceExprAST>(), ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = checkFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
            break;
        case ASTKind::ModuleAccessExpr:
            result = checkModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx);
            break;
        case ASTKind::NullableChainExpr:
            result = checkNullableChainExpr(expr->as<NullableChainExprAST>(), ctx);
            break;
        case ASTKind::NullCoalesceExpr:
            result = checkNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
            break;
        case ASTKind::AssignExpr:
            result = checkAssignExpr(expr->as<AssignExprAST>(), ctx);
            break;
        case ASTKind::PipelineExpr:
            result = checkPipelineExpr(expr->as<PipelineExprAST>(), ctx);
            break;
        case ASTKind::ComposeExpr:
            result = checkComposeExpr(expr->as<ComposeExprAST>(), ctx);
            break;
        case ASTKind::AnonFuncExpr:
            result = checkAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
            break;
        case ASTKind::IfExpr:
            result = checkIfExpr(expr->as<IfExprAST>(), ctx);
            break;
        case ASTKind::RangeExpr:
            result = checkRangeExpr(expr->as<RangeExprAST>(), ctx);
            break;
        default:
            // Unknown/error-recovery expression
            expr->resolvedType = nullptr;
            return nullptr;
    }

    expr->resolvedType = result;
    return result;
}

// =============================================================================
// checkLiteralExpr
// =============================================================================

/**
 * @brief Type-check a literal expression.
 *
 * Returns the primitive type corresponding to the literal kind:
 *   - Int/Hex/Binary → int
 *   - Float → float
 *   - String/RawString → string
 *   - Char → char
 *   - True/False → bool
 *   - Nil → nullable version of whatever type it's assigned to (inferred)
 *   - Err → fallible version of whatever type it's assigned to (inferred)
 *
 * @param expr The literal expression.
 * @param ctx  The semantic context.
 * @return The literal's type.
 */
TypeAST* checkLiteralExpr(LiteralExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    PrimitiveKind kind;
    switch (expr->kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            kind = PrimitiveKind::Int;
            break;
        case LiteralKind::Float:
            kind = PrimitiveKind::Float;
            break;
        case LiteralKind::String:
        case LiteralKind::RawString:
            kind = PrimitiveKind::String;
            break;
        case LiteralKind::Char:
            kind = PrimitiveKind::Char;
            break;
        case LiteralKind::True:
        case LiteralKind::False:
            kind = PrimitiveKind::Bool;
            break;
        case LiteralKind::Nil:
            // nil's type is inferred from context. We return nullptr here
            // (unknown type) and let the parent expression handle inference.
            // The parent (e.g., checkAssignExpr, checkVarDecl) will determine
            // the actual type and set it.
            return nullptr;
        case LiteralKind::Err:
            // err's type is inferred from context, similar to nil.
            return nullptr;
        default:
            return nullptr;
    }

    return ctx.arena.makeType<PrimitiveTypeAST>(kind);
}

// =============================================================================
// checkIdentifierExpr
// =============================================================================

/**
 * @brief Type-check an identifier expression: resolve the name and return
 *        its value type.
 *
 * @param expr The identifier expression.
 * @param ctx  The semantic context.
 * @return The resolved value's type, or nullptr on error.
 */
TypeAST* checkIdentifierExpr(IdentifierExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    ValueDeclAST* decl = resolveValueOrError(expr, ctx);
    if (!decl) {
        return nullptr;
    }

    // Store the resolved declaration for later use (codegen, etc.)
    // TODO: Store decl->valueType as the resolved type

    // If this is a generic function identifier, check generic arguments
    if (!expr->genericArgs.empty()) {
        // TODO: Verify this is a generic function declaration
        // TODO: Check generic argument arity and constraints
        // TODO: Instantiate the generic function type
    }

    return decl->valueType;
}

// =============================================================================
// checkArrayLiteralExpr
// =============================================================================

/**
 * @brief Type-check an array literal: verify all elements have the same type.
 *
 * The array kind (slice/dynamic/fixed) is inferred from context (assignment
 * or type annotation). The literal itself is kind-neutral.
 *
 * @param expr The array literal expression.
 * @param ctx  The semantic context.
 * @return The array's type (ArrayTypeAST), or nullptr on error.
 */
TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    if (expr->elements.empty()) {
        // Empty array literal: type is inferred from context.
        // Return nullptr and let the parent infer the type.
        return nullptr;
    }

    // Type-check the first element to determine the element type
    TypeAST* firstType = checkExpr(expr->elements[0], ctx);
    if (!firstType) {
        // First element failed to type-check; propagate error
        return nullptr;
    }

    // Verify all remaining elements have the same type
    for (size_t i = 1; i < expr->elements.size(); ++i) {
        TypeAST* elemType = checkExpr(expr->elements[i], ctx);
        if (!elemType) {
            // This element failed to type-check; propagate error
            return nullptr;
        }
        if (!typesEqual(firstType, elemType)) {
            ctx.error(expr->elements[i], DiagCode::E3003,
                       "array element type mismatch: expected ",
                       ctx.toString(firstType), ", found ",
                       ctx.toString(elemType));
        }
    }

    // The array kind is inferred from context. We create a placeholder
    // ArrayTypeAST with Slice kind, and the parent will set the correct kind.
    // TODO: The array kind should be set by the parent (assignment context)
    //       based on the target type.
    ArrayTypeAST* arrayType = ctx.arena.makeType<ArrayTypeAST>(
        ArrayKind::Slice, 0, firstType
    );

    return arrayType;
}

// =============================================================================
// checkStructLiteralExpr
// =============================================================================

/**
 * @brief Type-check a struct literal: resolve the struct type and validate
 *        all field initializers.
 *
 * @param expr The struct literal expression.
 * @param ctx  The semantic context.
 * @return The struct's type (NamedTypeAST), or nullptr on error.
 */
TypeAST* checkStructLiteralExpr(StructLiteralExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    // Resolve the struct type name
    NamedTypeAST* typeNode = ctx.arena.makeType<NamedTypeAST>(expr->typeName);
    TypeDeclAST* typeDecl = resolveTypeNameOrError(typeNode, ctx);
    if (!typeDecl) {
        return nullptr;
    }

    // Check generic arguments
    if (!expr->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints against the
        //       struct's generic parameters
    }

    // Verify it's a struct (not an enum or trait)
    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.error(expr, DiagCode::E2002,
                   "expected struct type, found ",
                   ctx.toString(expr->typeName));
        return nullptr;
    }

    StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Track which fields have been initialized
    std::unordered_set<InternedString> initializedFields;

    // Check each field initializer
    for (FieldInitAST* init : expr->inits) {
        checkFieldInit(init, structDecl, ctx);
        initializedFields.insert(init->name);
    }

    // Verify all required fields (const fields without defaults) are initialized
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->isConst && !field->defaultVal) {
            if (initializedFields.find(field->name) == initializedFields.end()) {
                ctx.error(expr, DiagCode::E3002,
                           "const field '", ctx.toString(field->name),
                           "' must be initialized");
            }
        }
    }

    // Cache the instantiated type
    expr->instantiatedType = typeNode;

    // The struct literal's type is the struct type itself
    // TODO: Create a NamedTypeAST with the resolved generic arguments
    return typeNode;
}

// =============================================================================
// checkFieldInit
// =============================================================================

/**
 * @brief Type-check a single field initializer in a struct literal.
 *
 * @param init        The field initializer.
 * @param targetStruct The struct being constructed.
 * @param ctx         The semantic context.
 */
void checkFieldInit(FieldInitAST* init, StructDeclAST* targetStruct, SemaContext& ctx) {
    if (!init || !targetStruct) return;

    // Find the field in the struct
    FieldDeclAST* field = nullptr;
    for (FieldDeclAST* f : targetStruct->fields) {
        if (f->name == init->name) {
            field = f;
            break;
        }
    }

    if (!field) {
        ctx.error(init, DiagCode::E2001,
                   "struct '", ctx.toString(targetStruct->name),
                   "' has no field named '", ctx.toString(init->name), "'");
        return;
    }

    // Type-check the initializer value
    TypeAST* valueType = checkExpr(init->value, ctx);
    if (!valueType) return;

    // Check assignability to the field type
    TypeAST* fieldType = field->type;
    if (!isAssignable(fieldType, valueType, ctx)) {
        ctx.error(init->value, DiagCode::E3003,
                   "type mismatch for field '", ctx.toString(init->name),
                   "': expected ", ctx.toString(fieldType),
                   ", found ", ctx.toString(valueType));
    }
}

// =============================================================================
// checkBinaryExpr
// =============================================================================

/**
 * @brief Type-check a binary expression.
 *
 * The type depends on the operator:
 *   - Arithmetic (+, -, *, /, %, **): numeric → numeric
 *   - Comparison (==, !=, <, <=, >, >=): any → bool
 *   - Logical (and, or): any (coerced to bool) → bool
 *   - Bitwise (&, |, ^, <<, >>): integer → integer
 *
 * @param expr The binary expression.
 * @param ctx  The semantic context.
 * @return The expression's type, or nullptr on error.
 */
TypeAST* checkBinaryExpr(BinaryExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* leftType = checkExpr(expr->left, ctx);
    TypeAST* rightType = checkExpr(expr->right, ctx);

    if (!leftType || !rightType) {
        return nullptr;
    }

    switch (expr->op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod: {
            // Arithmetic operators require numeric types
            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                ctx.error(expr, DiagCode::E3003,
                           "arithmetic operator requires numeric operands");
                return nullptr;
            }
            // Result type is the type of the operands (they must be the same)
            // TODO: Handle type promotion (int + float → float, etc.)
            if (!typesEqual(leftType, rightType)) {
                ctx.error(expr, DiagCode::E3003,
                           "arithmetic operands must have the same type");
                return nullptr;
            }
            return leftType;
        }

        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge: {
            // Comparison operators require comparable types
            // TODO: Verify types are comparable (primitives, structs, etc.)
            // Result is always bool
            return ctx.arena.makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }

        case BinaryOp::And:
        case BinaryOp::Or: {
            // Logical operators coerce to bool, result is bool
            // The operands can be any type (coerced via truthiness)
            // We just need to ensure they're valid (not an unknown type)
            // TODO: Verify truthiness coercion is valid for the operand types
            return ctx.arena.makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            // Bitwise operators require integer types
            if (!isIntegerType(leftType) || !isIntegerType(rightType)) {
                ctx.error(expr, DiagCode::E3003,
                           "bitwise operator requires integer operands");
                return nullptr;
            }
            if (!typesEqual(leftType, rightType)) {
                ctx.error(expr, DiagCode::E3003,
                           "bitwise operands must have the same type");
                return nullptr;
            }
            return leftType;
        }
    }

    return nullptr;
}

// =============================================================================
// checkUnaryExpr
// =============================================================================

/**
 * @brief Type-check a unary expression.
 *
 * @param expr The unary expression.
 * @param ctx  The semantic context.
 * @return The expression's type, or nullptr on error.
 */
TypeAST* checkUnaryExpr(UnaryExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* operandType = checkExpr(expr->operand, ctx);
    if (!operandType) return nullptr;

    switch (expr->op) {
        case UnaryOp::Neg:
            // Arithmetic negation requires numeric type
            if (!isNumericType(operandType)) {
                ctx.error(expr, DiagCode::E3003,
                           "negation requires numeric operand");
                return nullptr;
            }
            return operandType;

        case UnaryOp::Not:
            // Logical negation accepts any type (coerced to bool), result is bool
            // TODO: Verify truthiness coercion is valid for the operand type
            return ctx.arena.makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);

        case UnaryOp::BitNot:
            // Bitwise NOT requires integer type
            if (!isIntegerType(operandType)) {
                ctx.error(expr, DiagCode::E3003,
                           "bitwise NOT requires integer operand");
                return nullptr;
            }
            return operandType;
    }

    return nullptr;
}

// =============================================================================
// checkCallExpr
// =============================================================================

/**
 * @brief Type-check a function call: resolve the callee, check arguments,
 *        and return the function's return type.
 *
 * @param expr The call expression.
 * @param ctx  The semantic context.
 * @return The function's return type, or nullptr on error.
 */
TypeAST* checkCallExpr(CallExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    // Type-check the callee
    TypeAST* calleeType = checkExpr(expr->callee, ctx);
    if (!calleeType) return nullptr;

    // Try to resolve the callee to a function declaration
    FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);

    if (funcDecl) {
        // Named function call
        // Check generic arguments
        if (!expr->genericArgs.empty()) {
            // TODO: Check generic argument arity and constraints against the
            //       function's generic parameters
        }

        // Check argument count
        size_t expectedArgCount = 0;
        // TODO: Calculate expected argument count from the function's parameter groups
        // TODO: Check expr->args size matches expectedArgCount

        // Check each argument type
        // TODO: Check each argument is assignable to the corresponding parameter type

        // Return the function's return type
        return funcDecl->resolvedReturnType;
    } else {
        // Callee is an expression that produces a function value
        // (e.g., a curried call, an anonymous function, or a function pointer)

        // Verify the callee type is a function type
        if (!calleeType->isa<FuncTypeAST>()) {
            ctx.error(expr->callee, DiagCode::E2003,
                       "expression is not callable");
            return nullptr;
        }

        FuncTypeAST* funcType = calleeType->as<FuncTypeAST>();

        // Check argument count
        size_t expectedArgCount = funcType->params.size();
        if (expr->args.size() != expectedArgCount) {
            ctx.error(expr, DiagCode::E3001,
                       "wrong number of arguments: expected ",
                       std::to_string(expectedArgCount), ", found ",
                       std::to_string(expr->args.size()));
            return nullptr;
        }

        // Check each argument type
        for (size_t i = 0; i < expr->args.size(); ++i) {
            TypeAST* argType = checkExpr(expr->args[i], ctx);
            if (!argType) continue;

            ParamAST* param = funcType->params[i];
            if (!isAssignable(param->type, argType, ctx)) {
                ctx.error(expr->args[i], DiagCode::E3003,
                           "argument type mismatch for parameter ",
                           std::to_string(i + 1));
            }
        }

        // Return the function's return type
        if (funcType->returnTypes.empty()) {
            return nullptr; // void return
        }
        return funcType->returnTypes[0];
    }
}

// =============================================================================
// checkIntrinsicCallExpr
// =============================================================================

/**
 * @brief Type-check an intrinsic call: validate the intrinsic name and
 *        arguments, and return the intrinsic's return type.
 *
 * @param expr The intrinsic call expression.
 * @param ctx  The semantic context.
 * @return The intrinsic's return type, or nullptr on error.
 */
TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    // Look up the intrinsic in the registry
    const IntrinsicInfo* info = IntrinsicRegistry::getInstance(ctx.pool)
        .getIntrinsicInfo(expr->intrinsicName);

    if (!info) {
        ctx.error(expr, DiagCode::E3101,
                   "unknown intrinsic '", ctx.toString(expr->intrinsicName), "'");
        return nullptr;
    }

    // Check argument count
    if (!IntrinsicRegistry::getInstance(ctx.pool)
        .validateArgCount(expr->intrinsicName, expr->args.size())) {
        ctx.error(expr, DiagCode::E3001,
                   "wrong number of arguments for intrinsic '",
                   ctx.toString(expr->intrinsicName), "'");
        return nullptr;
    }

    // Type-check each argument
    for (ExprAST* arg : expr->args) {
        checkExpr(arg, ctx);
        // TODO: Validate argument types for specific intrinsics
    }

    // Store the LLVM intrinsic ID for codegen
    if (info->isValid()) {
        expr->intrinsicID = info->id;
    }

    // Determine the return type based on the intrinsic
    // For most intrinsics, the return type is the same as the first argument
    // or a primitive type.
    // TODO: Implement proper return type inference for each intrinsic

    // For now, return a placeholder type
    // For type/compile-time intrinsics (sizeof, alignof, etc.), return int
    return ctx.arena.makeType<PrimitiveTypeAST>(PrimitiveKind::Int);
}

// =============================================================================
// checkIndexExpr
// =============================================================================

/**
 * @brief Type-check an index expression: verify the target is an array and
 *        the index is an integer.
 *
 * @param expr The index expression.
 * @param ctx  The semantic context.
 * @return The array's element type, or nullptr on error.
 */
TypeAST* checkIndexExpr(IndexExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* targetType = checkExpr(expr->target, ctx);
    if (!targetType) return nullptr;

    TypeAST* indexType = checkExpr(expr->index, ctx);
    if (!indexType) return nullptr;

    // Verify index is an integer type
    if (!isIntegerType(indexType)) {
        ctx.error(expr->index, DiagCode::E3003,
                   "index must be an integer type");
        return nullptr;
    }

    // Verify target is an array type
    if (!targetType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                   "indexing requires an array type");
        return nullptr;
    }

    ArrayTypeAST* arrayType = targetType->as<ArrayTypeAST>();

    // Verify the index is within bounds for fixed arrays (if literal)
    // TODO: Check compile-time bounds for fixed arrays

    return arrayType->element;
}

// =============================================================================
// checkSliceExpr
// =============================================================================

/**
 * @brief Type-check a slice expression: verify the target is an array and
 *        the bounds are valid.
 *
 * @param expr The slice expression.
 * @param ctx  The semantic context.
 * @return The slice type ([_]T), or nullptr on error.
 */
TypeAST* checkSliceExpr(SliceExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* targetType = checkExpr(expr->target, ctx);
    if (!targetType) return nullptr;

    // Type-check start and end bounds if present
    if (expr->start) {
        TypeAST* startType = checkExpr(expr->start, ctx);
        if (startType && !isIntegerType(startType)) {
            ctx.error(expr->start, DiagCode::E3003,
                       "slice start must be an integer type");
        }
    }

    if (expr->end) {
        TypeAST* endType = checkExpr(expr->end, ctx);
        if (endType && !isIntegerType(endType)) {
            ctx.error(expr->end, DiagCode::E3003,
                       "slice end must be an integer type");
        }
    }

    // Verify target is an array type
    if (!targetType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                   "slicing requires an array type");
        return nullptr;
    }

    ArrayTypeAST* arrayType = targetType->as<ArrayTypeAST>();

    // A slice always produces a slice type ([_]T)
    return ctx.arena.makeType<ArrayTypeAST>(ArrayKind::Slice, 0, arrayType->element);
}

// =============================================================================
// checkFieldAccessExpr
// =============================================================================

/**
 * @brief Type-check a field access: verify the object has the field and
 *        return the field's type.
 *
 * @param expr The field access expression.
 * @param ctx  The semantic context.
 * @return The field's type, or nullptr on error.
 */
TypeAST* checkFieldAccessExpr(FieldAccessExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* objectType = checkExpr(expr->object, ctx);
    if (!objectType) return nullptr;

    // Resolve the object type to its declaration
    // For structs, look up the field
    TypeDeclAST* typeDecl = nullptr;

    if (objectType->isa<NamedTypeAST>()) {
        NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
        typeDecl = resolveTypeNameOrError(namedType, ctx);
    } else if (objectType->isa<PrimitiveTypeAST>()) {
        // Primitive types don't have fields
        // TODO: Check for enum variants (EnumName.Variant)
    }

    if (!typeDecl) {
        ctx.error(expr, DiagCode::E2002,
                   "cannot access field '", ctx.toString(expr->fieldName),
                   "' on non-struct type");
        return nullptr;
    }

    // Check if it's a struct
    if (typeDecl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        // Find the field
        for (FieldDeclAST* field : structDecl->fields) {
            if (field->name == expr->fieldName) {
                // Check if this is a module member access (read-only)
                if (expr->isModuleMember) {
                    // Module member access is always read-only
                    // TODO: Mark the result as const
                }
                return field->type;
            }
        }

        ctx.error(expr, DiagCode::E2001,
                   "struct '", ctx.toString(structDecl->name),
                   "' has no field named '", ctx.toString(expr->fieldName), "'");
        return nullptr;
    }

    // Check if it's an enum
    if (typeDecl->isa<EnumDeclAST>()) {
        EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();

        // Find the variant
        for (EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == expr->fieldName) {
                // Enum variant's type is the enum itself
                return selfTypeOf(enumDecl, ctx);
            }
        }

        ctx.error(expr, DiagCode::E2001,
                   "enum '", ctx.toString(enumDecl->name),
                   "' has no variant named '", ctx.toString(expr->fieldName), "'");
        return nullptr;
    }

    ctx.error(expr, DiagCode::E2002,
               "cannot access field on non-struct/enum type");
    return nullptr;
}

// =============================================================================
// checkModuleAccessExpr
// =============================================================================

/**
 * @brief Type-check a module access expression: resolve the module and
 *        member, returning the member's type.
 *
 * Module access is always read-only.
 *
 * @param expr The module access expression.
 * @param ctx  The semantic context.
 * @return The member's type, or nullptr on error.
 */
TypeAST* checkModuleAccessExpr(ModuleAccessExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    // Look up the module alias
    ModuleAST* module = ctx.lookupImport(expr->moduleName);
    if (!module) {
        ctx.error(expr, DiagCode::E2001,
                   "undefined module alias '", ctx.toString(expr->moduleName), "'");
        return nullptr;
    }

    // Get the module's table
    ModuleTable* table = ctx.findModuleTable(module);
    if (!table) {
        ctx.error(expr, DiagCode::E2001,
                   "module '", ctx.toString(expr->moduleName), "' has not been analyzed");
        return nullptr;
    }

    // Look up the member in the module's value namespace
    auto it = table->values.find(expr->memberName);
    if (it == table->values.end()) {
        ctx.error(expr, DiagCode::E2001,
                   "module '", ctx.toString(expr->moduleName),
                   "' has no exported member '", ctx.toString(expr->memberName), "'");
        return nullptr;
    }

    ValueDeclAST* decl = it->second;

    // Mark the access as read-only (module members are always read-only)
    expr->isModuleMember = true;

    // Check generic arguments if present
    if (!expr->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints
    }

    return decl->valueType;
}

// =============================================================================
// checkNullableChainExpr
// =============================================================================

/**
 * @brief Type-check a nullable chain expression: each step is only evaluated
 *        if the previous value is non-nil.
 *
 * The chain must be terminated by ?? (checked at the parent level).
 *
 * @param expr The nullable chain expression.
 * @param ctx  The semantic context.
 * @return The type of the final step, or nullptr on error.
 */
TypeAST* checkNullableChainExpr(NullableChainExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E3003, "empty nullable chain");
        return nullptr;
    }

    // Start with the object's type
    TypeAST* currentType = checkExpr(expr->object, ctx);
    if (!currentType) return nullptr;

    // Walk through each step
    for (InternedString step : expr->steps) {
        // The current type must be nullable
        if (!isNullableType(currentType)) {
            ctx.error(expr, DiagCode::E3003,
                       "cannot apply ?. to non-nullable type");
            return nullptr;
        }

        // Unwrap the nullable to get the inner type
        TypeAST* innerType = unwrapNullable(currentType);
        if (!innerType) return nullptr;

        // The inner type must have the field (struct) or be callable
        // TODO: Handle field access on the inner type
        // For now, just return the inner type (placeholder)
        currentType = innerType;
    }

    // The chain result is nullable (if any step is nil, the result is nil)
    return ctx.arena.makeType<NullableTypeAST>(currentType);
}

// =============================================================================
// checkNullCoalesceExpr
// =============================================================================

/**
 * @brief Type-check a null coalesce expression: the LHS must be nullable or
 *        fallible, and the RHS must be assignable to the unwrapped type.
 *
 * @param expr The null coalesce expression.
 * @param ctx  The semantic context.
 * @return The type of the RHS, or nullptr on error.
 */
TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* valueType = checkExpr(expr->value, ctx);
    if (!valueType) return nullptr;

    // The LHS must be nullable or fallible
    if (!isNullableType(valueType) && !isFallibleType(valueType)) {
        ctx.error(expr->value, DiagCode::E3003,
                   "null coalesce (??) requires nullable or fallible LHS");
        return nullptr;
    }

    TypeAST* fallbackType = checkExpr(expr->fallback, ctx);
    if (!fallbackType) return nullptr;

    // The fallback type must be assignable to the unwrapped value type
    TypeAST* unwrapped = unwrapNullable(unwrapFallible(valueType));
    if (!isAssignable(unwrapped, fallbackType, ctx)) {
        ctx.error(expr->fallback, DiagCode::E3003,
                   "fallback type mismatch for null coalesce");
        return nullptr;
    }

    // The result type is the fallback type
    return fallbackType;
}

// =============================================================================
// checkAssignExpr
// =============================================================================

/**
 * @brief Type-check an assignment: verify the LHS is an assignable lvalue
 *        and the RHS is assignable to the LHS type.
 *
 * @param expr The assignment expression.
 * @param ctx  The semantic context.
 * @return The LHS type, or nullptr on error.
 */
TypeAST* checkAssignExpr(AssignExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* lhsType = checkExpr(expr->lhs, ctx);
    if (!lhsType) return nullptr;

    TypeAST* rhsType = checkExpr(expr->rhs, ctx);
    if (!rhsType) return nullptr;

    // TODO: Verify LHS is an assignable lvalue (variable, field, index)
    // TODO: Check if LHS is const (reject assignment)
    // TODO: For compound assignments (+=, -=, etc.), verify the operator
    //       is valid on the LHS type

    if (!isAssignable(lhsType, rhsType, ctx)) {
        ctx.error(expr->rhs, DiagCode::E3003,
                   "assignment type mismatch");
        return nullptr;
    }

    return lhsType;
}

// =============================================================================
// checkPipelineExpr
// =============================================================================

/**
 * @brief Type-check a pipeline expression: the seed type flows through
 *        each step, and each step must be callable with the input type.
 *
 * @param expr The pipeline expression.
 * @param ctx  The semantic context.
 * @return The pipeline's final type, or nullptr on error.
 */
TypeAST* checkPipelineExpr(PipelineExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E1107, "pipeline has no steps");
        return nullptr;
    }

    // Start with the seed's type
    TypeAST* currentType = checkExpr(expr->seed, ctx);
    if (!currentType) return nullptr;

    // Walk through each step
    for (PipelineStepAST* step : expr->steps) {
        currentType = checkPipelineStep(step, currentType, ctx);
        if (!currentType) return nullptr;
    }

    return currentType;
}

// =============================================================================
// checkPipelineStep
// =============================================================================

/**
 * @brief Type-check a single pipeline step: verify the step is callable
 *        with the input type and return the output type.
 *
 * @param step      The pipeline step.
 * @param inputType The type flowing into this step.
 * @param ctx       The semantic context.
 * @return The step's output type, or nullptr on error.
 */
TypeAST* checkPipelineStep(PipelineStepAST* step, TypeAST* inputType, SemaContext& ctx) {
    if (!step || !inputType) return nullptr;

    // Type-check the callable
    TypeAST* callableType = checkExpr(step->callable, ctx);
    if (!callableType) return nullptr;

    // Verify the callable is a function type
    if (!callableType->isa<FuncTypeAST>()) {
        ctx.error(step->callable, DiagCode::E2003,
                   "pipeline step must be callable");
        return nullptr;
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // The input type becomes the first argument
    // If there are packArgs, they fill the remaining arguments
    size_t expectedArgs = funcType->params.size();

    // The first argument must match the input type
    if (expectedArgs == 0) {
        ctx.error(step, DiagCode::E3001,
                   "pipeline step has no parameters to accept the input");
        return nullptr;
    }

    ParamAST* firstParam = funcType->params[0];
    if (!isAssignable(firstParam->type, inputType, ctx)) {
        ctx.error(step->callable, DiagCode::E3003,
                   "pipeline input type mismatch");
        return nullptr;
    }

    // Check any additional arguments (packArgs)
    // TODO: Verify packArgs types match the remaining parameters

    // Return the function's return type
    if (funcType->returnTypes.empty()) {
        return nullptr; // void return
    }
    return funcType->returnTypes[0];
}

// =============================================================================
// checkComposeExpr
// =============================================================================

/**
 * @brief Type-check a composition expression: f +> g +> h
 *
 * The output type of each operand must match the input type of the next.
 *
 * @param expr The composition expression.
 * @param ctx  The semantic context.
 * @return The composed function type, or nullptr on error.
 */
TypeAST* checkComposeExpr(ComposeExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    if (expr->operands.empty()) {
        ctx.error(expr, DiagCode::E3003, "composition has no operands");
        return nullptr;
    }

    // Start with the left operand's type
    TypeAST* currentType = checkComposeOperand(expr->left, ctx);
    if (!currentType) return nullptr;

    // Walk through each right operand
    for (ComposeOperandAST* operand : expr->operands) {
        TypeAST* operandType = checkComposeOperand(operand, ctx);
        if (!operandType) return nullptr;

        // Verify the operand is a function type
        if (!operandType->isa<FuncTypeAST>()) {
            ctx.error(operand->callable, DiagCode::E2003,
                       "composition operand must be callable");
            return nullptr;
        }

        FuncTypeAST* funcType = operandType->as<FuncTypeAST>();

        // Verify the previous output matches this operand's input
        // The previous type must be a function whose return type matches
        // this operand's parameter type.
        if (!currentType->isa<FuncTypeAST>()) {
            ctx.error(operand->callable, DiagCode::E3003,
                       "composition type mismatch");
            return nullptr;
        }

        FuncTypeAST* prevFunc = currentType->as<FuncTypeAST>();
        if (prevFunc->returnTypes.empty()) {
            ctx.error(operand->callable, DiagCode::E3003,
                       "cannot compose void function with another function");
            return nullptr;
        }

        TypeAST* prevReturn = prevFunc->returnTypes[0];
        if (funcType->params.empty()) {
            ctx.error(operand->callable, DiagCode::E3003,
                       "right operand must take at least one parameter");
            return nullptr;
        }

        if (!isAssignable(funcType->params[0]->type, prevReturn, ctx)) {
            ctx.error(operand->callable, DiagCode::E3003,
                       "composition type mismatch: output of left doesn't match input of right");
            return nullptr;
        }

        // The composed function's type is: (left's params) -> (right's return)
        // TODO: Build the composed function type
        currentType = operandType;
    }

    return currentType;
}

// =============================================================================
// checkComposeOperand
// =============================================================================

/**
 * @brief Type-check a composition operand: resolve the callable and
 *        return its type.
 *
 * @param operand The composition operand.
 * @param ctx     The semantic context.
 * @return The operand's type, or nullptr on error.
 */
TypeAST* checkComposeOperand(ComposeOperandAST* operand, SemaContext& ctx) {
    if (!operand) return nullptr;

    TypeAST* result = checkExpr(operand->callable, ctx);
    if (!result) return nullptr;

    // Check generic arguments if present
    if (!operand->genericArgs.empty()) {
        // TODO: Check generic argument arity and constraints
    }

    return result;
}

// =============================================================================
// checkAnonFuncExpr
// =============================================================================

/**
 * @brief Type-check an anonymous function expression: resolve its type
 *        and analyze its body.
 *
 * @param expr The anonymous function expression.
 * @param ctx  The semantic context.
 * @return The function's type (FuncTypeAST), or nullptr on error.
 */
TypeAST* checkAnonFuncExpr(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    // Resolve the function type
    FuncTypeAST* funcType = expr->funcType;
    if (!funcType) {
        ctx.error(expr, DiagCode::E3003, "anonymous function has no type");
        return nullptr;
    }

    // Resolve all parameter and return types
    resolveFuncType(funcType, ctx);

    // Analyze the body inside a FuncBody semantic context
    // But first, open a scope for the parameters
    ScopedScope scope(ctx);

    // Insert all parameters into the scope
    for (ParamAST* param : funcType->params) {
        analyzeParam(param, ctx);
    }

    ScopedSemanticContext funcCtx(ctx, SemanticContext::FuncBody, expr, expr->loc);

    // Analyze the body
    bool bodyReturns = false;
    if (expr->body && expr->body->isa<BlockStmtAST>()) {
        bodyReturns = analyzeBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body) {
        bodyReturns = analyzeStmt(expr->body, ctx);
    }

    // Check that void functions don't have a return, and non-void functions
    // must return on all paths
    bool isVoid = funcType->isVoid();
    if (!isVoid && !bodyReturns) {
        ctx.error(expr, DiagCode::E3005,
                   "anonymous function is missing a return");
    }

    return funcType;
}

// =============================================================================
// checkIfExpr
// =============================================================================

/**
 * @brief Type-check an if expression: both branches must produce compatible types.
 *
 * @param expr The if expression.
 * @param ctx  The semantic context.
 * @return The common type of both branches, or nullptr on error.
 */
TypeAST* checkIfExpr(IfExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* condType = checkExpr(expr->condition, ctx);
    if (!condType) return nullptr;

    // TODO: Verify condition is bool or coercible to bool

    TypeAST* thenType = checkExpr(expr->thenBranch, ctx);
    if (!thenType) return nullptr;

    TypeAST* elseType = checkExpr(expr->elseBranch, ctx);
    if (!elseType) return nullptr;

    // Both branches must be assignable to each other (or have a common type)
    if (!typesEqual(thenType, elseType) &&
        !isAssignable(thenType, elseType, ctx) &&
        !isAssignable(elseType, thenType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                   "if expression branches have incompatible types");
        return nullptr;
    }

    // The result type is the type of the branches (they must be compatible)
    return thenType;
}

// =============================================================================
// checkRangeExpr
// =============================================================================

/**
 * @brief Type-check a range expression: verify both bounds are numeric.
 *
 * Ranges don't have a standalone type; they're only used in for loops,
 * slices, and switch cases.
 *
 * @param expr The range expression.
 * @param ctx  The semantic context.
 * @return The range's element type (numeric), or nullptr on error.
 */
TypeAST* checkRangeExpr(RangeExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;

    TypeAST* loType = checkExpr(expr->lo, ctx);
    if (!loType) return nullptr;

    TypeAST* hiType = checkExpr(expr->hi, ctx);
    if (!hiType) return nullptr;

    // Both bounds must be numeric
    if (!isNumericType(loType) || !isNumericType(hiType)) {
        ctx.error(expr, DiagCode::E3003,
                   "range bounds must be numeric");
        return nullptr;
    }

    // Both bounds must have the same type
    if (!typesEqual(loType, hiType)) {
        ctx.error(expr, DiagCode::E3003,
                   "range bounds must have the same type");
        return nullptr;
    }

    // Ranges don't have a type themselves; they're used in specific contexts.
    // Return the element type for use in for loops and slices.
    return loType;
}

} // namespace sema