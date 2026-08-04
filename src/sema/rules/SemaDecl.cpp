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
    if (ctx.reportImportAliasRedeclaration(decl->alias, decl)) {
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
    if (ctx.reportValueRedeclaration(decl)) {
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
    if (ctx.reportValueRedeclaration(decl)) {
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
    if (ctx.reportValueRedeclaration(param)) {
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
    if (ctx.reportGenericParamRedeclaration(param)) {
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
    if (ctx.reportTypeRedeclaration(decl)) {
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
    if (ctx.reportTypeRedeclaration(decl)) {
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
    if (ctx.reportTypeRedeclaration(decl)) {
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
        ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, decl,
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

    // ─── 2. Validate const type and initializer ──────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
            return;
        }
        if (!validateConstInitializer(decl->init != nullptr, decl->name, "variable", ctx)) {
            return;
        }
    }

    // ─── 3. Check initializer ────────────────────────────────────────
    if (decl->init) {
        TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            return;
        }

        if (decl->keyword == DeclKeyword::Let) {
            checkLetSelfReference(decl->init, decl->name, ctx);
        }

        // ─── 4. CONST EVALUATION ──────────────────────────────────────
        if (decl->keyword == DeclKeyword::Const) {
            ConstantValue val = ConstEvaluator::evaluateDecl(ctx, decl);
            if (!val.isError()) {
                const_cast<ExprAST*>(decl->init)->isConst = true;
                const_cast<ExprAST*>(decl->init)->constValue = val;
            }
        }
    }

    // ─── 5. REGISTER the variable in the current scope ────────────────
    if (!ctx.isAtModuleLevel()) {
        ctx.insertValue(decl);
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

    // 1. Resolve function type
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // 2. Check for @[foreign] attribute
    const AttributeAST* foreignAttr = attr::findAttribute(
        decl->attributes,
        attr::kForeign(ctx)
    );

    if (foreignAttr) {
        // ... foreign function validation ...
        const_cast<FuncDeclAST*>(decl)->isConst = false;
        return;
    }

    // 3. Resolve generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 4. REGISTER the function in the current scope ────────────────────
    // For nested functions, this registers them in the enclosing block scope.
    // For top-level functions, this is a no-op (already registered in Phase 1).
    if (!ctx.isAtModuleLevel()) {
        ctx.insertValue(decl);
    }

    // 5. Resolve parameters - REGISTER them in the function scope
    ctx.pushScope();  // Create scope for parameters
    
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);  // This registers the parameter name
        }
    }

    // 6. Analyze body - registers local names as it goes
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        ctx.popScope();  // Clean up parameter scope
        return;
    }

    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), funcType, decl->loc);

    bool bodyReturns = false;
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
    } else if (decl->body->isa<FuncRefStmtAST>()) {
        const FuncRefStmtAST* refStmt = decl->body->as<FuncRefStmtAST>();
        TypeAST* refType = resolveExprWithTarget(refStmt->target, funcType, ctx);
        if (!refType || refType->isa<UnknownTypeAST>()) {
            ctx.stack.pop();
            ctx.popScope();
            return;
        }
        bodyReturns = true;
    } else {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, decl,
                              "function '", ctx.pool.lookup(decl->name), 
                              "' has invalid body type");
        ctx.stack.pop();
        ctx.popScope();
        return;
    }

    // Verify return paths
    if (bodyReturns && !ctx.stack.returnRequirementsSatisfied()) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' has missing nested return");
    }

    ctx.stack.pop();
    ctx.popScope();  // Clean up parameter scope

    // 7. Mark as const if applicable
    if (decl->keyword == DeclKeyword::Const) {
        const_cast<FuncDeclAST*>(decl)->isConst = true;
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
    
    // ─── Validate const parameter ──────────────────────────────────────
    if (param->isConst) {
        // Const parameters must have definite types
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
    // Register the parameter name in the current scope
    ctx.insertValue(param);
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

    // ─── 1. REGISTER the enum in the current scope if nested ──────────────
    if (!ctx.isAtModuleLevel()) {
        ctx.insertType(decl);
    }

    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, decl,
                                  "invalid backing type for enum '",
                                  ctx.pool.lookup(decl->name), "'");
        }
    }

    for (const EnumVariantAST* variant : decl->variants) {
        attr::validateAttributes(variant, ctx);

        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.diagnostics.error(DiagCode::Sem_Redeclaration, variant,
                                      "redeclaration of '", ctx.pool.lookup(variant->name), "'");
                break;
            }
        }

        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->value == variant->value) {
                ctx.diagnostics.error(DiagCode::Sem_DuplicateValue, variant,
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

    // ─── 1. REGISTER the trait in the current scope if nested ────────────
    if (!ctx.isAtModuleLevel()) {
        ctx.insertType(decl);
    }

    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    for (const TraitFieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.diagnostics.error(DiagCode::Sem_Redeclaration, field,
                                      "redeclaration of '", ctx.pool.lookup(field->name), "'");
                break;
            }
        }

        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── Validate const trait field ──────────────────────────────────
        if (field->isConst) {
            if (!validateConstType(fieldType, field->name, "trait field", ctx)) {
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

    // ─── 1. REGISTER the struct in the current scope if nested ────────────
    if (!ctx.isAtModuleLevel()) {
        ctx.insertType(decl);
    }

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
                ctx.diagnostics.error(DiagCode::Sem_Redeclaration, field,
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

        // ─── 2b. Validate self-reference ────────────────────────────────
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── 2c. Validate const field type ──────────────────────────────
        if (field->isConst) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── 2d. Check default value ────────────────────────────────────
        if (field->defaultVal) {
            if (!fieldType->isa<FuncTypeAST>()) {
                TypeAST* defaultType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
                if (!defaultType || defaultType->isa<UnknownTypeAST>()) {
                    continue;
                }
            }
        }

        // ─── 2e. Validate reference type context (Downward Flow Rule) ────
        if (fieldType->isa<RefTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "reference type (&T) cannot be stored in struct field '",
                                  ctx.pool.lookup(field->name), "'");
        }

        // ─── 2f. Analyze function field bodies ──────────────────────────
        if (fieldType->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
    }
}

} // namespace sema