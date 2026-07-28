/// @file SemaStructField.cpp
/// @brief Implementation of struct field analysis - two-pass registration and body analysis.
/// 
/// @architectural_note Two-Pass Analysis for Structs
///   Struct fields require a two-pass approach to support self-reference:
///     Phase 1: Register ALL fields (names and types) without analyzing bodies
///     Phase 2: Analyze function bodies (with self parameter available)
/// 
///   This allows:
///     - `self.bar` to resolve even if `bar` is declared after the function
///     - Recursive field types to be detected (`Node<T>` vs `*Node<T>`)
///     - Const fields to be validated before body analysis

#include "SemaStructField.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Local Helpers (anonymous namespace)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// @brief Check if a NamedTypeAST refers to the same struct instantiation.
/// 
/// Compares both the name and the generic arguments to ensure it's the
/// exact same instantiation of the struct.
/// 
/// Example:
///   - Node<T> and Node<T> → same ✅
///   - Node<T> and Node<int> → different ❌
///   - Pair<T, int> and Pair<T, int> → same ✅
///   - Pair<T, int> and Pair<int, T> → different ❌ (order matters)
bool isSameStructInstantiation(const NamedTypeAST* named,
                                const StructDeclAST* currentStruct) {
    if (!named || !currentStruct) return false;

    // ─── 1. Name must match ────────────────────────────────────────────
    if (named->name != currentStruct->name) return false;

    // ─── 2. Generic argument count must match ─────────────────────────
    if (named->genericArgs.size() != currentStruct->genericParams.size()) {
        return false;
    }

    // ─── 3. Generic arguments must be exactly the same parameters ────
    for (size_t i = 0; i < named->genericArgs.size(); ++i) {
        TypeAST* arg = named->genericArgs[i];
        const GenericParamDeclAST* param = currentStruct->genericParams[i];

        // Check if this argument is the generic parameter
        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* argNamed = arg->as<NamedTypeAST>();
            if (argNamed->name == param->name) {
                continue;  // This is the same generic parameter
            }
        }
        // If it's not the generic parameter, it's a different type
        return false;
    }

    return true;
}

/// @brief Check if a field type is a self-reference to the current struct.
/// 
/// A self-reference occurs when a field is of the same type as the struct
/// being defined. This can be:
///   - Direct: `Node<T>` (NamedTypeAST) → recursive value (deep copy)
///   - Indirect: `*Node<T>` (PtrTypeAST) → raw pointer (sealed conduit)
/// 
/// @param fieldType The field's type (already resolved).
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
/// @return SelfReferenceInfo describing the self-reference nature, or empty if none.
SelfReferenceInfo checkSelfReferenceImpl(const TypeAST* fieldType,
                                          const StructDeclAST* currentStruct,
                                          SemaContext& ctx) {
    SelfReferenceInfo result;
    result.isSelfReference = false;

    if (!fieldType || !currentStruct) return result;

    // ─── 1. Track original type for diagnostics ──────────────────────
    bool isNullable = false;
    bool isPointer = false;
    const TypeAST* innerType = fieldType;

    // ─── 2. Check if it's a nullable type (T?) ────────────────────────
    if (fieldType->isa<NullableTypeAST>()) {
        isNullable = true;
        innerType = fieldType->as<NullableTypeAST>()->inner;
    }

    // ─── 3. Check if it's a raw pointer (*T or *T?) ──────────────────
    if (innerType->isa<PtrTypeAST>()) {
        isPointer = true;
        innerType = innerType->as<PtrTypeAST>()->inner;
    }

    // ─── 4. Check if the inner type is a NamedType ────────────────────
    if (!innerType->isa<NamedTypeAST>()) {
        return result;  // Not a named type → not a self-reference
    }

    const NamedTypeAST* named = innerType->as<NamedTypeAST>();

    // ─── 5. Check if name matches current struct ──────────────────────
    if (named->name != currentStruct->name) {
        return result;  // Different name → not a self-reference
    }

    // ─── 6. Check generic arguments match ─────────────────────────────
    if (!isSameStructInstantiation(named, currentStruct)) {
        return result;  // Different generic args → not the same type
    }

    // ─── 7. It's a self-reference! ────────────────────────────────────
    result.isSelfReference = true;
    result.isPointer = isPointer;
    result.isNullable = isNullable;
    result.isNonNullable = !isNullable && !isPointer;
    result.namedType = named;

    return result;
}

/// @brief Validate a single struct field (shared validation logic).
/// 
/// This handles all field validation including:
///   - Type resolution
///   - Self-reference detection (with different semantics for NamedType vs PtrType)
///   - Const field validation
///   - Default value validation
///   - Reference type validation (Downward Flow Rule)
/// 
/// @param field The field to validate.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void validateStructFieldImpl(const FieldDeclAST* field,
                              const StructDeclAST* currentStruct,
                              SemaContext& ctx) {
    if (!field) return;

    validateAttributes(field->attributes, field, ctx);

    // ─── 1. Resolve the field's type ──────────────────────────────────
    TypeAST* fieldType = resolveType(field->type, ctx);
    const_cast<FieldDeclAST*>(field)->type = fieldType;

    if (!fieldType) {
        // Type resolution failed - skip further validation
        return;
    }

    // ─── 2. Check for self-reference ──────────────────────────────────
    SelfReferenceInfo selfInfo = checkSelfReferenceImpl(fieldType, currentStruct, ctx);

    if (selfInfo.isSelfReference) {
        if (selfInfo.isPointer) {
            // ─── Field is *Node<T> or *Node<T>? ──────────────────────
            // This is a raw pointer (sealed conduit) - pointer semantics
            // The user explicitly asked for a pointer.
            // 
            // Semantics: pointer copy, sealed conduit restrictions apply.
            // This is allowed but with all the restrictions of *T.
            // 
            // ✅ OK: Pointer breaks the infinite cycle.
            // No error - this is the explicit pointer form.

        } else if (selfInfo.isNullable) {
            // ─── Field is Node<T>? ─────────────────────────────────────
            // This is a nullable recursive value (deep copy semantics).
            // 
            // The compiler will handle this as a value type with deep copy.
            // The field is stored as a pointer internally, but with
            // value semantics (copy-on-assignment, deep copy).
            // 
            // ✅ OK: Nullability allows the cycle to terminate with nil.
            // The user can set next = nil to end the chain.

            // Emit a note to educate the user
            ctx.note(field, "recursive field '", ctx.pool().lookup(field->name),
                     "' uses value semantics (deep copy); use '",
                     ctx.pool().lookup(field->name), " = nil' to terminate the chain");

        } else {
            // ─── Field is Node<T> (non-nullable, non-pointer) ──────────
            // ❌ ERROR: Non-nullable self-reference creates infinite size!
            // 
            // The user wrote `next Node<T>` without `?` or `*`.
            // This would require an infinite chain of Node values.
            // 
            // Fix: Use `next Node<T>?` (nullable) or `next *Node<T>` (pointer)

            ctx.error(field, DiagCode::E3003,
                      "non-nullable self-reference '", ctx.pool().lookup(field->name),
                      "' would create infinite size; use '",
                      ctx.pool().lookup(field->name), "?' or '*",
                      ctx.pool().lookup(field->name), "' instead");
        }
    }

    // ─── 3. Const field validation ────────────────────────────────────
    if (field->isConst) {
        if (isNullableType(fieldType) || isFallibleType(fieldType)) {
            ctx.error(field, DiagCode::E3004,
                      "const field '", ctx.pool().lookup(field->name),
                      "' must be definite (not nullable or fallible)");
        }
    }

    // ─── 4. Check default value ───────────────────────────────────────
    if (field->defaultVal) {
        // If this is a function field, the default value is the body.
        // We'll analyze it in Phase 2.
        if (!fieldType->isa<FuncTypeAST>()) {
            if (!checkExpr(field->defaultVal, fieldType, ctx)) {
                // Error already reported by checkExpr
            }
        }
    }

    // ─── 5. Validate reference type context (Downward Flow Rule) ─────
    if (fieldType->isa<RefTypeAST>()) {
        ctx.error(field, DiagCode::E3004,
                  "reference type (&T) cannot be stored in struct field '",
                  ctx.pool().lookup(field->name), "'");
    }

    // ─── 6. Validate sealed conduit restrictions for raw pointers ────
    if (fieldType->isa<PtrTypeAST>()) {
        // Raw pointers are allowed, but with restrictions.
        // The restrictions are enforced during expression checking.
        // No additional validation needed here.
    }
}

/// @brief Check for duplicate field names within a struct.
void checkDuplicateFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        for (const FieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Registration - Phase 1
// ─────────────────────────────────────────────────────────────────────────────

void registerStructField(const FieldDeclAST* field,
                          const StructDeclAST* currentStruct,
                          SemaContext& ctx) {
    if (!field) return;

    // ─── 1. Resolve and validate the field type ──────────────────────────
    // This does NOT analyze function bodies - only validates the type
    validateStructFieldImpl(field, currentStruct, ctx);

    // ─── 2. Register the field in the struct's scope ──────────────────────
    // Fields are in the value namespace
    ctx.symbols.insertValue(field);

    // ─── 3. For function fields, register parameters (except body) ────────
    // Parameters will be registered when the body is analyzed (Phase 2)
    // We don't register them here to avoid duplicate registration
    // The function type is already resolved by resolveType in validateStructFieldImpl
}

// ─────────────────────────────────────────────────────────────────────────────
// Struct Function Body Analysis - Phase 2
// ─────────────────────────────────────────────────────────────────────────────

void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx) {
    if (!field || !field->type || !field->type->isa<FuncTypeAST>()) {
        return;
    }

    FuncTypeAST* funcType = field->type->as<FuncTypeAST>();

    // ─── 1. Check: first parameter must be self ──────────────────────────
    if (funcType->params.empty()) {
        ctx.error(field, "function field '", ctx.pool().lookup(field->name),
                  "' must have at least one parameter (self)");
        return;
    }

    ParamAST* selfParam = funcType->params[0];

    // Verify self parameter type is the struct type
    // The struct type should be available as a NamedTypeAST
    // TODO: Check that selfParam->type resolves to the current struct
    // For now, we trust the user wrote `self Foo`
    // This will be validated when the parameter is registered

    // ─── 2. Get the function body ──────────────────────────────────────────
    if (!field->defaultVal) {
        ctx.error(field, "function field '", ctx.pool().lookup(field->name),
                  "' has no body");
        return;
    }

    // ─── 3. Push a scope for the function's parameters ────────────────────
    // We need to push a new scope before registering parameters
    // and analyzing the body
    ctx.symbols.pushScope();

    // ─── 4. Register the self parameter ────────────────────────────────────
    // The self parameter is the first parameter of the function
    // It's a value in the function's scope
    analyzeParam(selfParam, ctx);

    // ─── 5. Register the rest of the parameters ───────────────────────────
    // Skip the first parameter (self) since it's already registered
    for (size_t i = 1; i < funcType->params.size(); ++i) {
        analyzeParam(funcType->params[i], ctx);
    }

    // ─── 6. Analyze the body ──────────────────────────────────────────────
    // The body can be a BlockStmtAST or an expression
    if (field->defaultVal->isa<BlockStmtAST>()) {
        analyzeBlock(field->defaultVal->as<BlockStmtAST>(), ctx);
    } else {
        // Expression body - treat as a return statement
        // TODO: Handle expression bodies
        ctx.error(field->defaultVal, DiagCode::E3003,
                  "expression bodies in struct functions not yet supported");
    }

    // ─── 7. Pop the function scope ────────────────────────────────────────
    ctx.symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Validation Helpers
// ─────────────────────────────────────────────────────────────────────────────

void validateStructField(const FieldDeclAST* field,
                          const StructDeclAST* currentStruct,
                          SemaContext& ctx) {
    validateStructFieldImpl(field, currentStruct, ctx);
}

void validateStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    checkDuplicateFieldNames(decl, ctx);

    for (const FieldDeclAST* field : decl->fields) {
        validateStructFieldImpl(field, decl, ctx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Self-Reference Detection (Public API)
// ─────────────────────────────────────────────────────────────────────────────

SelfReferenceInfo checkSelfReference(const TypeAST* fieldType,
                                      const StructDeclAST* currentStruct,
                                      SemaContext& ctx) {
    return checkSelfReferenceImpl(fieldType, currentStruct, ctx);
}

bool isRecursiveValueType(const TypeAST* fieldType,
                           const StructDeclAST* currentStruct,
                           SemaContext& ctx) {
    SelfReferenceInfo info = checkSelfReferenceImpl(fieldType, currentStruct, ctx);
    return info.isSelfReference && !info.isPointer;
}

bool isPointerSelfReference(const TypeAST* fieldType,
                             const StructDeclAST* currentStruct,
                             SemaContext& ctx) {
    SelfReferenceInfo info = checkSelfReferenceImpl(fieldType, currentStruct, ctx);
    return info.isSelfReference && info.isPointer;
}

} // namespace sema