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
///   - Parameters: Registered in Phase 1 (registerFuncName) and Phase 2 (resolveParam)
///   - Generic params: Registered in Phase 1 (registerFuncName) and Phase 2 (resolveGenericParam)

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../const_eval/ConstEvaluator.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/registry/AttributeValidator.hpp"

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

    // ─── NOTE: Registration is handled by resolveDecl() ──────────────
    // Do NOT call ctx.insertValue() here.
}

void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
    // ─── 1. Validate all attributes ────────────────────────────────────────
    // This validates @[foreign] syntax (ABI string, etc.)
    validateAllAttributes(decl, ctx);

    // ─── 2. Check if @[foreign] is present ────────────────────────────────
    // We check this directly from the attributes list.
    // The attribute validator already validated the syntax,
    // now we just need to know if it's there.
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

    // ─── 4. Handle @[foreign] functions ────────────────────────────────────
    if (foreignAttr) {
        // The attribute validator already validated:
        //   - ABI is "C"
        //   - Function has no body (warning)
        //   - Parameter types are FFI-compatible
        //   - Return type is FFI-compatible
        //   - No generic parameters
        
        // Mark as const (foreign functions are compile-time constants)
        const_cast<FuncDeclAST*>(decl)->isConst = true;
        
        // No body to resolve - we're done
        return;
    }

    // ─── 5. Resolve generic parameters ───────────────────────────────────────
    for (const GenericParamDeclAST* g : decl->genericParams) {
        resolveGenericParam(g, ctx);
    }

    // ─── 6. Resolve parameters ────────────────────────────────────────────────
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
    ctx.stack.pushFunction(const_cast<FuncDeclAST*>(decl), expectedReturn, decl->loc);

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

    // ─── 12. Mark as const if applicable ─────────────────────────────────────
    if (decl->keyword == DeclKeyword::Const) {
        const_cast<FuncDeclAST*>(decl)->isConst = true;
    }
}

/// @brief Resolve a parameter type and register it in the current scope.
///
/// Parameters are special because they are only discovered during Phase 2
/// (when walking function signatures) and need to be registered in the
/// current scope for the function body to reference them.
///
/// @note This is called from both Phase 1 (registerFuncName) and Phase 2
///       (resolveFuncDecl). The duplicate insertValue calls are safe because
///       insertValue checks for duplicates before inserting.
void resolveParam(const ParamAST* param, SemaContext& ctx) {
    // Parameters don't support attributes, so we skip validateAllAttributes
    // If they somehow have attributes, they were already rejected by the parser.

    TypeAST* paramType = resolveType(param->type, ctx);
    if (!paramType) {
        return;
    }
    
    if (param->isConst) {
        if (!validateConstType(paramType, param->name, "parameter", ctx)) {
            return;
        }
    }
    
    // Parameters are registered in the current scope
    ctx.insertValue(param);
}

void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx) {
    for (const NamedTypeAST* constraint : param->constraints) {
        resolveTraitRef(constraint, ctx);
    }
}

void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx) {
    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by resolveDecl() ─────────────────────
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

    // ─── NOTE: Registration is handled by resolveDecl() ─────────────────────
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
    validateAllAttributes(decl, ctx);

    // ─── NOTE: Registration is handled by resolveDecl() ─────────────────────
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

        TypeAST* fieldType = resolveType(field->type, ctx);
        if (!fieldType) {
            continue;
        }

        // ─── Downward Flow Rule: Check borrowed types ──────────────────────
        // Struct fields cannot contain &T or [_]T
        if (isBorrowedType(fieldType)) {
            ctx.diagnostics.error(DiagCode::Sem_RefInStruct, field,
                                  "field '", ctx.pool.lookup(field->name),
                                  "' has borrowed type (",
                                  debug::typeToString(fieldType, ctx.pool),
                                  ") — struct fields cannot contain &T or [_]T");
            continue;
        }

        // ─── Validate self-reference ──────────────────────────────────────
        isValidStructSelfReference(fieldType, decl, ctx);

        // ─── Validate const field type ──────────────────────────────────
        if (field->isConst) {
            if (!validateConstType(fieldType, field->name, "struct field", ctx)) {
                continue;
            }
        }

        // ─── Handle default value ───────────────────────────────────────
        bool isFunctionType = fieldType->isa<FuncTypeAST>();

        if (field->defaultBody) {
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
    }
}

} // namespace sema