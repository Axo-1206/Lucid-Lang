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
#include "sema/types/SemaCompare.hpp"


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

    /// Register all field names in a struct (no type resolution).
    for (FieldDeclAST* field : decl->fields) {
        if (!field->name.isEmpty()) {
            ctx.insertValue(field);
        }
    }
}

// =============================================================================
// PHASE 2: Type Resolution
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

    // ─── 1. Resolve the declared type ───────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        return;
    }

    // ─── 2. Validate const type ──────────────────────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        if (!validateConstType(declaredType, decl->name, "variable", ctx)) {
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
                decl->init->isConst = true;
            }
        }
    } else {
        // ─── NO INITIALIZER: Set default value state ────────────────
        
        // Create a placeholder expression to represent the default value
        // This expression won't be used for code generation - only Sema needs
        // the valueState and resolvedType for flow analysis.
        UnknownExprAST* defaultExpr = ctx.arena.make<UnknownExprAST>();
        defaultExpr->resolvedType = declaredType;
        defaultExpr->isLValue = false;
        defaultExpr->isConst = true;
        
        // ─── 4a. Check if type is nullable (T?) ──────────────────────
        if (isNullableType(declaredType)) {
            // Nullable variables default to nil
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (nullable type)");
            return;
        }
        
        // ─── 4b. Check if type is fallible (T!) ──────────────────────
        if (isFallibleType(declaredType)) {
            // Fallible variables default to err
            defaultExpr->valueState = ValueState::Err;
            decl->init = defaultExpr;
            
            Trace::info("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to err (fallible type)");
            return;
        }
        
        // ─── 4c. Check if type is combined (T?!) ─────────────────────
        if (declaredType->isa<CombinedTypeAST>()) {
            // Combined types default to nil (the tag is nil)
            defaultExpr->valueState = ValueState::Nil;
            decl->init = defaultExpr;
            
            Trace::detail("Variable '", ctx.pool.lookup(decl->name),
                     "' default-initialized to nil (combined type)");
            return;
        }
        
        // ─── 4d. Check for function type with default body ───────────
        // Function types can have a default body at declaration site,
        // but at variable declaration site, we need an explicit initializer.
        if (declaredType->isa<FuncTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                                  "function type variable '", ctx.pool.lookup(decl->name),
                                  "' must be initialized with a function value");
            return;
        }
        
        // ─── 4e. Non-nullable, non-fallible, non-combined type ──────
        // These cannot be default-initialized
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "variable '", ctx.pool.lookup(decl->name),
                              "' of type '", typeToString(declaredType, ctx.pool),
                              "' must be initialized (type is not nullable or fallible)");
        ctx.diagnostics.note(decl, 
                             "Consider using a nullable type (T?) or fallible type (T!) ",
                             "if you want default initialization to nil/err, or provide an initializer");
        return;
    }

    // ─── 5. Generate mangled name for exported globals ───────────────────
    // Only module-level variables that are exported need mangled names.
    // Local variables don't need mangling - they're not visible outside.
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

    // ─── NOTE: Registration is handled by registerVarName() ──────────
    // Do NOT call ctx.insertValue() here.
}

// ─── resolveFuncDecl ──────────────────────────────────────────────────────────

void resolveFuncDecl(FuncDeclAST* decl, SemaContext& ctx) {
    if (decl->hasSyntaxError) {
        if (decl->type) {
            decl->type = ctx.getUnknownType();
        }
        return;
    }

    // ─── 1. Validate all attributes ────────────────────────────────────────
    validateAllAttributes(decl, ctx);

    // ─── 2. Check if @[foreign] is present ────────────────────────────────
    InternedString foreignName = ctx.pool.intern("foreign");
    for (AttributeAST* attr : decl->attributes) {
        if (attr->name == foreignName) {
            decl->isForeignFunction = true;
        }
    }

    // ─── 3. Resolve function type ─────────────────────────────────────────────
    FuncTypeAST* funcType = decl->funcType;
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // ─── 4. Handle @[foreign] functions ────────────────────────────────────
    if (decl->isForeignFunction) {
        // Foreign functions use their original name as the symbol
        decl->mangledName = decl->name;
        Trace::info("Foreign function '", ctx.pool.lookup(decl->name),
                 "' uses symbol name: ", ctx.pool.lookup(decl->mangledName));
        return;
    }

    // ─── 5. Resolve generic parameters ───────────────────────────────────────
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 6. Resolve parameters ──────────────────────────────────────────────
    ctx.pushScope();
    
    for (const FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── 7. Generate mangled name BEFORE body resolution ──────────────────
    InternedString mangled = generateMangledName(decl, ctx);
    if (mangled.isValid()) {
        decl->mangledName = mangled;
    }

    // ─── 8. Analyze body ──────────────────────────────────────────────────────
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingFuncBody, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        ctx.popScope();
        return;
    }

    // ─── 9. Push function context with expected return type ──────────────────
    // The function resolveReturnStmt will resolve this requirement
    TypeAST* expectedReturn = funcType ? funcType->returnType : nullptr;
    ctx.stack.pushFunction(decl, expectedReturn);

    bool bodyReturns = false;
    
    // ─── 10. Resolve the body ──────────────────────────────────────────────────
    if (decl->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(decl->body->as<BlockStmtAST>(), ctx);
    } else if (decl->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
    } else if (decl->body->isa<FuncRefStmtAST>()) {
        FuncRefStmtAST* refStmt = decl->body->as<FuncRefStmtAST>();
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

    // ─── 11. Verify return paths ──────────────────────────────────────────────
    if (!bodyReturns && expectedReturn) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 12. CAPTURE ANALYSIS for nested functions ──────────────────────────
    if (ctx.getClosureDepth() > 0) {
        analyzeCaptures(decl, ctx);
    }

    // ─── 13. Pop function context ────────────────────────────────────────────
    ctx.stack.pop();

    // ─── 14. Pop scope ──────────────────────────────────────────────────────────
    ctx.popScope();
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
    for (NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
    // ─── Register this generic parameter in the current scope ──────────────
    ctx.insertGenericParam(param);
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

    // ─── 1. Resolve generic parameters ──────────────────────────────────────
    for (GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 2. Resolve fields and compute logical layout ──────────────────────
    resolveStructFields(decl, ctx);

    // ─── 3. Validate trait implementations ──────────────────────────────────
    if (!validateAllTraitImplementations(decl, ctx)) {
        // Error already reported
    }

    // ─── 4. Validate generic parameter usage ───────────────────────────────
    std::vector<TypeAST*> types;
    for (FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── resolveStructFields ──────────────────────────────────────────────────────

void resolveStructFields(StructDeclAST* decl, SemaContext& ctx) {
    // ─── Phase 1: Resolve field types and validate ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
        if (field->hasSyntaxError) {
            continue;
        }

        validateAllAttributes(field, ctx);

        // ─── 1. Resolve the field's type ──────────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── 2. Downward Flow Rule: Check borrowed types ──────────────────
        // Struct fields cannot contain &T or [_]T (borrowed types)
        if (isBorrowedType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name),
                                  "' has borrowed type (",
                                  typeToString(fieldType, ctx.pool),
                                  ") — struct fields cannot contain &T or [_]T");
            continue;
        }

        // ─── 3. Validate self-reference ──────────────────────────────────────
        // Self-reference is only valid if nullable or a raw pointer
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── 4. Validate const field type ──────────────────────────────────
        // const fields must have a definite type (not nullable or fallible)
        if (field->isConst()) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── 5. Handle default value ───────────────────────────────────────
        bool isFunctionType = fieldType->isa<FuncTypeAST>();

        if (field->defaultBody) {
            // ─── Block body default (only for function fields) ─────────────
            if (!isFunctionType) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, field,
                                      "block body can only be used with function fields, but '",
                                      ctx.pool.lookup(field->name), "' has type ",
                                      typeToString(fieldType, ctx.pool));
                continue;
            }

            FuncTypeAST* funcType = fieldType->as<FuncTypeAST>();

            if (funcType->params.empty()) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, field,
                                      "function field '", ctx.pool.lookup(field->name),
                                      "' must have at least one parameter");
                continue;
            }

            // ─── Push scope for the function field's parameters ──────────
            ctx.pushScope();

            for (ParamAST* param : funcType->params) {
                resolveParam(param, ctx);
            }

            if (field->defaultBody->isa<BlockStmtAST>()) {
                resolveBlock(field->defaultBody->as<BlockStmtAST>(), ctx);
            } else {
                ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, field,
                                      "function field body must be a block");
            }

            ctx.popScope();

        } else if (field->defaultVal) {
            // ─── Expression default ──────────────────────────────────────────
            if (isFunctionType) {
                FuncTypeAST* funcType = fieldType->as<FuncTypeAST>();

                TypeAST* initType = resolveExprWithTarget(field->defaultVal, funcType, ctx);
                if (!initType || initType->isa<UnknownTypeAST>()) {
                    continue;
                }

                if (!isFunctionValue(field->defaultVal, ctx)) {
                    ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, field,
                                          "field '", ctx.pool.lookup(field->name),
                                          "' default value must be a function value");
                    continue;
                }

            } else {
                TypeAST* initType = resolveExprWithTarget(field->defaultVal, fieldType, ctx);
                if (!initType || initType->isa<UnknownTypeAST>()) {
                    continue;
                }
            }
        }
        // ─── No default value ─────────────────────────────────────────────
        // The struct literal must supply a value for this field.
        // This is valid - no action needed.
    }

    // ─── Phase 2: Compute logical layout ────────────────────────────────────
    // Field indices are simple - just the position in the fields span.
    // This is always valid regardless of target ABI.
    for (size_t i = 0; i < decl->fields.size(); ++i) {
        FieldDeclAST* field = decl->fields[i];
        field->fieldIndex = i;
    }
}

} // namespace sema