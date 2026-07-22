/**
 * @file SemaDecl.cpp
 * @brief Implements Sema.hpp's "Declarations" section — analyzeDecl() and
 *        every specific analyze*Decl() function.
 *
 * @architectural_note Insert-before-recurse, except where it would be wrong
 *   This file's central theme is Sema.hpp's own "insert this name, THEN
 *   recurse into its internals" pattern — but applied per declaration kind:
 *     - Structs/enums/traits: insert name, open ScopedTypeDefinition, THEN
 *       walk fields/variants — enables self-reference (e.g., `next ptr<Node>?`)
 *     - Functions: insert name BEFORE params/body — enables recursion
 *     - Variables: type-check initializer BEFORE inserting name — prevents
 *       `let x int = x` from resolving to itself
 *
 * @architectural_note Redeclaration checks use current tier only
 *   Shadowing outer names is allowed. Every duplicate-name check looks only
 *   at the tier where the new declaration will be inserted, using the
 *   redeclaration helpers from Lookup.cpp.
 *
 * @architectural_note AST nodes are read-only
 *   The parser already created and populated all AST nodes. Semantic analysis
 *   only reads from them and annotates them with resolved types. We never
 *   modify the structure of the AST, only add semantic annotations.
 */

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

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

/**
 * @brief Register an import declaration.
 *
 * REGISTRATION:
 *   - `ctx.symbols.addImportAlias(alias, module)` - registers import alias
 *   - This allows `module:member` syntax in expressions
 */
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

/**
 * @brief Register a variable declaration.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertValue(decl)` - registers in value namespace
 *   - For const declarations, marks isConst = true
 */
void analyzeVarDecl(const VarDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // Resolve the declared type - checks if the type name exists in scope
    TypeAST* declaredType = resolveType(decl->type, ctx);

    // Check value redeclaration in current tier only (shadowing is allowed)
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // `const` requires an initializer - no default value allowed
    if (decl->keyword == DeclKeyword::Const && !decl->init) {
        ctx.error(decl, DiagCode::E3002,
                  "'", ctx.pool().lookup(decl->name), "' must have an initializer");
        // Register despite error for better error recovery
        ctx.symbols.insertValue(decl);
        return;
    }

    // Check initializer type BEFORE inserting the variable name
    // This prevents `let x int = x` from resolving to itself
    if (decl->init) {
        TypeAST* initType = checkExpr(decl->init, ctx);
        
        if (declaredType && initType) {
            if (!isAssignable(declaredType, initType, ctx)) {
                std::string expected = debug::typeToString(declaredType, ctx.pool());
                std::string actual = debug::typeToString(initType, ctx.pool());
                ctx.error(decl->init, DiagCode::E3003,
                          "type mismatch for '", ctx.pool().lookup(decl->name),
                          "': expected ", expected, ", got ", actual);
            }
        }
    }

    ctx.symbols.insertValue(decl);
}

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
 void analyzeFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {

 }

 /**
 * @brief Analyze a function parameter.
 *
 * REGISTRATION:
 *   - Parameters are registered in the function's scope
 *   - `ctx.symbols.insertValue(param)` - registers in value namespace
 *   - Parameters shadow outer variables
 */
void analyzeParam(const ParamAST* param, SemaContext& ctx) {
    validateAttributes(param->attributes, param, ctx);

    // Check value redeclaration in current scope only
    if (reportValueRedeclaration(param, ctx)) {
        return;
    }

    // Register in value namespace (parameters are values in the function's scope)
    ctx.symbols.insertValue(param);
}

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
void analyzeGenericParamDecl(const GenericParamDeclAST* param, SemaContext& ctx) {
    // Resolve all trait constraints - each must be a valid trait
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }

    // Check generic param redeclaration in current scope only
    if (reportGenericParamRedeclaration(param, ctx)) {
        return;
    }

    // Register in genericParams map (highest lookup priority)
    ctx.symbols.insertGenericParam(param);
}


/**
 * @brief Register an enum declaration and analyze its variants.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl)` - registers in type namespace
 *   - Variants are registered as values in the enum's scope
 */
void analyzeEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // Register enum name BEFORE analyzing variants - enables self-reference
    // for variant types (e.g., `Direction.North` resolves to enum type)
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // Resolve backing type (optional) - defaults to int32
    if (decl->backingType) {
        resolvePrimitiveType(decl->backingType, ctx);
    }

    // Push ScopedTypeDefinition so variants can reference the enum type
    ScopedTypeDefinition defining(ctx, decl);

    // Analyze each variant - variants are read-only, we don't modify them
    for (const EnumVariantAST* variant : decl->variants) {
        validateAttributes(variant->attributes, variant, ctx);

        // Check for duplicate variant names and duplicate values
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.error(variant, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(variant->name), "'");
            }
            if (existing->value == variant->value) {
                ctx.error(variant, DiagCode::E3006,
                          "duplicate enum value ", std::to_string(variant->value),
                          " (also used by '", ctx.pool().lookup(existing->name), "')");
            }
        }
    }
}

/**
 * @brief Register a trait declaration and analyze its fields.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl)` - registers in type namespace
 *   - Generic params registered via analyzeGenericParamDecl() BEFORE fields
 */
void analyzeTraitDecl(const TraitDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // Register trait name BEFORE analyzing fields - enables self-reference
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // Push ScopedTypeDefinition for self-reference detection
    ScopedTypeDefinition defining(ctx, decl);

    // Generic parameters are registered in the current scope's genericParams map
    // They shadow type names within the trait's scope
    for (const GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    // Analyze each trait field requirement
    for (const TraitFieldDeclAST* field : decl->fields) {
        validateAttributes(field->attributes, field, ctx);

        // Check for duplicate field names within this trait
        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }

        // Resolve the field's type - must exist in scope
        // Note: The AST node is stored in the arena and we can annotate it
        const_cast<TraitFieldDeclAST*>(field)->type = resolveType(field->type, ctx);

        // Trait fields must not be nullable or fallible
        if (field->type && (isNullableType(field->type) || isFallibleType(field->type))) {
            ctx.error(field, DiagCode::E3004,
                      "'", ctx.pool().lookup(field->name), "' must not be nullable or fallible");
        }
    }

    // Verify all generic parameters are used in at least one field type
    validateGenericParamUsage(decl, ctx);
}

/**
 * @brief Register a struct declaration and analyze its fields.
 *
 * REGISTRATION:
 *   - `ctx.symbols.insertType(decl)` - registers in type namespace
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
void analyzeStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    validateAttributes(decl->attributes, decl, ctx);

    // Register struct name BEFORE analyzing fields - enables self-reference
    // (e.g., `next ptr<Node<T>>?` can resolve Node while still being defined)
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // Push ScopedTypeDefinition so checkRecursiveFieldType can detect
    // direct self-reference (e.g., `value Node<T>` is illegal, infinite size)
    ScopedTypeDefinition defining(ctx, decl);

    // Generic parameters are registered in the current scope's genericParams map
    // They shadow type names within the struct's scope
    for (const GenericParamDeclAST* g : decl->genericParams) {
        analyzeGenericParamDecl(g, ctx);
    }

    // Analyze each field
    for (const FieldDeclAST* field : decl->fields) {
        validateAttributes(field->attributes, field, ctx);

        // Check for duplicate field names within this struct
        for (const FieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }

        // Check for direct self-reference (would cause infinite size)
        // This may annotate the field with resolved type info
        checkRecursiveFieldType(const_cast<FieldDeclAST*>(field), decl, ctx);

        // Const fields must not be nullable or fallible
        if (field->isConst && field->type &&
            (isNullableType(field->type) || isFallibleType(field->type))) {
            ctx.error(field, DiagCode::E3004,
                      "'", ctx.pool().lookup(field->name), "' must not be nullable or fallible");
        }

        // Check default value type matches the field's type
        if (field->defaultVal) {
            TypeAST* initType = checkExpr(field->defaultVal, ctx);
            if (field->type && initType && !isAssignable(field->type, initType, ctx)) {
                std::string expected = debug::typeToString(field->type, ctx.pool());
                std::string actual = debug::typeToString(initType, ctx.pool());
                ctx.error(field->defaultVal, DiagCode::E3003,
                          "type mismatch for field '", ctx.pool().lookup(field->name),
                          "': expected ", expected, ", got ", actual);
            }
        }
    }

    // Validate trait implementations - each trait reference must resolve
    // and the struct must implement all required fields
    std::unordered_map<InternedString, const NamedTypeAST*> requiredBy;
    for (const NamedTypeAST* ref : decl->traitRefs) {
        const TraitDeclAST* trait = resolveTraitRef(ref, ctx);
        if (!trait) continue; // resolveTraitRef already reported its own error

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

    // Verify all generic parameters are used in at least one field type
    validateGenericParamUsage(decl, ctx);
}

} // namespace sema