/// @file SemaDecl.cpp
/// @brief Implements Sema.hpp's "Declarations" section — registration and resolution.
/// 
/// This file is split into two phases:
///   Phase 1: register*Name() - Register names in symbol table (no type resolution)
///   Phase 2: resolve*Decl() - Resolve types and check bodies
/// 
/// @architectural_note Two-Pass Approach
///   All names are registered first, then types are resolved. This enables
///   forward references (names can be used before they're defined).
/// 
/// @architectural_note Struct Two-Pass
///   Structs already used a two-pass approach internally:
///     Phase 1: Register struct name and field names
///     Phase 2: Resolve field types (enables self-reference)
///   This is now naturally aligned with the global two-pass design.

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/registry/AttributeRegistry.hpp"
#include "sema/support/TraitValidation.hpp"
#include "sema/support/GenericValidation.hpp"

namespace sema {

// =============================================================================
// PHASE 1: Name Registration
// =============================================================================

/// @brief Register an import declaration's name.
/// 
/// REGISTRATION:
///   - `ctx.symbols.addImportAlias(alias, module)` - registers import alias
///   - This allows `module:member` syntax in expressions
void registerImportName(const ImportDeclAST* decl, SemaContext& ctx) {
    // Check import alias redeclaration in the current module
    if (reportImportAliasRedeclaration(decl->alias, decl, ctx)) {
        return;
    }

    // Resolve the module path to a ModuleAST
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        // Error will be reported in Phase 2
        // Skip registration for now
        return;
    }

    // Register the import alias
    ctx.symbols.addImportAlias(decl->alias, target);
}

/// @brief Register a variable declaration's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertValue(decl)` - registers in value namespace
///   - No type resolution is performed
///   - For const declarations, initializer is NOT evaluated
void registerVarName(const VarDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check redeclaration ──────────────────────────────────────
    // Check value redeclaration in current tier only (shadowing is allowed)
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 2. Register the variable ────────────────────────────────────
    // Just register the name. Type resolution and initializer checking
    // will happen in Phase 2.
    ctx.symbols.insertValue(decl);
}

/// @brief Register a function declaration's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertValue(decl)` - registers in value namespace
///   - Generic params registered BEFORE body
///   - Parameters registered in function scope
///   - Body names are registered via registerStmtNames
void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check redeclaration ───────────────────────────────────────────
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 2. Register function name ──────────────────────────────────────
    ctx.symbols.insertValue(decl);

    // ─── 3. Register generic parameters ────────────────────────────────────
    // Generic parameters must be registered before params/body
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // ─── 4. Register parameters ────────────────────────────────────────────
    // Parameters are in the function's scope
    ctx.symbols.pushScope();

    if (decl->funcType) {
        for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                registerParamName(param, ctx);
            }
        }
    }

    ctx.symbols.popScope();

    // ─── 5. Note: Body names will be registered by registerStmtNames ──────
    // during the module-level registration pass.
}

/// @brief Register a parameter's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertValue(param)` - registers in value namespace
///   - Parameters shadow outer variables
void registerParamName(const ParamAST* param, SemaContext& ctx) {
    // ─── 1. Check redeclaration ───────────────────────────────────────────
    if (reportValueRedeclaration(param, ctx)) {
        return;
    }

    // ─── 2. Register the parameter ────────────────────────────────────────
    ctx.symbols.insertValue(param);
}

/// @brief Register a generic parameter's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertGenericParam(param)` - registers in the current
///     scope's genericParams map (transient, not module-level)
///
/// PRIORITY:
///   - Generic parameters have the HIGHEST lookup priority
///   - They shadow type names in the current scope
void registerGenericParamName(const GenericParamDeclAST* param, SemaContext& ctx) {
    // ─── 1. Check redeclaration ───────────────────────────────────────────
    if (reportGenericParamRedeclaration(param, ctx)) {
        return;
    }

    // ─── 2. Register the generic parameter ────────────────────────────────
    ctx.symbols.insertGenericParam(param);
}

/// @brief Register an enum declaration's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Variants are registered as values in the enum's scope
void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register enum name ────────────────────────────────────────────
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Register each variant as a value ──────────────────────────────
    for (const EnumVariantAST* variant : decl->variants) {
        ctx.symbols.insertValue(variant);
    }
}

/// @brief Register a trait declaration's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Generic params registered BEFORE fields
void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register trait name ───────────────────────────────────────────
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Register generic parameters ──────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // ─── 3. Trait fields are requirements, not values ────────────────────
    // No registration needed for trait fields
}

/// @brief Register a struct declaration's name.
///
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Generic params registered BEFORE fields
///   - Fields are registered in Phase 1 (registerStructFieldNames)
void registerStructName(const StructDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register struct name ──────────────────────────────────────────
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Register generic parameters ───────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // ─── 3. Phase 1 of struct two-pass: Register field names ──────────────
    registerStructFieldNames(decl, ctx);
}

/// @brief Register all field names in a struct (no type resolution).
///
/// This is Phase 1 of struct analysis. It registers field names so that
/// self-reference is possible in Phase 2.
///
/// @param decl The struct declaration.
/// @param ctx The semantic context.
void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        // Field names must be unique within the struct
        // We check this during resolution
        ctx.symbols.insertValue(field);
    }
}

// =============================================================================
// PHASE 2: Type Resolution
// =============================================================================

/// @brief Resolve an import declaration.
///
/// VALIDATION:
///   - Module path must exist
///   - Import alias must not be redeclared (checked in Phase 1)
void resolveImportDecl(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.error(decl, DiagCode::E2001,
                  "undefined module '", ctx.pool().lookup(decl->path), "'");
    }
    // Import alias was already registered in Phase 1
}

/// @brief Resolve a variable declaration.
///
/// RESOLUTION:
///   - Resolve the declared type
///   - For const: enforce initializer
///   - For init: type-check the expression
///
/// NOTE: The variable name was already registered in Phase 1.
///       For let declarations, we check that the initializer doesn't
///       reference the variable itself.
void resolveVarDecl(const VarDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ─────────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        // Error already reported by resolveType
        return;
    }

    // ─── 2. Const requires initializer ──────────────────────────────
    if (decl->keyword == DeclKeyword::Const && !decl->init) {
        ctx.error(decl, DiagCode::E3002,
                  "'", ctx.pool().lookup(decl->name), "' must have an initializer");
        return;
    }

    // ─── 3. Check initializer ────────────────────────────────────────
    if (decl->init) {
        if (!checkExpr(decl->init, declaredType, ctx)) {
            // Error already reported by checkExpr
            return;
        }

        // ─── 4. Check for self-reference in let initializer ──────────────
        // For let declarations, we need to detect `let x int = x`
        // Since the name is already registered (Phase 1), checkExpr would succeed.
        // We need to detect this case and report an error.
        if (decl->keyword == DeclKeyword::Let) {
            checkLetSelfReference(decl->init, decl->name, ctx);
        }
    }
}

/// @brief Check if a let initializer references the variable being declared.
///
/// This is necessary because the variable name is already registered in Phase 1,
/// so `let x int = x` would pass normal lookup. We need to detect this case
/// and report an error.
void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    // Walk the expression tree looking for IdentifierExprAST with the same name
    if (expr->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
        if (id->name == varName) {
            ctx.error(expr, DiagCode::E3003,
                      "variable '", ctx.pool().lookup(varName),
                      "' cannot be used in its own initializer");
        }
        return;
    }
    
    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.error(expr, DiagCode::E3003,
                          "let variable '", ctx.pool().lookup(varName),
                          "' cannot be used in its own initializer");
            }
            return;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            checkLetSelfReference(bin->left, varName, ctx);
            checkLetSelfReference(bin->right, varName, ctx);
            return;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            checkLetSelfReference(unary->operand, varName, ctx);
            return;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            checkLetSelfReference(call->callee, varName, ctx);
            for (const ExprAST* arg : call->args) {
                checkLetSelfReference(arg, varName, ctx);
            }
            return;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            checkLetSelfReference(field->object, varName, ctx);
            return;
        }
        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            checkLetSelfReference(index->target, varName, ctx);
            checkLetSelfReference(index->index, varName, ctx);
            return;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : arr->elements) {
                checkLetSelfReference(elem, varName, ctx);
            }
            return;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : st->inits) {
                checkLetSelfReference(init->value, varName, ctx);
            }
            return;
        }
        default:
            return;
    }
}

/// @brief Resolve a function declaration.
///
/// RESOLUTION:
///   - Resolve the function type
///   - Check for @[foreign] attribute
///   - Resolve generic parameters
///   - Resolve parameters
///   - Analyze body
///
/// NOTE: Function name was already registered in Phase 1.
///       Body analysis uses resolveStmt() which now has all names available.
void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve the function type ─────────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        // Error already reported by resolveFuncType
        return;
    }

    // ─── 2. Check for @[foreign] attribute ────────────────────────────────
    const AttributeAST* foreignAttr = attr::findAttribute(
        decl->attributes,
        attr::kForeign(ctx)
    );

    if (foreignAttr) {
        // Foreign functions must not have a body
        if (decl->body) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool().lookup(decl->name),
                      "' must not have a body (implementation is external)");
        }

        // Foreign functions cannot have generic parameters
        if (!decl->genericParams.empty()) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool().lookup(decl->name),
                      "' cannot have generic parameters");
        }

        // Skip further analysis - foreign functions are external
        return;
    }

    // ─── 3. Resolve generic parameters ────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 4. Resolve parameters ────────────────────────────────────────────
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── 5. Analyze body ──────────────────────────────────────────────────
    if (!decl->body) {
        ctx.error(decl, DiagCode::E3003,
                  "function '", ctx.pool().lookup(decl->name), "' has no body");
        return;
    }

    // Push function context with return requirements
    ctx.contexts.pushFunction(const_cast<FuncDeclAST*>(decl), funcType, decl->loc);

    bool bodyReturns = false;
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
    } else if (decl->body->isa<FuncRefStmtAST>()) {
        const FuncRefStmtAST* refStmt = decl->body->as<FuncRefStmtAST>();
        if (!checkExpr(refStmt->target, funcType, ctx)) {
            ctx.error(refStmt->target, DiagCode::E3003,
                      "function reference target type mismatch for '",
                      ctx.pool().lookup(decl->name), "'");
        }
        bodyReturns = true;
    } else {
        ctx.error(decl, DiagCode::E3003,
                  "function '", ctx.pool().lookup(decl->name), "' has invalid body type");
        ctx.contexts.pop();
        return;
    }

    // Verify return paths
    if (bodyReturns && !ctx.contexts.returnRequirementsSatisfied()) {
        ctx.error(decl, DiagCode::E3005,
                  "function '", ctx.pool().lookup(decl->name),
                  "' has missing nested return");
    }

    ctx.contexts.pop();
}

/// @brief Resolve a function parameter.
///
/// RESOLUTION:
///   - Resolve the parameter type
///
/// NOTE: Parameter name was already registered in Phase 1.
void resolveParam(const ParamAST* param, SemaContext& ctx) {
    // ─── 1. Resolve the parameter type ────────────────────────────────────
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        // Error already reported by resolveType
        return;
    }

    // Parameter was already registered in Phase 1
    // No additional validation needed
}

/// @brief Resolve a generic parameter declaration.
///
/// RESOLUTION:
///   - Resolve each trait constraint
///
/// NOTE: Generic parameter name was already registered in Phase 1.
void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    // ─── 1. Resolve trait constraints ─────────────────────────────────────
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }

    // Generic parameter was already registered in Phase 1
}

/// @brief Resolve an enum declaration.
///
/// RESOLUTION:
///   - Resolve backing type
///   - Check for duplicate variant names
///   - Check for duplicate variant values
///
/// NOTE: Enum name and variants were already registered in Phase 1.
void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve backing type (optional) ──────────────────────────────
    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.error(decl, DiagCode::E2002,
                      "invalid backing type for enum '",
                      ctx.pool().lookup(decl->name), "'");
        }
    }

    // ─── 2. Validate variants ─────────────────────────────────────────────
    for (const EnumVariantAST* variant : decl->variants) {
        attr::validateAttributes(variant, ctx);

        // ─── 2a. Check for duplicate variant names ──────────────────────
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.error(variant, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(variant->name), "'");
                break;
            }
        }

        // ─── 2b. Check for duplicate variant values ──────────────────────
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->value == variant->value) {
                ctx.error(variant, DiagCode::E3006,
                          "duplicate enum value ", std::to_string(variant->value),
                          " (also used by '", ctx.pool().lookup(existing->name), "')");
                break;
            }
        }
    }

    // ─── 3. Verify enum has at least one variant ──────────────────────────
    // Empty enums are allowed in Lucid
    // No validation needed
}

/// @brief Resolve a trait declaration.
///
/// RESOLUTION:
///   - Resolve generic parameters
///   - Resolve each field type
///   - Validate const field types
///   - Verify generic parameters are used
///
/// NOTE: Trait name was already registered in Phase 1.
void resolveTraitDecl(const TraitDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve generic parameters ──────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Resolve each trait field ──────────────────────────────────────
    for (const TraitFieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // ─── 2a. Check for duplicate field names ────────────────────────
        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }

        // ─── 2b. Resolve the field's type ────────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── 2c. Validate const field type ───────────────────────────────
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const trait field '", ctx.pool().lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }
    }

    // ─── 3. Verify all generic parameters are used ──────────────────────
    std::vector<const TypeAST*> types;
    for (const TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

/// @brief Resolve a struct declaration.
///
/// RESOLUTION:
///   - Resolve generic parameters
///   - Phase 2 of struct two-pass: Resolve field types
///   - Validate trait implementations
///   - Verify generic parameters are used
///
/// NOTE: Struct name and field names were already registered in Phase 1.
void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Push ScopedTypeDefinition ─────────────────────────────────────
    // Allows self-reference detection
    ScopedTypeDefinition defining(ctx, decl);

    // ─── 2. Resolve generic parameters ───────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 3. Phase 2 of struct two-pass: Resolve field types ──────────────
    // All field names were registered in Phase 1 (registerStructFieldNames)
    // Now we resolve the field types (which may reference the struct itself)
    resolveStructFields(decl, ctx);

    // ─── 4. Validate trait implementations ──────────────────────────────
    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported by validateAllTraitImplementations
        // Continue with field analysis for error recovery
    }

    // ─── 5. Verify all generic parameters are used ──────────────────────
    std::vector<const TypeAST*> types;
    for (const FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

/// @brief Resolve all field types in a struct (Phase 2 of struct two-pass).
///
/// This resolves the field types and validates each field.
/// Called after all field names are registered.
///
/// @param decl The struct declaration.
/// @param ctx The semantic context.
void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check for duplicate field names ──────────────────────────────
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

    // ─── 2. Resolve each field's type ─────────────────────────────────────
    for (const FieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // ─── 2a. Resolve the field's type ──────────────────────────────
        // Now self-reference is possible because the struct name is registered
        // and the field names are registered.
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── 2b. Validate const field type ──────────────────────────────
        // Const fields must be definite (not nullable or fallible)
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const field '", ctx.pool().lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }

        // ─── 2c. Check default value ────────────────────────────────────
        if (field->defaultVal) {
            // If this is a function field, the default value is the body.
            // We'll analyze it in Phase 2 of function fields.
            if (!fieldType->isa<FuncTypeAST>()) {
                if (!checkExpr(field->defaultVal, fieldType, ctx)) {
                    // Error already reported by checkExpr
                }
            }
        }

        // ─── 2d. Validate reference type context (Downward Flow Rule) ────
        if (fieldType->isa<RefTypeAST>()) {
            ctx.error(field, DiagCode::E3004,
                      "reference type (&T) cannot be stored in struct field '",
                      ctx.pool().lookup(field->name), "'");
        }

        // ─── 2e. Analyze function field bodies ──────────────────────────
        // Phase 2 of struct function fields - analyze the body
        if (fieldType->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
    }
}

} // namespace sema