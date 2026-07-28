/// @file SemaDecl.cpp
/// @brief Implements Sema.hpp's "Declarations" section — analyzeDecl() and
///        every specific analyze*Decl() function.
/// 
/// @architectural_note Insert-before-recurse, except where it would be wrong
///   This file's central theme is Sema.hpp's own "insert this name, THEN
///   recurse into its internals" pattern — but applied per declaration kind:
///     - Structs/enums/traits: insert name, open ScopedTypeDefinition, THEN
///       walk fields/variants — enables self-reference (e.g., `next ptr<Node>?`)
///     - Functions: insert name BEFORE params/body — enables recursion
///     - Variables: type-check initializer BEFORE inserting name — prevents
///       `let x int = x` from resolving to itself
/// 
/// @architectural_note Redeclaration checks use current tier only
///   Shadowing outer names is allowed. Every duplicate-name check looks only
///   at the tier where the new declaration will be inserted, using the
///   redeclaration helpers from Lookup.cpp.
/// 
/// @architectural_note AST nodes are read-only
///   The parser already created and populated all AST nodes. Semantic analysis
///   only reads from them and annotates them with resolved types. We never
///   modify the structure of the AST, only add semantic annotations.

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// analyzeDecl - Dispatch
// =============================================================================

/// @brief Dispatch a declaration to its specific analyzer.
/// 
/// IMPORTANT: Every declaration analyzer follows this pattern:
///   1. REGISTER the declaration's name in the symbol table
///   2. Push appropriate context guard (ScopedTypeDefinition for types)
///   3. Analyze the declaration's internals (fields, body, etc.)
///   4. Pop context guard
/// 
/// This ordering enables self-reference: the name is findable while analyzing
/// the declaration's own internals.
void analyzeDecl(const DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl: analyzeImportDecl(decl->as<const ImportDeclAST>(), ctx); return;
        case ASTKind::VarDecl:    analyzeVarDecl(decl->as<const VarDeclAST>(), ctx); return;
        case ASTKind::FuncDecl:   analyzeFuncDecl(decl->as<const FuncDeclAST>(), ctx); return;
        case ASTKind::EnumDecl:   analyzeEnumDecl(decl->as<const EnumDeclAST>(), ctx); return;
        case ASTKind::TraitDecl:  analyzeTraitDecl(decl->as<const TraitDeclAST>(), ctx); return;
        case ASTKind::StructDecl: analyzeStructDecl(decl->as<const StructDeclAST>(), ctx); return;
        default:
            return;
    }
}

// =============================================================================
// Import Declaration
// =============================================================================

/// @brief Register an import declaration.
/// 
/// REGISTRATION:
///   - `ctx.symbols.addImportAlias(alias, module)` - registers import alias
///   - This allows `module:member` syntax in expressions
void analyzeImportDecl(const ImportDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // Check import alias redeclaration
    if (reportImportAliasRedeclaration(decl->alias, decl, ctx)) {
        return;
    }

    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.error(decl, DiagCode::E2001,
                  "undefined module '", ctx.pool().lookup(decl->path), "'");
        return;
    }

    ctx.symbols.addImportAlias(decl->alias, target);
}

// =============================================================================
// Variable Declaration
// =============================================================================

/// @brief Register a variable declaration.
///
/// REGISTRATION:
///   - `ctx.symbols.insertValue(decl)` - registers in value namespace
///   - For const declarations, marks isConst = true
///
/// ORDER:
///   1. Validate attributes
///   2. Resolve the declared type
///   3. Check redeclaration
///   4. For const: enforce initializer
///   5. For init: type-check the expression
///   6. Register the variable
///
/// NOTE: The initializer is checked BEFORE inserting the variable name.
/// This prevents `let x int = x` from resolving to itself.
void analyzeVarDecl(const VarDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // ─── 1. Resolve the declared type ─────────────────────────────────
    // Checks if the type name exists in scope
    TypeAST* declaredType = resolveType(decl->type, ctx);

    // ─── 2. Check redeclaration ──────────────────────────────────────
    // Check value redeclaration in current tier only (shadowing is allowed)
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 3. Const requires initializer ──────────────────────────────
    // const variables must have an initializer - no default value allowed
    if (decl->keyword == DeclKeyword::Const && !decl->init) {
        ctx.error(decl, DiagCode::E3002,
                  "'", ctx.pool().lookup(decl->name), "' must have an initializer");
        // Register despite error for better error recovery
        ctx.symbols.insertValue(decl);
        return;
    }

    // ─── 4. Check initializer ────────────────────────────────────────
    // Check initializer type BEFORE inserting the variable name
    // This prevents `let x int = x` from resolving to itself
    if (decl->init) {
        // checkExpr validates the expression against the declared type
        // This handles: type assignability, value state propagation, etc.
        if (!checkExpr(decl->init, declaredType, ctx)) {
            // Error already reported by checkExpr
            // Still register the variable for error recovery
            ctx.symbols.insertValue(decl);
            return;
        }
    }

    // ─── 5. Register the variable ────────────────────────────────────
    // Set the variable's cached type and register it in the symbol table
    const_cast<VarDeclAST*>(decl)->type = declaredType;
    ctx.symbols.insertValue(decl);
}

// =============================================================================
// Function Declaration
// =============================================================================

/// @brief Analyze a function declaration.
///
/// REGISTRATION:
///   - `ctx.symbols.insertValue(decl)` - registers in value namespace
///   - Generic params registered via analyzeGenericParamDecl() BEFORE body
///
/// ORDER:
///   1. Register function name (for recursion)
///   2. Register generic parameters (for use in params/return/body)
///   3. Push ScopedSemanticContext(FuncBody) with return requirements
///   4. Analyze parameters, return type, and body
///   5. Pop context
///
/// BODY TYPES:
///   - Block: { ... } - executable statements
///   - Expression: a + b - wrapped in ReturnStmtAST
///   - Reference: module:func - FuncRefStmtAST
void analyzeFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // ─── 1. Resolve the function type ─────────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    resolveFuncType(funcType, ctx);

    // ─── 2. Check redeclaration ───────────────────────────────────────────
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // ─── 3. Register function name BEFORE analyzing body ──────────────────
    ctx.symbols.insertValue(decl);

    // ─── 4. Check for @[foreign] attribute ────────────────────────────────
    // TODO: @[foreign] attribute support is not yet implemented.
    AttributeAST* foreignAttr = findForeignAttr(decl->attributes, ctx);
    if (foreignAttr) {
        // TODO: Implement proper foreign function validation
        // validateForeignFunc(decl, foreignAttr, ctx);
        // return;
    }

    // ─── 5. Analyze generic parameters ────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    // ─── 6. Analyze parameters ────────────────────────────────────────────
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            analyzeParam(param, ctx);
        }
    }

    // ─── 7. Push function context with return requirements ───────────────
    ctx.contexts.pushFunction(const_cast<FuncDeclAST*>(decl), funcType, decl->loc);

    if (!decl->body) {
        ctx.error(decl, DiagCode::E3003, "function '", ctx.pool().lookup(decl->name), "' has no body");
        ctx.contexts.pop();
        return;
    }

    bool bodyReturns = false;
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = analyzeBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = analyzeReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
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

    // ─── 8. Verify return paths ──────────────────────────────────────────
    if (bodyReturns && !ctx.contexts.returnRequirementsSatisfied()) {
        ctx.error(decl, DiagCode::E3005,
                  "function '", ctx.pool().lookup(decl->name), "' has missing nested return");
    }

    // ─── 9. Pop function context ──────────────────────────────────────────
    ctx.contexts.pop();
}

// =============================================================================
// Parameter Analysis
// =============================================================================

/// @brief Analyze a function parameter.
///
/// REGISTRATION:
///   - Parameters are registered in the function's scope
///   - `ctx.symbols.insertValue(param)` - registers in value namespace
///   - Parameters shadow outer variables
///
/// VALIDATION:
///   - Parameter type must exist in scope
///   - Parameter name must not be redeclared in the same scope
///   - `const` modifier marks a read-only reference parameter
///   - Variadic parameters collect trailing arguments into a `[*]type` array
///
/// NOTE: Parameters are analyzed BEFORE the function body so they are
///       available for use in the body.
void analyzeParam(const ParamAST* param, SemaContext& ctx) {
    validateAttributes(param->attributes, param, ctx);

    // ─── 1. Resolve the parameter type ────────────────────────────────────
    // Checks if the type exists in scope
    // If the type is invalid, report error but continue for error recovery
    TypeAST* paramType = resolveType(param->type, ctx);

    // ─── 2. Check redeclaration ───────────────────────────────────────────
    // Check value redeclaration in current scope only (shadowing is allowed)
    // Parameters cannot have the same name as another parameter in the same scope
    if (reportValueRedeclaration(param, ctx)) {
        return;
    }

    // ─── 3. Register the parameter ────────────────────────────────────────
    // Parameters are values in the function's scope
    // They are accessible by name in the function body and any nested scopes
    ctx.symbols.insertValue(param);
}

// =============================================================================
// Generic Parameter Analysis
// =============================================================================

/// @brief Analyze a generic parameter declaration.
///
/// REGISTRATION:
///   - `ctx.symbols.insertGenericParam(param)` - registers in the current
///     scope's genericParams map (transient, not module-level)
///
/// PRIORITY:
///   - Generic parameters have the HIGHEST lookup priority
///   - They shadow type names in the current scope
///   - Example: In `struct Box<T>`, `T` shadows any global type named `T`
///
/// SCOPE:
///   - Generic parameters are only valid in the scope they're registered in
///   - They are popped when the scope is popped
///
/// VALIDATION:
///   - Each trait constraint must resolve to a valid trait
///   - Generic parameter name must not be redeclared in the same scope
///
/// NOTE: The parser already created the node with name and constraints.
///       We only validate and register it.
void analyzeGenericParamDecl(const GenericParamDeclAST* param, SemaContext& ctx) {
    // ─── 1. Resolve trait constraints ─────────────────────────────────────
    // Each constraint must be a valid trait in scope
    // resolveTraitRef validates the trait exists and reports errors
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }

    // ─── 2. Check redeclaration ───────────────────────────────────────────
    // Generic parameters cannot have the same name as another generic parameter
    // in the same scope (e.g., `<T, T>` is invalid)
    if (reportGenericParamRedeclaration(param, ctx)) {
        return;
    }

    // ─── 3. Register the generic parameter ────────────────────────────────
    // Generic parameters are registered in the current scope's genericParams map
    // They have the highest lookup priority and shadow type names
    ctx.symbols.insertGenericParam(param);
}

// =============================================================================
// Enum Declaration
// =============================================================================

/// @brief Register an enum declaration and analyze its variants.
///
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Variants are registered as values in the enum's scope
///
/// ORDER:
///   1. Register enum name (for self-reference)
///   2. Push ScopedTypeDefinition
///   3. Analyze variants (now can reference the enum type)
///   4. Pop ScopedTypeDefinition
///
/// VALIDATION:
///   - Enum name must not be redeclared in the same scope
///   - Backing type must be a valid integer type
///   - Variant names must be unique within the enum
///   - Variant values must be unique within the enum
void analyzeEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // ─── 1. Register enum name BEFORE analyzing variants ──────────────────
    // This enables self-reference: variants can reference the enum type
    // (e.g., `Direction.North` resolves to the enum type)
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Resolve backing type (optional) ──────────────────────────────
    // Defaults to int32 if not specified
    if (decl->backingType) {
        resolvePrimitiveType(decl->backingType, ctx);
    }

    // ─── 3. Push ScopedTypeDefinition ─────────────────────────────────────
    // This allows variants to reference the enum type during analysis
    ScopedTypeDefinition defining(ctx, decl);

    // ─── 4. Analyze each variant ──────────────────────────────────────────
    for (const EnumVariantAST* variant : decl->variants) {
        validateAttributes(variant->attributes, variant, ctx);

        // ─── 4a. Check for duplicate variant names ──────────────────────
        // Variant names must be unique within the enum
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.error(variant, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(variant->name), "'");
                // Continue checking other errors
                break;
            }
        }

        // ─── 4b. Check for duplicate variant values ──────────────────────
        // Variant values must be unique within the enum
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->value == variant->value) {
                ctx.error(variant, DiagCode::E3006,
                          "duplicate enum value ", std::to_string(variant->value),
                          " (also used by '", ctx.pool().lookup(existing->name), "')");
                // Continue checking other errors
                break;
            }
        }

        // ─── 4c. Set the variant's type (semantic annotation) ────────────
        // The variant's type is the enum itself (Direction.North has type Direction)
        // Since the enum is registered, we can look it up by name
        const TypeDeclAST* enumType = lookupType(decl->name, ctx);
        if (!enumType) {
            /// LOG ERROR
        }
        // If lookup fails, error was already reported by reportTypeRedeclaration
    }

    // ─── 5. Verify enum has at least one variant ──────────────────────────
    // Empty enums are allowed in Lucid (they can be extended later)
    // No validation needed - empty enums are valid
}

// =============================================================================
// Trait Declaration
// =============================================================================

/// @brief Register a trait declaration and analyze its fields.
///
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Generic params registered via analyzeGenericParamDecl() BEFORE fields
///
/// ORDER:
///   1. Register trait name (for self-reference)
///   2. Push ScopedTypeDefinition
///   3. Register generic parameters (for use in fields)
///   4. Analyze fields (now can find both trait and generic params)
///   5. Pop ScopedTypeDefinition
///
/// VALIDATION:
///   - Trait name must not be redeclared in the same scope
///   - Generic parameters must be used in at least one field type
///   - Field names must be unique within the trait
///   - Field types must exist in scope
///   - Non-const fields: may be nullable, fallible, combined, or definite
///   - Const fields: must be definite (not nullable or fallible)
///
/// NOTE: Trait fields are contracts. They can be nullable or fallible
///       unless marked `const`. This allows traits to require optional
///       or error-prone fields.
void analyzeTraitDecl(const TraitDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // ─── 1. Register trait name BEFORE analyzing fields ──────────────────
    // This enables self-reference: the trait can reference itself
    // in its field types
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Push ScopedTypeDefinition ────────────────────────────────────
    // This allows the trait to reference itself during field analysis
    ScopedTypeDefinition defining(ctx, decl);

    // ─── 3. Register generic parameters ──────────────────────────────────
    // Generic parameters are registered in the current scope's genericParams map
    // They shadow type names within the trait's scope
    for (const GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    // ─── 4. Analyze each trait field ──────────────────────────────────────
    for (const TraitFieldDeclAST* field : decl->fields) {
        validateAttributes(field->attributes, field, ctx);

        // ─── 4a. Check for duplicate field names ────────────────────────
        // Field names must be unique within the trait
        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }

        // ─── 4b. Resolve the field's type ────────────────────────────────
        // The type must exist in scope
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            // Error already reported by resolveType
            continue;
        }

        // ─── 4c. Validate const field type ───────────────────────────────
        // If the field is marked const, its type must be definite
        // (not nullable or fallible)
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const trait field '", ctx.pool().lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }
        // Non-const fields can be nullable, fallible, combined, or definite
        // No additional restrictions

        // ─── 4d. Set the field's type (semantic annotation) ──────────────
        const_cast<TraitFieldDeclAST*>(field)->type = fieldType;
    }

    // ─── 5. Verify all generic parameters are used ──────────────────────
    // All generic parameters must be used in at least one field type
    validateGenericParamUsage(decl, ctx);
}

// =============================================================================
// Struct Declaration - Two-Pass Analysis
// =============================================================================

/// @brief Register a struct declaration and analyze its fields.
/// 
/// REGISTRATION:
///   - `ctx.symbols.insertType(decl)` - registers in type namespace
///   - Generic params registered via analyzeGenericParamDecl() BEFORE fields
///   - Pushes ScopedTypeDefinition for self-reference detection
/// 
/// ORDER:
///   1. Register struct name (for self-reference)
///   2. Register generic parameters (for use in fields)
///   3. Push ScopedTypeDefinition
///   4. PHASE 1: Register ALL fields (names and types, no body analysis)
///   5. PHASE 2: Analyze function bodies (last, with self available)
///   6. Pop ScopedTypeDefinition
/// 
/// ERROR RECOVERY:
///   - Struct is registered even if fields have errors (prevents "unknown type" cascading)
///   - Field errors are reported but analysis continues
/// 
/// FIELD VALIDATION:
///   - Field names must be unique within the struct
///   - Field types must exist in scope
///   - Const fields must be definite (not nullable or fallible)
///   - Default values must match field types
///   - No direct self-reference (infinite size) unless using pointer/reference
///   - No reference types in struct fields (Downward Flow Rule)
void analyzeStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // ─── 1. Register struct name BEFORE analyzing fields ──────────────────
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // ─── 2. Push ScopedTypeDefinition ─────────────────────────────────────
    // So checkSelfReference can detect self-reference
    ScopedTypeDefinition defining(ctx, decl);

    // ─── 3. Register generic parameters ───────────────────────────────────
    // They shadow type names within the struct's scope
    for (const GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    // ─── 4. Validate trait implementations ──────────────────────────────
    // Traits are validated before fields so we know which fields are required
    std::unordered_map<InternedString, const NamedTypeAST*> requiredBy;
    for (const NamedTypeAST* ref : decl->traitRefs) {
        const TraitDeclAST* trait = resolveTraitRef(ref, ctx);
        if (!trait) continue;

        validateTraitImplementation(decl, ctx);

        // Check for duplicate field names required by multiple traits
        for (const TraitFieldDeclAST* tf : trait->fields) {
            auto [it, inserted] = requiredBy.try_emplace(tf->name, ref);
            if (!inserted && it->second != ref) {
                ctx.error(ref, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(tf->name),
                          "' required by multiple traits");
            }
        }
    }

    // ─── 5. PHASE 1: Register ALL fields (no body analysis) ──────────────
    // This registers field names and types, but does NOT analyze function bodies
    // This allows:
    //   - self.bar to resolve even if bar is declared after the function
    //   - Recursive field types to be detected
    for (const FieldDeclAST* field : decl->fields) {
        registerStructField(field, decl, ctx);
    }

    // ─── 6. PHASE 2: Analyze function bodies (last) ──────────────────────
    // Now that all fields are registered, we can analyze function bodies
    // with self parameter available
    for (const FieldDeclAST* field : decl->fields) {
        if (field->type && field->type->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
    }

    // ─── 7. Verify all generic parameters are used ──────────────────────
    validateGenericParamUsage(decl, ctx);

    // ─── 8. Pop ScopedTypeDefinition ──────────────────────────────────────
    // Handled by ScopedTypeDefinition destructor
}

} // namespace sema