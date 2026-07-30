/// @file Sema.cpp
/// @brief Implements the public API for the semantic phase.
///
/// @architectural_note Two-Pass Name Resolution
///   Lucid now uses a two-pass approach:
///     Phase 1: Register ALL names (no type resolution)
///       - Walk AST and register every name in the symbol table
///       - This includes module-level and all nested scopes
///     Phase 2: Resolve ALL types and check bodies
///       - Walk AST again, now all names are available
///       - Resolve types, check bodies, perform semantic validation
///
/// @architectural_note Struct Two-Pass
///   Structs already used a two-pass approach internally:
///     Phase 1: Register struct name and field names
///     Phase 2: Resolve field types (enables self-reference)
///   This is now naturally aligned with the global two-pass design.

#include "Sema.hpp"
#include "context/SemaContext.hpp"
#include "const_eval/ConstEvaluator.hpp"

namespace sema {

// =============================================================================
// analyze - Main Entry Point
// =============================================================================

void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 1: Register ALL names (No type resolution)
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Walk each module and register every name in the symbol table.
    // This includes module-level declarations and all nested scopes.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        ctx.symbols.enterModule(module);
        registerModuleNames(module, ctx);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 2: Resolve ALL types and check bodies
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Now that all names are registered, walk each module again and resolve
    // types, check bodies, and perform semantic validation.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        ctx.symbols.enterModule(module);
        resolveModuleDecls(module, ctx);

        // Record whether this module had errors.
        module->hasErrors = diagnostic::hasErrorsInCurrentSource();

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            return;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 3: Const Evaluation
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Evaluate all const declarations at compile-time.
    evaluateConstDeclarations(modules, ctx);
}

// =============================================================================
// PHASE 1: Name Registration
// =============================================================================

void registerModuleNames(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;
        registerDeclName(decl, ctx);

        if (!ctx.canContinue()) {
            return;
        }
    }
}

void registerDeclName(const DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl: registerImportName(decl->as<ImportDeclAST>(), ctx); return;
        case ASTKind::VarDecl:    registerVarName(decl->as<VarDeclAST>(), ctx); return;
        case ASTKind::FuncDecl:   registerFuncName(decl->as<FuncDeclAST>(), ctx); return;
        case ASTKind::EnumDecl:   registerEnumName(decl->as<EnumDeclAST>(), ctx); return;
        case ASTKind::TraitDecl:  registerTraitName(decl->as<TraitDeclAST>(), ctx); return;
        case ASTKind::StructDecl: registerStructName(decl->as<StructDeclAST>(), ctx); return;
        default: return;
    }
}

// ─── Import Name Registration ────────────────────────────────────────────

void registerImportName(const ImportDeclAST* decl, SemaContext& ctx) {
    if (reportImportAliasRedeclaration(decl->alias, decl, ctx)) {
        return;
    }

    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        // Error will be reported in Phase 2
        // Just skip registration for now
        return;
    }

    ctx.symbols.addImportAlias(decl->alias, target);
}

// ─── Variable Name Registration ──────────────────────────────────────────

void registerVarName(const VarDeclAST* decl, SemaContext& ctx) {
    // Check redeclaration
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // Register the name (no type resolution yet)
    ctx.symbols.insertValue(decl);
}

// ─── Function Name Registration ──────────────────────────────────────────

void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx) {
    // Check redeclaration
    if (reportValueRedeclaration(decl, ctx)) {
        return;
    }

    // Register the name (no type resolution yet)
    ctx.symbols.insertValue(decl);

    // Register generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // Register parameter names (for nested scopes)
    // Parameters are in the function's scope
    ctx.symbols.pushScope();

    // Walk the function type to register parameter names
    if (decl->funcType) {
        for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                registerParamName(param, ctx);
            }
        }
    }

    ctx.symbols.popScope();

    // Note: Function body names will be registered when registerStmtNames is called
    // during the module-level registration pass.
}

// ─── Parameter Name Registration ────────────────────────────────────────

void registerParamName(const ParamAST* param, SemaContext& ctx) {
    if (reportValueRedeclaration(param, ctx)) {
        return;
    }
    ctx.symbols.insertValue(param);
}

// ─── Generic Parameter Name Registration ────────────────────────────────

void registerGenericParamName(const GenericParamDeclAST* param, SemaContext& ctx) {
    if (reportGenericParamRedeclaration(param, ctx)) {
        return;
    }
    ctx.symbols.insertGenericParam(param);
}

// ─── Struct Name Registration ────────────────────────────────────────────

void registerStructName(const StructDeclAST* decl, SemaContext& ctx) {
    // Check redeclaration
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }

    // Register the struct name
    ctx.symbols.insertType(decl);

    // Register generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // ─── Phase 1 of struct two-pass: Register field names ────────────────
    registerStructFieldNames(decl, ctx);
}

void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    // Fields are registered in the struct's scope
    // But structs don't have a scope in the symbol table (they're not block-scoped)
    // Fields are registered directly in the value namespace of the current module/scope
    for (const FieldDeclAST* field : decl->fields) {
        // Check field name uniqueness within the struct
        // This is handled during resolution
        ctx.symbols.insertValue(field);
    }
}

// ─── Enum Name Registration ──────────────────────────────────────────────

void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx) {
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // Register enum variants as values
    for (const EnumVariantAST* variant : decl->variants) {
        ctx.symbols.insertValue(variant);
    }
}

// ─── Trait Name Registration ─────────────────────────────────────────────

void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx) {
    if (reportTypeRedeclaration(decl, ctx)) {
        return;
    }
    ctx.symbols.insertType(decl);

    // Register generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        registerGenericParamName(g, ctx);
    }

    // Trait fields are not values - they're requirements
    // No registration needed
}

// ─── Statement Name Registration ────────────────────────────────────────

void registerStmtNames(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            const BlockStmtAST* block = stmt->as<BlockStmtAST>();
            ctx.symbols.pushScope();
            for (const StmtPtr s : block->stmts) {
                registerStmtNames(s, ctx);
            }
            ctx.symbols.popScope();
            return;
        }
        case ASTKind::DeclStmt: {
            const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            registerDeclName(declStmt->decl, ctx);
            return;
        }
        case ASTKind::ForStmt: {
            const ForStmtAST* forStmt = stmt->as<ForStmtAST>();
            ctx.symbols.pushScope();
            if (forStmt->indexVar) {
                registerParamName(forStmt->indexVar, ctx);
            }
            if (forStmt->valueVar) {
                registerParamName(forStmt->valueVar, ctx);
            }
            registerStmtNames(forStmt->body, ctx);
            ctx.symbols.popScope();
            return;
        }
        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            registerStmtNames(ifStmt->thenBranch, ctx);
            if (ifStmt->elseBranch) {
                registerStmtNames(ifStmt->elseBranch, ctx);
            }
            return;
        }
        case ASTKind::WhileStmt: {
            const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            registerStmtNames(whileStmt->body, ctx);
            return;
        }
        case ASTKind::DoWhileStmt: {
            const DoWhileStmtAST* doWhileStmt = stmt->as<DoWhileStmtAST>();
            registerStmtNames(doWhileStmt->body, ctx);
            return;
        }
        case ASTKind::SwitchStmt: {
            const SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
            for (const SwitchCasePtr caseStmt : switchStmt->cases) {
                registerStmtNames(caseStmt->body, ctx);
            }
            if (switchStmt->defaultBody) {
                registerStmtNames(switchStmt->defaultBody, ctx);
            }
            return;
        }
        default:
            // Other statements don't introduce names (ReturnStmt, BreakStmt, etc.)
            return;
    }
}

// =============================================================================
// PHASE 2: Type Resolution
// =============================================================================

void resolveModuleDecls(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;
        resolveDecl(decl, ctx);

        if (!ctx.canContinue()) {
            return;
        }
    }
}

void resolveDecl(const DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl: resolveImportDecl(decl->as<ImportDeclAST>(), ctx); return;
        case ASTKind::VarDecl:    resolveVarDecl(decl->as<VarDeclAST>(), ctx); return;
        case ASTKind::FuncDecl:   resolveFuncDecl(decl->as<FuncDeclAST>(), ctx); return;
        case ASTKind::EnumDecl:   resolveEnumDecl(decl->as<EnumDeclAST>(), ctx); return;
        case ASTKind::TraitDecl:  resolveTraitDecl(decl->as<TraitDeclAST>(), ctx); return;
        case ASTKind::StructDecl: resolveStructDecl(decl->as<StructDeclAST>(), ctx); return;
        default: return;
    }
}

// ─── Import Declaration Resolution ──────────────────────────────────────

void resolveImportDecl(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.error(decl, DiagCode::E2001,
                  "undefined module '", ctx.pool().lookup(decl->path), "'");
    }
    // Note: Import alias was already registered in Phase 1
}

// ─── Variable Declaration Resolution ────────────────────────────────────

void resolveVarDecl(const VarDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ──────────────────────────────────
    // Now all names are registered, so this lookup will succeed.
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        // Error already reported by resolveType
        return;
    }

    // ─── 2. Const requires initializer ──────────────────────────────────
    if (decl->keyword == DeclKeyword::Const && !decl->init) {
        ctx.error(decl, DiagCode::E3002,
                  "'", ctx.pool().lookup(decl->name), "' must have an initializer");
        return;
    }

    // ─── 3. Check initializer ───────────────────────────────────────────
    if (decl->init) {
        if (!checkExpr(decl->init, declaredType, ctx)) {
            // Error already reported by checkExpr
            return;
        }
    }
}

// ─── Function Declaration Resolution ────────────────────────────────────

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
        if (decl->body) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool().lookup(decl->name),
                      "' must not have a body (implementation is external)");
        }

        if (!decl->genericParams.empty()) {
            ctx.error(decl, DiagCode::E3003,
                      "@[foreign] function '", ctx.pool().lookup(decl->name),
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
                  "function '", ctx.pool().lookup(decl->name), "' has no body");
        return;
    }

    // Push function context with return requirements
    ctx.contexts.pushFunction(const_cast<FuncDeclAST*>(decl), funcType, decl->loc);

    bool bodyReturns = resolveStmt(decl->body, ctx);

    // Verify return paths
    if (bodyReturns && !ctx.contexts.returnRequirementsSatisfied()) {
        ctx.error(decl, DiagCode::E3005,
                  "function '", ctx.pool().lookup(decl->name),
                  "' has missing nested return");
    }

    ctx.contexts.pop();
}

// ─── Parameter Resolution ───────────────────────────────────────────────

void resolveParam(const ParamAST* param, SemaContext& ctx) {
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        // Error already reported by resolveType
        return;
    }
    // Parameter was already registered in Phase 1
    // No additional registration needed
}

// ─── Generic Parameter Resolution ──────────────────────────────────────

void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    // Resolve trait constraints
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
    // Generic parameter was already registered in Phase 1
}

// ─── Enum Declaration Resolution ────────────────────────────────────────

void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // Resolve backing type (optional)
    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.error(decl, DiagCode::E2002,
                      "invalid backing type for enum '",
                      ctx.pool().lookup(decl->name), "'");
        }
    }

    // Check for duplicate variant names and values
    for (const EnumVariantAST* variant : decl->variants) {
        attr::validateAttributes(variant, ctx);

        // Check for duplicate variant names
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->name == variant->name) {
                ctx.error(variant, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(variant->name), "'");
                break;
            }
        }

        // Check for duplicate variant values
        for (const EnumVariantAST* existing : decl->variants) {
            if (existing == variant) break;
            if (existing->value == variant->value) {
                ctx.error(variant, DiagCode::E3006,
                          "duplicate enum value ", std::to_string(variant->value),
                          " (also used by '", ctx.pool().lookup(existing->name), "')");
                break;
            }
        }

        // Verify the enum type exists (for the variant's type)
        const TypeDeclAST* enumType = lookupType(decl->name, ctx);
        if (!enumType) {
            // Error was already reported by reportTypeRedeclaration
        }
    }
}

// ─── Trait Declaration Resolution ──────────────────────────────────────

void resolveTraitDecl(const TraitDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // Resolve generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // Resolve each trait field
    for (const TraitFieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // Check for duplicate field names
        for (const TraitFieldDeclAST* existing : decl->fields) {
            if (existing == field) break;
            if (existing->name == field->name) {
                ctx.error(field, DiagCode::E2101,
                          "redeclaration of '", ctx.pool().lookup(field->name), "'");
                break;
            }
        }

        // Resolve the field's type
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // Validate const field type
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const trait field '", ctx.pool().lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }
    }

    // Verify all generic parameters are used
    std::vector<const TypeAST*> types;
    for (const TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── Struct Declaration Resolution ──────────────────────────────────────

void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // Push ScopedTypeDefinition for self-reference detection
    ScopedTypeDefinition defining(ctx, decl);

    // Resolve generic parameters
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── Phase 2 of struct two-pass: Resolve field types ──────────────────
    // All field names were registered in Phase 1 (registerStructFieldNames)
    // Now we resolve the field types (which may reference the struct itself)
    resolveStructFields(decl, ctx);

    // Validate trait implementations (uses the resolved field types)
    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported
    }

    // Verify all generic parameters are used
    std::vector<const TypeAST*> types;
    for (const FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    // Resolve each field's type
    for (const FieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // Resolve the field's type (now self-reference is possible)
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // Check for self-reference (already handled in resolveType)
        // But we need to validate const field type
        if (field->isConst) {
            if (isNullableType(fieldType) || isFallibleType(fieldType)) {
                ctx.error(field, DiagCode::E3004,
                          "const field '", ctx.pool().lookup(field->name),
                          "' must be definite (not nullable or fallible)");
                continue;
            }
        }

        // Check default value
        if (field->defaultVal) {
            if (!fieldType->isa<FuncTypeAST>()) {
                if (!checkExpr(field->defaultVal, fieldType, ctx)) {
                    // Error already reported by checkExpr
                }
            }
        }

        // Validate reference type context (Downward Flow Rule)
        if (fieldType->isa<RefTypeAST>()) {
            ctx.error(field, DiagCode::E3004,
                      "reference type (&T) cannot be stored in struct field '",
                      ctx.pool().lookup(field->name), "'");
        }

        // Analyze function field bodies (Phase 2 of struct function fields)
        if (fieldType->isa<FuncTypeAST>()) {
            analyzeFunctionFieldBody(field, decl, ctx);
        }
    }
}

// =============================================================================
// PHASE 2: Statement Resolution
// =============================================================================

bool resolveStmt(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // Dispatch to specific resolver functions
    switch (stmt->kind) {
        case ASTKind::BlockStmt:        return resolveBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::IfStmt:           return resolveIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return resolveSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::SwitchCase:       return resolveSwitchCase(stmt->as<SwitchCaseAST>(), ctx);
        case ASTKind::ForStmt:          return resolveForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return resolveWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return resolveDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return resolveReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return resolveBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return resolveContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return resolveExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return resolveDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::AsyncExpr:        return resolveAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitExpr:        return resolveAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnExpr:        return resolveSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinExpr:         return resolveJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            // Unknown/error-recovery statement
            return false;
    }
}

// =============================================================================
// LEGACY Compatibility Functions (DEPRECATED)
// =============================================================================

void analyzeDecl(const DeclAST* decl, SemaContext& ctx) {
    // Forward to resolveDecl (Phase 2)
    // Note: This assumes names are already registered
    resolveDecl(decl, ctx);
}

void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx) {
    // Forward to resolveModuleDecls (Phase 2)
    // Note: This assumes names are already registered
    resolveModuleDecls(module, ctx);
}

bool analyzeStmt(const StmtAST* stmt, SemaContext& ctx) {
    // Forward to resolveStmt (Phase 2)
    // Note: This assumes names are already registered
    return resolveStmt(stmt, ctx);
}

bool analyzeBlock(const BlockStmtAST* block, SemaContext& ctx) {
    // Forward to resolveBlock (Phase 2)
    // Note: This assumes names are already registered
    return resolveBlock(block, ctx);
}

// =============================================================================
// CONST EVALUATION
// =============================================================================

void evaluateConstDeclarations(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    ConstEvaluator evaluator(ctx);
    evaluator.evaluateAll();

    if (diagnostic::hasErrors()) {
        // Errors were reported by the evaluator
        return;
    }
}

} // namespace sema