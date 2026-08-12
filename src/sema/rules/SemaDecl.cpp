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
#include "debug/DebugUtils.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../support/MangledName.hpp"
#include "../support/CaptureAnalysis.hpp"
#include "../registry/AttributeValidator.hpp"


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

    // ─── 2. Register generic parameters ──────────────────────────────────────
    // Generic parameters need to be registered in Phase 1 so they can be
    // resolved when types are resolved in Phase 2.
    for (GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }

    // ─── 3. Parameters are NOT registered in Phase 1 ─────────────────────────
    // Parameters are only needed inside the function body, which is resolved
    // in Phase 2. They are registered when resolveFuncDecl calls resolveParam.
}

void registerEnumName(EnumDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (EnumVariantAST* variant : decl->variants) {
        ctx.insertValue(variant);
    }
}

void registerTraitName(TraitDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }
}

void registerStructName(StructDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }
    registerStructFieldNames(decl, ctx);
}

void registerStructFieldNames(StructDeclAST* decl, SemaContext& ctx) {
    for (FieldDeclAST* field : decl->fields) {
        ctx.insertValue(field);
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

    // ─── 5. Generate mangled name for exported globals ───────────────────
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
            const_cast<VarDeclAST*>(decl)->mangledName = mangled;
            LOG_SEMA("Generated mangled name for exported variable '",
                     ctx.pool.lookup(decl->name), "' → '",
                     ctx.pool.lookup(mangled), "'");
        }
    }

    // ─── NOTE: Registration is handled by registerVarName() ──────────
    // Do NOT call ctx.insertValue() here.
}

// ─── resolveFuncDecl ──────────────────────────────────────────────────────────

void resolveFuncDecl(FuncDeclAST* decl, SemaContext& ctx) {
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
    const FuncTypeAST* funcType = decl->funcType;
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // ─── 4. Handle @[foreign] functions ────────────────────────────────────
    if (decl->isForeignFunction) {
        // Foreign functions use their original name as the symbol
        const_cast<FuncDeclAST*>(decl)->mangledName = decl->name;
        LOG_SEMA("Foreign function '", ctx.pool.lookup(decl->name),
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
        const_cast<FuncDeclAST*>(decl)->mangledName = mangled;
        LOG_SEMA("Generated mangled name for function '",
                 ctx.pool.lookup(decl->name), "' → '",
                 ctx.pool.lookup(mangled), "'");
    }

    // ─── 8. Analyze body ──────────────────────────────────────────────────────
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingFuncBody, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        ctx.popScope();
        return;
    }

    // ─── 9. Push function context with expected return type ──────────────────
    const TypeAST* expectedReturn = funcType ? funcType->returnType : nullptr;
    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), expectedReturn);

    bool bodyReturns = false;
    
    // ─── 10. Resolve the body ──────────────────────────────────────────────────
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

    // ─── 11. Verify return paths ──────────────────────────────────────────────
    if (!bodyReturns && expectedReturn) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 12. CAPTURE ANALYSIS for nested functions ──────────────────────────
    if (ctx.getClosureDepth() > 0) {
        LOG_SEMA("resolveFuncDecl: analyzing captures for nested function '",
                 ctx.pool.lookup(decl->name), "' at depth ", ctx.getClosureDepth());
        analyzeCaptures(const_cast<FuncDeclAST*>(decl), ctx);
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
    // ─── 1. Resolve the parameter type ──────────────────────────────────────
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        return;
    }
    
    // ─── 2. Validate const parameter ────────────────────────────────────────
    if (param->isConstParam) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
    // ─── 4. Register the parameter in the current scope ────────────────────
    // The current scope is the function's parameter scope (pushed in resolveFuncDecl)
    ctx.insertValue(param);
}

// ─── resolveGenericParam ──────────────────────────────────────────────────────

void resolveGenericParam(GenericParamDeclAST* param, SemaContext& ctx) {
    for (NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
}

// ─── resolveEnumDecl ──────────────────────────────────────────────────────────

void resolveEnumDecl(EnumDeclAST* decl, SemaContext& ctx) {
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
    // Note: EnumVariantAST doesn't have a semanticType field because variants
    // are values of the enum type. The enum type itself is the type.
    for (EnumVariantAST* variant : decl->variants) {
        validateAllAttributes(variant, ctx);

        // Check duplicate variant values
        for (EnumVariantAST* existing : decl->variants) {
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

// ─── resolveTraitDecl ─────────────────────────────────────────────────────────

void resolveTraitDecl(TraitDeclAST* decl, SemaContext& ctx) {
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
    std::vector<const TypeAST*> types;
    for (TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── resolveStructDecl ────────────────────────────────────────────────────────

void resolveStructDecl(StructDeclAST* decl, SemaContext& ctx) {
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
    std::vector<const TypeAST*> types;
    for (FieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

// ─── resolveStructFields ──────────────────────────────────────────────────────

void resolveStructFields(StructDeclAST* decl, SemaContext& ctx) {
    // ─── Phase 1: Resolve field types and validate ──────────────────────────
    for (FieldDeclAST* field : decl->fields) {
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
                                  debug::typeToString(fieldType, ctx.pool),
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
                                      debug::typeToString(fieldType, ctx.pool));
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
                const FuncTypeAST* funcType = fieldType->as<FuncTypeAST>();

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
        const_cast<FieldDeclAST*>(field)->fieldIndex = i;
    }
}

} // namespace sema