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
// PHASE 1: Name Registration (Simplified)
// =============================================================================

void registerImportName(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) return;  // Error will be reported in Phase 2
    ctx.addImportAlias(decl->alias, target, decl);
}

void registerVarName(const VarDeclAST* decl, SemaContext& ctx) {
    ctx.insertValue(decl);
}

void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx) {
    ctx.insertValue(decl);

    for (const GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }

    ctx.pushScope();
    if (decl->funcType) {
        for (FuncTypeAST* group = decl->funcType; group; group = group->getNext()) {
            for (ParamAST* param : group->params) {
                ctx.insertValue(param);
            }
        }
    }
    ctx.popScope();
}

void registerParamName(const ParamAST* param, SemaContext& ctx) {
    ctx.insertValue(param);
}

void registerGenericParamName(const GenericParamDeclAST* param, SemaContext& ctx) {
    ctx.insertGenericParam(param);
}

void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (const EnumVariantAST* variant : decl->variants) {
        ctx.insertValue(variant);
    }
}

void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (const GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }
}

void registerStructName(const StructDeclAST* decl, SemaContext& ctx) {
    ctx.insertType(decl);
    for (const GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }
    registerStructFieldNames(decl, ctx);
}

void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        ctx.insertValue(field);
    }
}

// =============================================================================
// PHASE 2: Type Resolution
// =============================================================================

void resolveImportDecl(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, decl,
                              "undefined module '", ctx.pool.lookup(decl->path), "'");
    }
}

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
    // insertValue handles both module-level and local scopes
    // For top-level variables, this is a no-op (already registered in Phase 1)
    // For local variables, this registers them in the current block scope
    ctx.insertValue(decl);
}

void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // ─── 1. Resolve function type ─────────────────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }

    // ─── 2. Check for @[foreign] attribute ───────────────────────────────────
    const AttributeAST* foreignAttr = attr::findAttribute(
        decl->attributes,
        attr::kForeign(ctx)
    );

    if (foreignAttr) {
        const_cast<FuncDeclAST*>(decl)->isConst = false;
        return;
    }

    // ─── 3. Resolve generic parameters ───────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 4. REGISTER the function in the current scope ──────────────────────
    ctx.insertValue(decl);

    // ─── 5. Resolve parameters - REGISTER them in the function scope ────────
    ctx.pushScope();
    
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── 6. Analyze body ──────────────────────────────────────────────────────
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        ctx.popScope();
        return;
    }

    // ─── 7. Push function context with expected return type ──────────────────
    // The expected return type is the function's return type (funcType->returnType)
    // For curried functions, this will be another FuncTypeAST
    const TypeAST* expectedReturn = funcType ? funcType->returnType : nullptr;
    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), expectedReturn, decl->loc);

    bool bodyReturns = false;
    
    // ─── 8. Resolve the body ──────────────────────────────────────────────────
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

    // ─── 9. Verify return paths ──────────────────────────────────────────────
    // With the stack-based approach, we check that the body actually returns
    // something when expected. The return statements themselves check against
    // the current return type on the stack.
    if (!bodyReturns && expectedReturn) {
        // Non-void function must return a value
        // But if the body had a return statement, bodyReturns would be true
        // So this is a catch-all for functions that fall through
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 10. Pop function context ────────────────────────────────────────────
    ctx.stack.pop();
    ctx.popScope();

    // ─── 11. Mark as const if applicable ─────────────────────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        const_cast<FuncDeclAST*>(decl)->isConst = true;
    }
}

void resolveParam(const ParamAST* param, SemaContext& ctx) {
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        return;
    }
    
    if (param->isConst) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
    ctx.insertValue(param);
}

void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
}

void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // insertType handles both module-level and local scopes
    ctx.insertType(decl);

    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, decl,
                                  "invalid backing type for enum '",
                                  ctx.pool.lookup(decl->name), "'");
        }
    }

    for (const EnumVariantAST* variant : decl->variants) {
        attr::validateAttributes(variant, ctx);

        // Check duplicate variant values
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

void resolveTraitDecl(const TraitDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // insertType handles both module-level and local scopes
    ctx.insertType(decl);

    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    for (const TraitFieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

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

void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    attr::validateAttributes(decl, ctx);

    // insertType handles both module-level and local scopes
    ctx.insertType(decl);

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

void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx) {
    for (const FieldDeclAST* field : decl->fields) {
        attr::validateAttributes(field, ctx);

        // ─── 1. Resolve the field's type ──────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── 2. Validate self-reference ────────────────────────────────
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── 3. Validate const field type ──────────────────────────────
        if (field->isConst) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── 4. Validate reference type context (Downward Flow Rule) ────
        if (fieldType->isa<RefTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "reference type (&T) cannot be stored in struct field '",
                                  ctx.pool.lookup(field->name), "'");
            continue;
        }

        // ─── 5. Handle default value ────────────────────────────────────
        bool isFunctionType = fieldType->isa<FuncTypeAST>();

        if (field->defaultBody) {
            // ─── Function field with block body ──────────────────────────
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
            // ─── Expression default ──────────────────────────────────────
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

        } else {
            // ─── No default value ──────────────────────────────────────────
            // The struct literal must supply a value for this field
            // This is valid - no action needed
        }
    }
}

} // namespace sema