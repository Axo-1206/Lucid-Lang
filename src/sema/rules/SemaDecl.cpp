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

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/registry/AttributeValidator.hpp"

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

void registerImportName(const ImportDeclAST* decl, SemaContext& ctx) {
    ModuleAST* target = ctx.findModuleByPath(decl->path);
    if (!target) return;  // Error will be reported in Phase 2
    ctx.addImportAlias(decl->alias, target, decl);
}

void registerVarName(const VarDeclAST* decl, SemaContext& ctx) {
    ctx.insertValue(decl);
}

void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Register the function itself ──────────────────────────────────────
    ctx.insertValue(decl);

    // ─── 2. Register generic parameters ──────────────────────────────────────
    // Generic parameters need to be registered in Phase 1 so they can be
    // resolved when types are resolved in Phase 2.
    for (const GenericParamDeclAST* g : decl->genericParams) {
        ctx.insertGenericParam(g);
    }

    // ─── 3. Parameters are NOT registered in Phase 1 ─────────────────────────
    // Parameters are only needed inside the function body, which is resolved
    // in Phase 2. They are registered when resolveFuncDecl calls resolveParam.
    // 
    // Note: The previous implementation pushed a scope and registered parameters
    // here, but the scope was immediately popped, making the registration
    // pointless. We've removed this dead code.
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
    validateAllAttributes(decl, ctx);

    // ─── 1. Resolve the declared type ───────────────────────────────
    TypeAST* declaredType = resolveType(decl->type, ctx);
    if (!declaredType) {
        return;
    }
    
    // ─── 2. Store the resolved type on the declaration ──────────────
    const_cast<VarDeclAST*>(decl)->semanticType = declaredType;

    // ─── 3. Validate const type and initializer ──────────────────────
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

    // ─── NOTE: Registration is handled by registerVarName() ──────────
    // Do NOT call ctx.insertValue() here.
}

void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Validate all attributes ────────────────────────────────────────
    validateAllAttributes(decl, ctx);

    // ─── 2. Check if @[foreign] is present ────────────────────────────────
    InternedString foreignName = ctx.pool.intern("foreign");
    const AttributeAST* foreignAttr = nullptr;
    for (const AttributeAST* attr : decl->attributes) {
        if (attr->name == foreignName) {
            foreignAttr = attr;
        }
    }

    // ─── 3. Resolve function type ─────────────────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(decl->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return;
    }
    const_cast<FuncDeclAST*>(decl)->semanticType = funcType;

    // ─── 4. Handle @[foreign] functions ────────────────────────────────────
    if (foreignAttr) {
        // The attribute validator already validated:
        //   - ABI is "C"
        //   - Function has no body (warning)
        //   - Parameter types are FFI-compatible
        //   - Return type is FFI-compatible
        //   - No generic parameters
        // No body to resolve - we're done
        return;
    }

    // ─── 5. Resolve generic parameters ───────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 6. Resolve parameters ──────────────────────────────────────────────
    // Parameters are registered in the function's scope so they are
    // available when resolving the body. The scope is pushed here and
    // popped after the body is resolved.
    ctx.pushScope();
    
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── 7. Analyze body ──────────────────────────────────────────────────────
    if (!decl->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name), "' has no body");
        ctx.popScope();
        return;
    }

    // ─── 8. Push function context with expected return type ──────────────────
    const TypeAST* expectedReturn = funcType ? funcType->returnType : nullptr;
    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), expectedReturn);

    bool bodyReturns = false;
    
    // ─── 9. Resolve the body ──────────────────────────────────────────────────
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

    // ─── 10. Verify return paths ──────────────────────────────────────────────
    if (!bodyReturns && expectedReturn) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, decl,
                              "function '", ctx.pool.lookup(decl->name),
                              "' does not return a value on all paths");
    }

    // ─── 11. Pop function context ────────────────────────────────────────────
    ctx.stack.pop();
    ctx.popScope();
}

/// @brief Resolve a parameter type and register it in the current scope.
///
/// Parameters are registered in Phase 2 (resolveFuncDecl) because they are
/// only needed when resolving the function body. Unlike top-level declarations,
/// parameters don't need to be visible for forward references.
///
/// @note This is called from resolveFuncDecl, NOT from registerFuncName.
void resolveParam(const ParamAST* param, SemaContext& ctx) {
    // ─── 1. Resolve the parameter type ──────────────────────────────────────
    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        return;
    }
    
    // ─── 2. Store the resolved type on the parameter ──────────────────────
    const_cast<ParamAST*>(param)->semanticType = paramType;
    
    // ─── 3. Validate const parameter ────────────────────────────────────────
    if (param->isConst()) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
    // ─── 4. Register the parameter in the current scope ────────────────────
    // The current scope is the function's parameter scope (pushed in resolveFuncDecl)
    ctx.insertValue(param);
}

void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
}

void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerEnumName() ──────────────
    // Do NOT call ctx.insertType() here.

    if (decl->backingType) {
        if (!resolvePrimitiveType(decl->backingType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, decl,
                                  "invalid backing type for enum '",
                                  ctx.pool.lookup(decl->name), "'");
        }
    }

    for (const EnumVariantAST* variant : decl->variants) {
        validateAllAttributes(variant, ctx);

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
    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerTraitName() ─────────────
    // Do NOT call ctx.insertType() here.

    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    for (const TraitFieldDeclAST* field : decl->fields) {
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

    std::vector<const TypeAST*> types;
    for (const TraitFieldDeclAST* field : decl->fields) {
        types.push_back(field->type);
    }
    validateGenericParameterUsage(decl->genericParams, types, decl, ctx);
}

void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by registerStructName() ────────────
    // Do NOT call ctx.insertType() here.

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
        validateAllAttributes(field, ctx);

        // ─── 1. Resolve the field's type ──────────────────────────────────
        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── Store the resolved type on the field ──────────────────────
        const_cast<FieldDeclAST*>(field)->semanticType = fieldType;

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
}

} // namespace sema