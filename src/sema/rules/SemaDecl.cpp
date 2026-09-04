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
/// @architectural_note Registration Rules
///   - Top-level declarations: Registered in Phase 1 (register*Name)
///   - Nested declarations: Registered in Phase 2 (resolveDecl)
///   - Parameters: Registered ONLY in Phase 2 (resolveParam) 
///     (Parameters are not needed for Phase 1 name resolution)
///   - Generic params: Registered in Phase 1 (registerFuncName) and Phase 2 (resolveGenericParam)

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ASTStrings.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../support/MangledName.hpp"
#include "../registry/AttributeValidator.hpp"
#include "sema/types/SemaType.hpp"


namespace sema {

// =============================================================================
// PHASE 1: Name Registration
// =============================================================================
//
// Phase 1 registers all names that need to be visible for forward references.
// Only top-level declarations and their generic parameters need to be registered
// here. Parameters and local variables are registered in Phase 2.
//
// IMPORTANT: Parameters are NOT registered in Phase 1 because:
//   1. They are only used inside the function body (resolved in Phase 2)
//   2. They don't need to be visible for forward references
//   3. They are scoped to the function and resolved when the body is processed

void registerImportName(ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) return;  // Error will be reported in Phase 2
    ctx.addImportAlias(decl->alias, target, decl);
}

void registerVarName(VarDeclAST* decl, SemaContext& ctx) {
    ctx.insertValue(decl);
}

void registerFuncName(FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register the function itself ──────────────────────────────────────
    ctx.insertValue(decl);

    // ─── 2. Generic parameters are NOT registered in Phase 1 ──────────────────
    // Generic parameters are only valid inside the function body, which is
    // resolved in Phase 2. They are registered when resolveFuncDecl is called.
    // 
    // If we register them at module level here, they'll leak into other
    // functions and cause name conflicts in nested functions.

    // ─── 3. Parameters are NOT registered in Phase 1 ─────────────────────────
    // Parameters are only needed inside the function body, which is resolved
    // in Phase 2. They are registered when resolveFuncDecl calls resolveParam.
}

void registerEnumName(EnumDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (EnumVariantAST* variant : decl->variants) {
        if (!variant->name.isEmpty()) {
            ctx.insertValue(variant);
        }
    }
}

void registerTraitName(TraitDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
}

void registerStructName(StructDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    /// NOTE: Do not ignore this comment
    /// ─── REMOVED: Field registration at module level ──────────────────────
    // Fields are NOT registered in the global symbol table.
    // They are accessed through self.field or instance.field.
    // Field lookup is done via the struct's field list, not the symbol table.
}

// =============================================================================
// PHASE 2: Declaration Resolution
// =============================================================================

void resolveImportDecl(ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, decl,
                              "undefined module '", ctx.pool.lookup(decl->path), "'");
    }
}

// ─── resolveVarDecl ──────────────────────────────────────────────────────────

void resolveVarDecl(VarDeclAST* decl, SemaContext& ctx) {
    validateAllAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ──────────────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        return;
    }

    // ─── 2. Validate const type ────────────────────────────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
            return;
        }
    }

    // ─── 3. Arena type special validation ──────────────────────────────────
    if (isArenaType(declaredType)) {
        // ─── 3a. Arena bindings must be declared with `const` ──────────────
        if (decl->keyword == DeclKeyword::Let) {
            ctx.diagnostics.error(DiagCode::Sem_ConstRequired, decl,
                                "Arena bindings must be declared with `const`");
            ctx.diagnostics.note(decl,
                                "Reassigning an Arena binding would orphan slices "
                                "into its backing region");
            return;
        }
        
        // ─── 3b. Arena cannot be declared at module level ───────────────────
        if (ctx.isAtModuleLevel()) {
            ctx.diagnostics.error(DiagCode::Sem_BuiltinTypeMisuse, decl,
                                "Arena cannot be declared at top level");
            ctx.diagnostics.note(decl,
                                "Arena is scope-confined and should be declared inside "
                                "a function or block where it will be properly scoped");
            return;
        }
        
        // ─── 3c. Initializer must exist ─────────────────────────────────────
        if (!decl->init) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                "Arena binding must be initialized with "
                                "Arena::create(size) or Arena::empty()");
            return;
        }
        
        // ─── 3d. Validate the initializer ───────────────────────────────────
        // The only valid initializers for an Arena binding are:
        //   - Arena::create(size)  (fallible)
        //   - Arena::empty()       (non-fallible)
        // 
        // This rejects:
        //   - const b Arena = a;               (copy of existing arena)
        //   - const b Arena = someFunction();  (any other expression)
        //   - const b Arena = Arena{};         already rejected by type system
        if (!validateArenaInitializer(decl->init, ctx)) {
            return;
        }
    }

    // ─── 4. Check initializer ──────────────────────────────────────────────
    if (decl->init) {
        TypeAST* initType = resolveExprWithTarget(decl->init, declaredType, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            return;
        }

        if (decl->keyword == DeclKeyword::Let) {
            checkLetSelfReference(decl->init, decl->name, ctx);
        }

        // ─── 5. CONST EVALUATION ──────────────────────────────────────────
        if (decl->keyword == DeclKeyword::Const) {
            ConstantValue val = ConstEvaluator::evaluateDecl(ctx, decl);
            if (!val.isError()) {
                decl->init->isConst = true;
            }
        }
    } else {
        // ─── NO INITIALIZER: Set default value state ──────────────────────
        // (Only for nullable/fallible/combined types - Arena is handled above)
        UnknownExprAST* defaultExpr = ctx.arena.make<UnknownExprAST>();
        defaultExpr->resolvedType = declaredType;
        defaultExpr->isLValue = false;
        defaultExpr->isConst = true;
        
        if (isNullableType(declaredType)) {
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (nullable type)");
            return;
        }
        
        if (isFallibleType(declaredType)) {
            defaultExpr->valueState = ValueState::Err;
            decl->init = defaultExpr;
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to err (fallible type)");
            return;
        }
        
        if (declaredType->isa<CombinedTypeAST>()) {
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            Trace::detail("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (combined type)");
            return;
        }
        
        if (declaredType->isa<FuncTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                  "function type variable '", ctx.pool.lookup(decl->name),
                                  "' must be initialized with a function value");
            return;
        }
        
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "variable '", ctx.pool.lookup(decl->name),
                              "' of type '", typeToString(declaredType, ctx.pool),
                              "' must be initialized (type is not nullable or fallible)");
        ctx.diagnostics.note(decl, 
                             "Consider using a nullable type (T?) or fallible type (T!) ",
                             "if you want default initialization to nil/err, or provide an initializer");
        return;
    }

    // ─── 6. Generate mangled name for exported globals ───────────────────
    bool isModuleLevel = isModuleLevelDeclaration(decl, ctx);
    bool isExported = false;
    InternedString exportName = ctx.pool.intern("export");
    for (const AttributeAST* attr : decl->attributes) {
        if (attr->name == exportName) {
            isExported = true;
            break;
        }
    }
    
    if (isModuleLevel && isExported) {
        InternedString mangled = generateMangledName(decl, ctx);
        if (mangled.isValid()) {
            decl->mangledName = mangled;
        }
    }
}

// ─── resolveFuncDecl ──────────────────────────────────────────────────────────

void resolveFuncDecl(FuncDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        if (decl->type) {
            decl->type = ctx.getUnknownType();
        }
        return;
    }

    // 1. Validate attributes
    validateAllAttributes(decl, ctx);

    // 2. Check @[foreign]
    InternedString foreignName = ctx.pool.intern("foreign");
    for (AttributeAST* attr : decl->attributes) {
        if (attr->name == foreignName) {
            decl->isForeignFunction = true;
        }
    }

    // 3. Resolve the function type (nested FuncTypeAST)
    FuncTypeAST* funcType = decl->funcType;
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // 4. Foreign functions – use original name as symbol
    if (decl->isForeignFunction) {
        decl->mangledName = decl->name;
        Trace::info("Foreign function '", ctx.pool.lookup(decl->name),
                 "' uses symbol name: ", ctx.pool.lookup(decl->mangledName));
        return;
    }

    // 5. Resolve generic parameters (if any)
    // ─── Generic parameters are resolved in the OUTER scope ──────────────
    // They should NOT be in the function's parameter scope because they're
    // visible throughout the function body, not just as parameters.
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 6. Generate mangled name BEFORE pushing scopes ────────────────────
    // Mangling doesn't depend on scopes, so do it early.
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }

    // 7. Foreign functions have no body – skip body resolution
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingFuncBody, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        return;
    }

    // ─── 8. Push function scopes using RAII guard ──────────────────────────
    // This pushes:
    //   1. A symbol scope for parameters
    //   2. A function context (FuncBody) on the context stack
    ScopedFunction funcScope(ctx, decl, funcType->returnType);

    // ─── 9. Resolve parameters of the OUTERMOST group only ────────────────
    // Parameters are registered in the symbol scope pushed by ScopedFunction.
    for (ParamAST* param : funcType->params) {
        resolveParam(param, ctx);
    }

    // ─── 10. Resolve the body ──────────────────────────────────────────────
    // The parser has already desugared adjacent groups into nested
    // ReturnStmtAST → AnonFuncExprAST chains.
    bool bodyReturns = false;
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
    } else if (decl->body->isa<FuncRefStmtAST>()) {
        // Direct function reference body
        FuncRefStmtAST* refStmt = decl->body->as<FuncRefStmtAST>();
        TypeAST* refType = resolveExprWithTarget(refStmt->target, funcType, ctx);
        if (!refType || refType->isa<UnknownTypeAST>()) {
            // ─── ScopedFunction destructor automatically pops scopes ──────
            return;
        }
        bodyReturns = true;
    } else {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' has invalid body type");
        // ─── ScopedFunction destructor automatically pops scopes ──────────
        return;
    }

    // ─── 11. Check return paths ────────────────────────────────────────────
    TypeAST* expectedReturn = funcType->returnType;
    if (!bodyReturns && expectedReturn) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 12. Capture analysis (nested functions) ──────────────────────────
    if (ctx.getClosureDepth() > 0) {
        analyzeCaptures(decl, ctx);
    }

    // ─── 13. ScopedFunction destructor automatically pops:
    //     1. Function context (FuncBody)
    //     2. Parameter scope
    // ──────────────────────────────────────────────────────────────────────────
}

// ─── resolveParam ─────────────────────────────────────────────────────────────

/// @brief Resolve a parameter type and register it in the current scope.
///
/// Parameters are registered in Phase 2 (resolveFuncDecl) because they are
/// only needed when resolving the function body. Unlike top-level declarations,
/// parameters don't need to be visible for forward references.
///
/// @note This is called from resolveFuncDecl, NOT from registerFuncName.
void resolveParam(ParamAST* param, SemaContext& ctx) {
    if (!param) return;

    if (!param->name.isEmpty()) {
        ctx.insertValue(param);
    }
    if (param->hasSyntaxError) {
        param->type = ctx.getUnknownType();
        return;
    }

    // ─── 1. Resolve the parameter type ──────────────────────────────────────
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        param->type = ctx.getUnknownType();
        return;
    }
    
    // ─── 2. Validate const parameter ────────────────────────────────────────
    if (param->isConstParam) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
}

// ─── resolveGenericParam ──────────────────────────────────────────────────────

void resolveGenericParam(GenericParamDeclAST* param, SemaContext& ctx) {
    // ─── Push generic constraint context using RAII guard ──────────────────
    ScopedSemanticContext constraintCtx(ctx, ContextKind::GenericConstraint, param);
    
    for (NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
    
    // ─── Register this generic parameter in the current scope ──────────────
    ctx.insertGenericParam(param);
    
    // ─── ScopedSemanticContext destructor automatically pops the context ───
}

// ─── resolveEnumDecl ──────────────────────────────────────────────────────────

void resolveEnumDecl(EnumDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerEnumName() ──────────────
    // Do NOT call ctx.insertType() here.

    // ─── 1. Resolve backing type ────────────────────────────────────────────
    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, decl,
                                  "invalid backing type for enum '",
                                  ctx.pool.lookup(decl->name), "'");
        }
    }

    // ─── 2. Validate enum variants ──────────────────────────────────────────
    // Note: EnumVariantAST doesn't have a resolvedType field because variants
    // are values of the enum type. The enum type itself is the type.
    for (EnumVariantAST* variant : decl->variants) {
        validateAllAttributes(variant, ctx);

        // Check duplicate variant values
        for (EnumVariantAST* existing : decl->variants) {
            if (existing == variant) {
                ctx.diagnostics.error(DiagCode::Sem_DuplicateValue, variant,
                                      "duplicate enum variant ", ctx.pool.lookup(variant->name));
                break;
            }
            if (existing->value == variant->value) {
                ctx.diagnostics.error(DiagCode::Sem_DuplicateValue, variant,
                                      "duplicate enum value ", std::to_string(variant->value),
                                      " (also used by '", ctx.pool.lookup(existing->name), "')");
                break;
            }
        }
    }

    // ─── 3. Generate mangled name ────────────────────────────────────────────
    // This is CRITICAL for CodeGen to create unique enum types.
    // Without mangling, two enums with the same name in different modules
    // would collide in LLVM.
    //
    // Example:
    //   module1: enum Status { Ok = 0, Err = 1 }
    //   module2: enum Status { Active = 0, Inactive = 1 }
    //   Both would be named "Status" without mangling → conflict!
    //
    // With mangling:
    //   module1 → _Lmodule1_Status_Bi_V2
    //   module2 → _Lmodule2_Status_Bi_V2
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }
}

// ─── resolveTraitDecl ─────────────────────────────────────────────────────────

void resolveTraitDecl(TraitDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerTraitName() ─────────────
    // Do NOT call ctx.insertType() here.

    // ─── 1. Resolve generic parameters ──────────────────────────────────────
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Resolve trait fields ────────────────────────────────────────────
    for (TraitFieldDeclAST* field : decl->fields) {
        validateAllAttributes(field, ctx);

        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── Arena validation: Cannot store Arena in struct fields ──────
        if (isArenaType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name), "' cannot be of type Arena");
            ctx.diagnostics.note(field,
                                 "Arena is scope-confined and cannot be stored in traits");
            continue;
        }

        // ─── Validate const trait field ──────────────────────────────────
        // const trait fields must have a definite type (not nullable or fallible)
        if (field->isConst()) {
            if (!validateConstType(fieldType, field->name, "trait field", ctx)) {
                continue;
            }
        }
    }

    // ─── 3. Validate generic parameter usage ───────────────────────────────
    std::vector<TypeAST*> types;
    for (TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── resolveStructDecl ────────────────────────────────────────────────────────

void resolveStructDecl(StructDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        return;
    }

    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerStructName() ────────────
    // Do NOT call ctx.insertType() here.

    ScopedTypeDefinition defining(ctx, decl);

    // ─── 1. Resolve generic parameters FIRST ──────────────────────────────
    // Generic parameters must be resolved before fields because field types
    // may reference them (e.g., struct Box<T> { value T; })
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Push a scope for struct fields ──────────────────────────────
    // This scope exists only during struct resolution.
    // Fields are registered here so they can be resolved during default
    // body resolution (for function fields with default bodies).
    ctx.pushScope();

    // ─── 3. Register all fields in the struct scope ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
        if (!field->name.isEmpty()) {
            ctx.insertValue(field);
        }
    }

    // ─── 4. Resolve fields and compute logical layout ──────────────────────
    resolveStructFields(decl, ctx);

    // ─── 5. Validate trait implementations ──────────────────────────────────
    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported
    }

    // ─── 6. Validate generic parameter usage ───────────────────────────────
    std::vector<TypeAST*> types;
    for (FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);

    // ─── 7. Generate mangled name ───────────────────────────────────────────
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }

    // ─── 8. Pop the struct scope ─────────────────────────────────────────
    ctx.popScope();
}

// ─── resolveStructFields ──────────────────────────────────────────────────────

void resolveStructFields(StructDeclAST* decl, SemaContext& ctx) {
    // ─── Phase 1: Resolve field types and validate ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
        if (field->hasSyntaxError) continue;

        validateAllAttributes(field, ctx);

        // ─── 1. Resolve the field's type ──────────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) continue;

        // ─── Arena validation: Cannot store Arena in struct fields ──────
        if (isArenaType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name), "' cannot be of type Arena");
            ctx.diagnostics.note(field,
                                 "Arena is scope-confined and cannot be stored in structs");
            continue;
        }

        // ─── 2. Downward Flow Rule: Check borrowed types ──────────────────
        if (isBorrowedType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name),
                                  "' has borrowed type (",
                                  typeToString(fieldType, ctx.pool),
                                  ") — struct fields cannot contain &T or [_]T");
            continue;
        }

        // ─── 3. Validate self-reference ──────────────────────────────────────
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── 4. Validate const field type ──────────────────────────────────
        if (field->isConst()) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── 5. Handle default value (NO self synthesis needed!) ──────────
        bool isFunctionType = fieldType->isa<FuncTypeAST>();

        if (isFunctionType && field->defaultBody) {
            // ─── The parser already synthesized self as the first parameter ──
            // We just need to resolve the parameters and the body.
            FuncTypeAST* funcType = fieldType->as<FuncTypeAST>();
            
            // ─── Push scope for parameters ──────────────────────────────────
            ctx.pushScope();
            
            // Resolve all parameters (including self)
            for (ParamAST* param : funcType->params) {
                resolveParam(param, ctx);
            }
            
            // ─── Resolve the body ──────────────────────────────────────────
            if (field->defaultBody->isa<BlockStmtAST>()) {
                resolveBlock(field->defaultBody->as<BlockStmtAST>(), ctx);
            } else {
                ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, field,
                                      "function field body must be a block");
            }
            
            ctx.popScope();
            
        } else if (isFunctionType && field->defaultVal) {
            // ─── Expression default (function reference) ────────────────────
            // The self parameter must already be in the signature
            // (the user wrote it explicitly)
            TypeAST* initType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
            if (!initType || initType->isa<UnknownTypeAST>()) {
                continue;
            }

            if (!isFunctionValue(field->defaultVal, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, field,
                                      "field '", ctx.pool.lookup(field->name),
                                      "' default value must be a function value");
                continue;
            }
        } else if (!isFunctionType && field->defaultVal) {
            // ─── Non-function field with default value ────────────────────
            TypeAST* initType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
            if (!initType || initType->isa<UnknownTypeAST>()) {
                continue;
            }
        }
        // ─── No default value ─────────────────────────────────────────────
        // The struct literal must supply a value for this field.
    }

    // ─── Phase 2: Compute logical layout ────────────────────────────────────
    for (size_t i = 0; i < decl->fields.size(); ++i) {
        FieldDeclAST* field = decl->fields[i];
        field->fieldIndex = i;
    }
}

} // namespace sema