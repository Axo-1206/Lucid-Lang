/// @file SemaDecl.cpp
/// @brief Implements Sema.hpp's "Declarations" section — registration and resolution.
/// 
/// This file is split into two phases:
///   Phase 1: register*Name() - Register names in symbol table (no type resolution)
///   Phase 2: resolve*Decl() - Resolve types, check bodies, AND evaluate consts
/// 
/// @architectural_note Two-Pass Approach
///   All names are registered first, then types are resolved. This enables
///   forward references (names can be used before they're defined).
/// 
/// @architectural_note Const Evaluation Integration
///   Const evaluation now happens DURING Phase 2 (type resolution), not as
///   a separate Phase 3. This allows const evaluation to use the fully
///   resolved type information and context.
/// 
/// @architectural_note Expression Resolution with Target Type
///   Declarations provide the target type for their initializers.
///   `resolveExprWithTarget(expr, targetType, ctx)` validates the expression
///   against the target type and stores the result on the expression node.

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/registry/AttributeRegistry.hpp"

namespace sema {

// =============================================================================
// PHASE 1: Name Registration
// =============================================================================

/// @brief Register an import declaration's name.
/// 
/// REGISTRATION:
///   - `ctx.addImportAlias(alias, module)` - registers import alias
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

    // Register the import alias using the integrated context
    ctx.addImportAlias(decl->alias, target);
}

/// @brief Register a variable declaration's name.
///
/// REGISTRATION:
///   - `ctx.insertValue(decl)` - registers in value namespace
///   - No type resolution is performed
///   - For const declarations, initializer is NOT evaluated here
void registerVarName(const VarDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check redeclaration ──────────────────────────────────────
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 2. Register the variable using the integrated context ──────
    // Just register the name. Type resolution and const evaluation
    // will happen in Phase 2 (resolveVarDecl).
    ctx.insertValue(decl);
}

/// @brief Register a function declaration's name.
///
/// REGISTRATION:
///   - `ctx.insertValue(decl)` - registers in value namespace
///   - Generic params registered BEFORE body
///   - Parameters registered in function scope
///   - Body names are registered via registerStmtNames
void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check redeclaration ───────────────────────────────────────────
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 2. Register function name ──────────────────────────────────────
    ctx.insertValue(decl);

    // ─── 3. Register generic parameters ────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // ─── 4. Register parameters ────────────────────────────────────────────
    ctx.pushScope();

    if (decl->funcType) {
        for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                registerParamName(param, ctx);
            }
        }
    }

    ctx.popScope();

    // ─── 5. Note: Body names will be registered by registerStmtNames ──────
}

/// @brief Register a parameter's name.
///
/// REGISTRATION:
///   - `ctx.insertValue(param)` - registers in value namespace
///   - Parameters shadow outer variables
void registerParamName(const ParamAST* param, SemaContext& ctx) {
    if (reportValueRedeclaration(param, ctx)) {
        return;
    }
    ctx.insertValue(param);
}

/// @brief Register a generic parameter's name.
///
/// REGISTRATION:
///   - `ctx.insertGenericParam(param)` - registers in the current
///     scope's genericParams map (transient, not module-level)
///
/// PRIORITY:
///   - Generic parameters have the HIGHEST lookup priority
///   - They shadow type names in the current scope
void registerGenericParamName(const GenericParamDeclAST* param, SemaContext& ctx) {
    if (reportGenericParamRedeclaration(param, ctx)) {
        return;
    }
    ctx.insertGenericParam(param);
}

/// @brief Register an enum declaration's name.
///
/// REGISTRATION:
///   - `ctx.insertType(decl)` - registers in type namespace
///   - Variants are registered as values in the enum's scope
void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx) {
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.insertType(decl);

    for (const EnumVariantAST* variant : decl->variants) {
        ctx.insertValue(variant);
    }
}

/// @brief Register a trait declaration's name.
///
/// REGISTRATION:
///   - `ctx.insertType(decl)` - registers in type namespace
///   - Generic params registered BEFORE fields
void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx) {
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.insertType(decl);

    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // Trait fields are requirements, not values - no registration needed
}

/// @brief Register a struct declaration's name.
///
/// REGISTRATION:
///   - `ctx.insertType(decl)` - registers in type namespace
///   - Generic params registered BEFORE fields
///   - Fields are registered in Phase 1 (registerStructFieldNames)
void registerStructName(const StructDeclAST* decl, SemaContext& ctx) {
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.insertType(decl);

    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    registerStructFieldNames(decl, ctx);
}

/// @brief Register all field names in a struct (no type resolution).
///
/// This is Phase 1 of struct analysis. It registers field names so that
/// self-reference is possible in Phase 2.
void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        ctx.insertValue(field);
    }
}

// =============================================================================
// PHASE 2: Type Resolution
// =============================================================================

/// @brief Resolve an import declaration.
///
/// VALIDATION:
///   - Module path must exist
///   - Import alias was already registered in Phase 1
void resolveImportDecl(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.error(decl, DiagCode::E2001,
                  "undefined module '", ctx.pool.lookup(decl->path), "'");
    }
}

/// @brief Resolve a variable declaration.
///
/// RESOLUTION:
///   - Resolve the declared type
///   - For const: enforce initializer
///   - For init: resolve the expression with target type validation
///   - For const: EVALUATE the initializer NOW (integrated const evaluation)
///
/// NOTE: The variable name was already registered in Phase 1.
void resolveVarDecl(const VarDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ───────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        return;
    }

    // ─── 2. Const requires initializer ──────────────────────────────
    if (decl->keyword == DeclKeyword::Const && !decl->init) {
        ctx.error(decl, DiagCode::E3002,
                  "'", ctx.pool.lookup(decl->name), "' must have an initializer");
        return;
    }

    // ─── 3. Check initializer ────────────────────────────────────────
    if (decl->init) {
        // Resolve the initializer's type using the new target-type approach
        TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            return;
        }

        // ─── 4. Check for self-reference in let initializer ───────────
        if (decl->keyword == DeclKeyword::Let) {
            checkLetSelfReference(decl->init, decl->name, ctx);
        }

        // ─── 5. CONST EVALUATION ──────────────────────────────────────
        // For const declarations, evaluate the initializer NOW.
        // This happens during type resolution, not as a separate phase.
        if (decl->keyword == DeclKeyword::Const) {
            ConstEvaluator evaluator(ctx);
            ConstantValue val = evaluator.evaluateDecl(decl);
            if (!val.isError()) {
                // Store the evaluated value on the initializer expression
                decl->init->isConst = true;
                decl->init->constValue = val;
                // Mark the declaration as const
                const_cast<VarDeclAST*>(decl)->isConst = true;
            }
        }
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
void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve the function type ─────────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // ─── 2. Check for @[foreign] attribute ────────────────────────────────
    const AttributeAST* foreignAttr = attr::findAttribute(
        decl->attributes,
        attr::kForeign(ctx)
    );

    if (foreignAttr) {
        if (decl->body) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool.lookup(decl->name),
                      "' must not have a body (implementation is external)");
        }

        if (!decl->genericParams.empty()) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool.lookup(decl->name),
                      "' cannot have generic parameters");
        }
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
                  "function '", ctx.pool.lookup(decl->name), "' has no body");
        return;
    }

    // Push function context with return requirements
    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), funcType, decl->loc);

    bool bodyReturns = false;
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
    } else if (decl->body->isa<FuncRefStmtAST>()) {
        const FuncRefStmtAST* refStmt = decl->body->as<FuncRefStmtAST>();
        // Function reference must match the function type
        TypeAST* refType = resolveExprWithTarget(refStmt->target, funcType, ctx);
        if (!refType || refType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            ctx.stack.pop();
            return;
        }
        bodyReturns = true;
    } else {
        ctx.error(decl, DiagCode::E3003,
                  "function '", ctx.pool.lookup(decl->name), "' has invalid body type");
        ctx.stack.pop();
        return;
    }

    // Verify return paths
    if (bodyReturns && !ctx.stack.returnRequirementsSatisfied()) {
        ctx.error(decl, DiagCode::E3005,
                  "function '", ctx.pool.lookup(decl->name),
                  "' has missing nested return");
    }

    ctx.stack.pop();

    // ─── 6. CONST FUNCTION EVALUATION (INTEGRATED) ────────────────────────
    // For const functions, we don't evaluate the body here.
    // Const functions are evaluated when called (at compile-time).
    // The function is marked as const, and the body will be interpreted
    // when a const function call is encountered.
    if (decl->keyword == DeclKeyword::Const) {
        const_cast<FuncDeclAST*>(decl)->isConst = true;
        // We could optionally pre-validate that the body is const-evaluable
        // But we defer this to the actual call site.
    }
}

/// @brief Resolve a function parameter.
///
/// RESOLUTION:
///   - Resolve the parameter type
///
/// NOTE: Parameter name was already registered in Phase 1.
void resolveParam(const ParamAST* param, SemaContext& ctx) {
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        // Error already reported by resolveType
        return;
    }
}

/// @brief Resolve a generic parameter declaration.
///
/// RESOLUTION:
///   - Resolve each trait constraint
///
/// NOTE: Generic parameter name was already registered in Phase 1.
void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
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

    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.error(decl, DiagCode::E2002,
                      "invalid backing type for enum '",
                      ctx.pool.lookup(decl->name), "'");
        }
    }

    for (const EnumVariantAST* variant : decl->variants) {
        attr::validateAttributes(variant, ctx);

        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.error(variant, DiagCode::E2101,
                          "redeclaration of '", ctx.pool.lookup(variant->name), "'");
                break;
            }
        }

        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->value == variant->value) {
                ctx.error(variant, DiagCode::E3006,
                          "duplicate enum value ", std::to_string(variant->value),
                          " (also used by '", ctx.pool.lookup(existing->name), "')");
                break;
            }
        }
    }
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

    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    for (const TraitFieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool.lookup(field->name), "'");
                break;
            }
        }

        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const trait field '", ctx.pool.lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }
    }

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

    ScopedTypeDefinition defining(ctx, decl);

    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    resolveStructFields(decl, ctx);

    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported
    }

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
void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Check for duplicate field names ──────────────────────────────
    for (const FieldDeclAST* field : decl->fields) {
        for (const FieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool.lookup(field->name), "'");
                break;
            }
        }
    }

    // ─── 2. Resolve each field's type ─────────────────────────────────────
    for (const FieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // ─── 2a. Resolve the field's type ──────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── 2b. Validate const field type ──────────────────────────────
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const field '", ctx.pool.lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }

        // ─── 2c. Check default value ────────────────────────────────────
        if (field->defaultVal) {
            // Function fields are handled separately
            if (!fieldType->isa<FuncTypeAST>()) {
                // Resolve default value against the field type
                TypeAST* defaultType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
                if (!defaultType || defaultType->isa<UnknownTypeAST>()) {
                    // Error already reported by resolveExprWithTarget
                    continue;
                }
            }
        }

        // ─── 2d. Validate reference type context (Downward Flow Rule) ────
        if (fieldType->isa<RefTypeAST>()) {
            ctx.error(field, DiagCode::E3004,
                      "reference type (&T) cannot be stored in struct field '",
                      ctx.pool.lookup(field->name), "'");
        }

        // ─── 2e. Analyze function field bodies ──────────────────────────
        if (fieldType->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
    }
}

} // namespace sema