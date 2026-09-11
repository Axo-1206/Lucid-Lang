/// @file sema/context/Generic.cpp
/// @brief Implementation of generic instantiation and substitution utilities.

#include "Generic.hpp"
#include "sema/support/MangledName.hpp"
#include "sema/types/GenericHelpers.hpp"
#include "core/trace/Trace.hpp"
#include "sema/types/SemaType.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Create a shell struct and register it in the cache
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create an empty shell for an instantiated struct and register it in the cache.
/// 
/// This is the first half of the "register before recursing" pattern.
/// The shell is created with no fields and immediately inserted into the cache.
/// If a recursive call tries to create the same instantiation, it finds the shell
/// and returns it, breaking the infinite loop.
/// 
/// @param templateDecl The generic struct template.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param mangledName The mangled name for the instantiation.
/// @param ctx The semantic context.
/// @return A shell StructDeclAST with empty fields, or nullptr on error.
static StructDeclAST* createInstantiatedStructShell(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    InternedString mangledName,
    SemaContext& ctx
) {
    if (!templateDecl || !mangledName.isValid()) {
        return nullptr;
    }

    // ─── Create the shell with empty fields ──────────────────────────────
    StructDeclAST* shell = ctx.arena.make<StructDeclAST>(
        mangledName,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),  // No generic params
        ctx.arena.emptySpan<FieldDeclAST*>(),         // Empty fields (will be filled later)
        templateDecl->traitRefs,                      // Traits are unchanged
        templateDecl->isPacked
    );
    shell->mangledName = mangledName;
    shell->loc = templateDecl->loc;

    // ─── Register the shell BEFORE any substitution ──────────────────────
    // This breaks recursive cycles. If substituting a field triggers
    // createInstantiatedStruct for the same (templateDecl, typeArgs), the
    // cache will return this shell.
    // 
    // Use typeArgs, NOT templateDecl->genericParams!
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = shell;

    return shell;
}

/// @brief Finalize an instantiated struct by filling its fields.
/// 
/// This is the second half of the "register before recursing" pattern.
/// After the shell is registered, we substitute all fields and create
/// the final struct. The cache entry is updated to point to the final struct.
/// 
/// @param templateDecl The generic struct template.
/// @param typeArgs The concrete type arguments.
/// @param shell The shell struct to finalize.
/// @param ctx The semantic context.
/// @return The finalized StructDeclAST, or nullptr on error.
static StructDeclAST* finalizeInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    StructDeclAST* shell,
    SemaContext& ctx
) {
    if (!templateDecl || !shell) return nullptr;

    // ─── Create substitution context ──────────────────────────────────────
    GenericSubstitution subst{templateDecl->genericParams, typeArgs};

    // ─── Substitute all fields ────────────────────────────────────────────
    std::vector<FieldDeclAST*> fieldList;
    fieldList.reserve(templateDecl->fields.size());
    bool hasError = false;

    for (FieldDeclAST* field : templateDecl->fields) {
        // Substitute the field type (this may recursively call back into
        // createInstantiatedStruct, but the shell is already in the cache)
        TypeAST* substitutedType = substituteType(field->type, subst, ctx);
        if (!substitutedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, field,
                "field '", ctx.pool.lookup(field->name),
                "' has invalid type in instantiation");
            hasError = true;
            break;
        }

        // Substitute default value if present
        ExprAST* substitutedDefault = field->defaultVal 
            ? substituteExpr(field->defaultVal, subst, ctx) 
            : nullptr;

        // Substitute default body if present
        StmtAST* substitutedBody = field->defaultBody 
            ? substituteStmt(field->defaultBody, subst, ctx) 
            : nullptr;

        // Create the new field
        FieldDeclAST* newField = ctx.arena.make<FieldDeclAST>(
            field->name,
            substitutedType,
            substitutedDefault,
            substitutedBody,
            field->isConstField
        );
        newField->loc = field->loc;
        fieldList.push_back(newField);
    }

    if (hasError) {
        return nullptr;
    }

    // ─── Create the final struct with all fields ─────────────────────────
    StructDeclAST* finalStruct = ctx.arena.make<StructDeclAST>(
        shell->name,                                    // Same mangled name
        ctx.arena.emptySpan<GenericParamDeclAST*>(),   // No generic params
        ctx.arena.makeSpan<FieldDeclAST*>(fieldList),  // Populated fields
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    finalStruct->mangledName = shell->mangledName;
    finalStruct->loc = shell->loc;

    // ─── Update the cache entry to point to the final struct ─────────────
    // The shell is no longer needed; we replace it with the final struct.
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalStruct;

    Trace::detail("Finalized instantiated struct: ", 
                  ctx.pool.lookup(finalStruct->mangledName),
                  " (", fieldList.size(), " fields)");

    return finalStruct;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Create a shell function and register it in the cache
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create an empty shell for an instantiated function and register it in the cache.
/// 
/// This is the function equivalent of createInstantiatedStructShell.
/// 
/// @param templateDecl The generic function template.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param mangledName The mangled name for the instantiation.
/// @param ctx The semantic context.
/// @return A shell FuncDeclAST with empty body, or nullptr on error.
static FuncDeclAST* createInstantiatedFunctionShell(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    InternedString mangledName,
    SemaContext& ctx
) {
    if (!templateDecl || !mangledName.isValid()) {
        return nullptr;
    }

    // ─── Create the shell with empty body ──────────────────────────────────
    FuncDeclAST* shell = ctx.arena.make<FuncDeclAST>(
        mangledName,
        templateDecl->keyword,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),  // No generic params
        nullptr,                                       // funcType (will be filled later)
        nullptr                                        // body (will be filled later)
    );
    shell->mangledName = mangledName;
    shell->isForeignFunction = templateDecl->isForeignFunction;
    shell->isInline = templateDecl->isInline;
    shell->isNoInline = templateDecl->isNoInline;
    shell->hasClosure = templateDecl->hasClosure;
    shell->isReturned = templateDecl->isReturned;
    shell->loc = templateDecl->loc;

    // ─── Register the shell BEFORE any substitution ──────────────────────
    // Use typeArgs, NOT templateDecl->genericParams!
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = shell;

    return shell;
}

/// @brief Finalize an instantiated function by filling its body.
/// 
/// This is the function equivalent of finalizeInstantiatedStruct.
/// 
/// @param templateDecl The generic function template.
/// @param typeArgs The concrete type arguments.
/// @param shell The shell function to finalize.
/// @param ctx The semantic context.
/// @return The finalized FuncDeclAST, or nullptr on error.
static FuncDeclAST* finalizeInstantiatedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    FuncDeclAST* shell,
    SemaContext& ctx
) {
    if (!templateDecl || !shell) return nullptr;

    // ─── Create substitution context ──────────────────────────────────────
    GenericSubstitution subst{templateDecl->genericParams, typeArgs};

    // ─── Substitute function type ──────────────────────────────────────────
    TypeAST* substitutedFuncType = substituteType(templateDecl->funcType, subst, ctx);
    if (!substitutedFuncType || !substitutedFuncType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, templateDecl,
            "failed to substitute function type for '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    // ─── Substitute body ────────────────────────────────────────────────────
    StmtAST* substitutedBody = templateDecl->body 
        ? substituteStmt(templateDecl->body, subst, ctx) 
        : nullptr;

    // ─── Create the final function ─────────────────────────────────────────
    FuncDeclAST* finalFunc = ctx.arena.make<FuncDeclAST>(
        shell->name,
        shell->keyword,
        ctx.arena.emptySpan<GenericParamDeclAST*>(),  // No generic params
        substitutedFuncType->as<FuncTypeAST>(),
        substitutedBody
    );
    finalFunc->mangledName = shell->mangledName;
    finalFunc->isForeignFunction = shell->isForeignFunction;
    finalFunc->isInline = shell->isInline;
    finalFunc->isNoInline = shell->isNoInline;
    finalFunc->hasClosure = shell->hasClosure;
    finalFunc->isReturned = shell->isReturned;
    finalFunc->loc = shell->loc;

    // ─── Update the cache entry to point to the final function ────────────
    InstantiationKey key{templateDecl, typeArgs};
    ctx.instantiationCache[key] = finalFunc;

    Trace::detail("Finalized instantiated function: ", 
                  ctx.pool.lookup(finalFunc->mangledName));

    return finalFunc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Substitution Implementation
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* substituteType(TypeAST* type, const GenericSubstitution& subst, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return type;  // Primitives don't contain generic params

        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            
            // ─── Check if this is a generic parameter ──────────────────────
            if (subst.isParam(named->name)) {
                TypeAST* result = subst.lookup(named->name);
                if (result) {
                    // Recursively substitute the result
                    return substituteType(result, subst, ctx);
                }
                return type;
            }
            
            // ─── Check if this is a generic struct with args ──────────────
            if (!named->genericArgs.empty()) {
                bool changed = false;
                
                // Use makeSpan with transform - clean functional style
                auto subArgs = ctx.arena.makeSpan<TypeAST*>(
                    named->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        TypeAST* subArg = substituteType(arg, subst, ctx);
                        if (subArg != arg) changed = true;
                        return subArg;
                    }
                );
                
                if (changed) {
                    NamedTypeAST* newNamed = ctx.arena.make<NamedTypeAST>(named->name);
                    newNamed->genericArgs = subArgs;
                    newNamed->resolvedDecl = named->resolvedDecl;
                    newNamed->loc = named->loc;
                    return newNamed;
                }
            }
            
            return type;
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            TypeAST* subElement = substituteType(arr->element, subst, ctx);
            if (subElement != arr->element) {
                return ctx.getArrayType(arr->arrayKind, arr->size, subElement);
            }
            return type;
        }

        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            TypeAST* subInner = substituteType(nullable->inner, subst, ctx);
            if (subInner != nullable->inner) {
                return ctx.arena.make<NullableTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            TypeAST* subInner = substituteType(fallible->inner, subst, ctx);
            if (subInner != fallible->inner) {
                return ctx.arena.make<FallibleTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            TypeAST* subInner = substituteType(combined->inner, subst, ctx);
            if (subInner != combined->inner) {
                return ctx.arena.make<CombinedTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            TypeAST* subInner = substituteType(ref->inner, subst, ctx);
            if (subInner != ref->inner) {
                return ctx.arena.make<RefTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            TypeAST* subInner = substituteType(ptr->inner, subst, ctx);
            if (subInner != ptr->inner) {
                return ctx.arena.make<PtrTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            
            // Substitute parameter types
            bool paramsChanged = false;
            
            // Use makeSpan with transform for params
            auto subParams = ctx.arena.makeSpan<ParamAST*>(
                func->params,
                [&](ParamAST* param) -> ParamAST* {
                    if (!param->type) return param;
                    
                    TypeAST* subType = substituteType(param->type, subst, ctx);
                    if (subType != param->type) {
                        paramsChanged = true;
                        ParamAST* newParam = ctx.arena.make<ParamAST>(
                            param->name, subType, param->isVariadic, param->isConstParam);
                        newParam->loc = param->loc;
                        return newParam;
                    }
                    return param;
                }
            );
            
            // Substitute return type
            TypeAST* subReturn = func->returnType 
                ? substituteType(func->returnType, subst, ctx) 
                : nullptr;
            
            if (paramsChanged || subReturn != func->returnType) {
                FuncTypeAST* newFunc = ctx.arena.make<FuncTypeAST>();
                newFunc->params = subParams;
                newFunc->returnType = subReturn;
                newFunc->loc = func->loc;
                return newFunc;
            }
            return type;
        }

        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            TypeAST* subInner = substituteType(future->inner, subst, ctx);
            if (subInner != future->inner) {
                return ctx.arena.make<FutureTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            TypeAST* subInner = substituteType(thread->inner, subst, ctx);
            if (subInner != thread->inner) {
                return ctx.arena.make<ThreadTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            TypeAST* subElement = substituteType(simd->elementType, subst, ctx);
            if (subElement != simd->elementType) {
                return ctx.arena.make<SimdTypeAST>(subElement, simd->laneCount);
            }
            return type;
        }

        case ASTKind::ArenaType:
        case ASTKind::ArenaDescriptorType:
        case ASTKind::ModuleTypeAccess:
            return type;

        default:
            return type;
    }
}

StmtAST* substituteStmt(StmtAST* stmt, const GenericSubstitution& subst, SemaContext& ctx) {
    if (!stmt) return nullptr;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            BlockStmtAST* block = stmt->as<BlockStmtAST>();
            BlockStmtAST* newBlock = ctx.arena.make<BlockStmtAST>();
            
            // Use makeSpan with transform for statements
            newBlock->stmts = ctx.arena.makeSpan<StmtAST*>(
                block->stmts,
                [&](StmtAST* s) -> StmtAST* {
                    return substituteStmt(s, subst, ctx);
                }
            );
            
            // Copy scope exits (these are semantic metadata, not AST nodes)
            newBlock->scopeExits = block->scopeExits;
            return newBlock;
        }

        case ASTKind::ReturnStmt: {
            ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            ReturnStmtAST* newRet = ctx.arena.make<ReturnStmtAST>();
            if (ret->value) {
                newRet->value = substituteExpr(ret->value, subst, ctx);
            }
            newRet->loc = ret->loc;
            return newRet;
        }

        case ASTKind::ExprStmt: {
            ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
            ExprStmtAST* newExprStmt = ctx.arena.make<ExprStmtAST>(
                substituteExpr(exprStmt->expr, subst, ctx)
            );
            newExprStmt->loc = exprStmt->loc;
            return newExprStmt;
        }

        case ASTKind::DeclStmt: {
            DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            // Declarations inside the body need special handling
            // For now, return as-is (the declaration's type will be substituted elsewhere)
            // TODO: Need to substitute types inside the declaration
            return stmt;
        }

        case ASTKind::IfStmt: {
            IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            IfStmtAST* newIf = ctx.arena.make<IfStmtAST>();
            newIf->condition = substituteExpr(ifStmt->condition, subst, ctx);
            newIf->thenBranch = substituteStmt(ifStmt->thenBranch, subst, ctx);
            newIf->elseBranch = ifStmt->elseBranch 
                ? substituteStmt(ifStmt->elseBranch, subst, ctx) 
                : nullptr;
            newIf->loc = ifStmt->loc;
            return newIf;
        }

        case ASTKind::WhileStmt: {
            WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            WhileStmtAST* newWhile = ctx.arena.make<WhileStmtAST>();
            newWhile->condition = substituteExpr(whileStmt->condition, subst, ctx);
            newWhile->body = substituteStmt(whileStmt->body, subst, ctx);
            newWhile->loc = whileStmt->loc;
            return newWhile;
        }

        case ASTKind::DoWhileStmt: {
            DoWhileStmtAST* doWhileStmt = stmt->as<DoWhileStmtAST>();
            DoWhileStmtAST* newDoWhile = ctx.arena.make<DoWhileStmtAST>();
            newDoWhile->body = substituteStmt(doWhileStmt->body, subst, ctx);
            newDoWhile->condition = substituteExpr(doWhileStmt->condition, subst, ctx);
            newDoWhile->loc = doWhileStmt->loc;
            return newDoWhile;
        }

        case ASTKind::ForStmt: {
            ForStmtAST* forStmt = stmt->as<ForStmtAST>();
            ForStmtAST* newFor = ctx.arena.make<ForStmtAST>();
            newFor->indexVar = forStmt->indexVar;  // TODO: Need to substitute param types
            newFor->valueVar = forStmt->valueVar;  // TODO: Need to substitute param types
            newFor->iterable = substituteExpr(forStmt->iterable, subst, ctx);
            newFor->step = forStmt->step ? substituteExpr(forStmt->step, subst, ctx) : nullptr;
            newFor->body = substituteStmt(forStmt->body, subst, ctx);
            newFor->loc = forStmt->loc;
            return newFor;
        }

        case ASTKind::SwitchStmt: {
            SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
            SwitchStmtAST* newSwitch = ctx.arena.make<SwitchStmtAST>();
            newSwitch->subject = substituteExpr(switchStmt->subject, subst, ctx);
            
            // Use makeSpan with transform for cases
            newSwitch->cases = ctx.arena.makeSpan<SwitchCaseAST*>(
                switchStmt->cases,
                [&](SwitchCaseAST* caseNode) -> SwitchCaseAST* {
                    // Create new case with substituted values and body
                    SwitchCaseAST* newCase = ctx.arena.make<SwitchCaseAST>();
                    
                    // Substitute case values
                    newCase->values = ctx.arena.makeSpan<ExprAST*>(
                        caseNode->values,
                        [&](ExprAST* val) -> ExprAST* {
                            return substituteExpr(val, subst, ctx);
                        }
                    );
                    
                    newCase->body = substituteStmt(caseNode->body, subst, ctx)->as<BlockStmtAST>();
                    newCase->loc = caseNode->loc;
                    return newCase;
                }
            );
            
            newSwitch->defaultBody = switchStmt->defaultBody 
                ? substituteStmt(switchStmt->defaultBody, subst, ctx)->as<BlockStmtAST>()
                : nullptr;
            newSwitch->defaultLoc = switchStmt->defaultLoc;
            newSwitch->loc = switchStmt->loc;
            return newSwitch;
        }

        case ASTKind::BreakStmt:
        case ASTKind::ContinueStmt:
            // These have no data - just copy
            return stmt;

        case ASTKind::FuncRefStmt: {
            FuncRefStmtAST* funcRef = stmt->as<FuncRefStmtAST>();
            FuncRefStmtAST* newFuncRef = ctx.arena.make<FuncRefStmtAST>();
            newFuncRef->target = substituteExpr(funcRef->target, subst, ctx);
            newFuncRef->resolvedFunction = funcRef->resolvedFunction;
            newFuncRef->loc = funcRef->loc;
            return newFuncRef;
        }

        default:
            return stmt;
    }
}

ExprAST* substituteExpr(ExprAST* expr, const GenericSubstitution& subst, SemaContext& ctx) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return expr;  // Literals are immutable

        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            // Generic function references in the body need to be handled
            // They should have been resolved by Sema already
            return expr;
        }

        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            BinaryExprAST* newBin = ctx.arena.make<BinaryExprAST>(bin->op);
            newBin->left = substituteExpr(bin->left, subst, ctx);
            newBin->right = substituteExpr(bin->right, subst, ctx);
            newBin->loc = bin->loc;
            return newBin;
        }

        case ASTKind::UnaryExpr: {
            UnaryExprAST* unary = expr->as<UnaryExprAST>();
            UnaryExprAST* newUnary = ctx.arena.make<UnaryExprAST>(unary->op);
            newUnary->operand = substituteExpr(unary->operand, subst, ctx);
            newUnary->loc = unary->loc;
            return newUnary;
        }

        case ASTKind::CallExpr: {
            CallExprAST* call = expr->as<CallExprAST>();
            CallExprAST* newCall = ctx.arena.make<CallExprAST>(call->hasArgPack);
            
            // ─── Substitute the callee (which may have generic args) ──────────────
            newCall->callee = substituteExpr(call->callee, subst, ctx);
            
            // ─── Substitute arguments ──────────────────────────────────────────────
            newCall->args = ctx.arena.makeSpan<ExprAST*>(
                call->args,
                [&](ExprAST* arg) -> ExprAST* {
                    return substituteExpr(arg, subst, ctx);
                }
            );
            
            // ─── Copy semantic fields ──────────────────────────────────────────────
            // Note: genericArgs is NOT stored on CallExprAST - it's on the callee
            newCall->loc = call->loc;
            
            return newCall;
        }

        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            FieldAccessExprAST* newField = ctx.arena.make<FieldAccessExprAST>(field->fieldName);
            newField->object = substituteExpr(field->object, subst, ctx);
            newField->resolvedDecl = field->resolvedDecl;
            newField->ownerType = field->ownerType;
            newField->isEnumAccess = field->isEnumAccess;
            newField->fieldIndex = field->fieldIndex;
            newField->loc = field->loc;
            return newField;
        }

        case ASTKind::ArrayLiteralExpr: {
            ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            
            // Use makeSpan with transform for elements
            auto subElements = ctx.arena.makeSpan<ExprAST*>(
                arr->elements,
                [&](ExprAST* elem) -> ExprAST* {
                    return substituteExpr(elem, subst, ctx);
                }
            );
            
            ArrayLiteralExprAST* newArr = ctx.arena.make<ArrayLiteralExprAST>(subElements);
            newArr->loc = arr->loc;
            return newArr;
        }

        case ASTKind::StructLiteralExpr: {
            StructLiteralExprAST* structExpr = expr->as<StructLiteralExprAST>();
            
            // Use makeSpan with transform for field inits
            auto subInits = ctx.arena.makeSpan<FieldInitAST*>(
                structExpr->inits,
                [&](FieldInitAST* init) -> FieldInitAST* {
                    FieldInitAST* newInit = ctx.arena.make<FieldInitAST>(
                        init->name, 
                        substituteExpr(init->value, subst, ctx)
                    );
                    newInit->loc = init->loc;
                    return newInit;
                }
            );
            
            StructLiteralExprAST* newStruct = ctx.arena.make<StructLiteralExprAST>(
                structExpr->typeName, 
                structExpr->genericArgs, 
                subInits
            );
            newStruct->resolvedDecl = structExpr->resolvedDecl;
            newStruct->loc = structExpr->loc;
            return newStruct;
        }

        case ASTKind::IndexExpr: {
            IndexExprAST* index = expr->as<IndexExprAST>();
            IndexExprAST* newIndex = ctx.arena.make<IndexExprAST>(
                substituteExpr(index->target, subst, ctx),
                substituteExpr(index->index, subst, ctx)
            );
            newIndex->loc = index->loc;
            return newIndex;
        }

        case ASTKind::SliceExpr: {
            SliceExprAST* slice = expr->as<SliceExprAST>();
            SliceExprAST* newSlice = ctx.arena.make<SliceExprAST>(
                substituteExpr(slice->target, subst, ctx),
                slice->start ? substituteExpr(slice->start, subst, ctx) : nullptr,
                slice->end ? substituteExpr(slice->end, subst, ctx) : nullptr,
                slice->isExclusive
            );
            newSlice->loc = slice->loc;
            return newSlice;
        }

        case ASTKind::NullCoalesceExpr: {
            NullCoalesceExprAST* coalesce = expr->as<NullCoalesceExprAST>();
            NullCoalesceExprAST* newCoalesce = ctx.arena.make<NullCoalesceExprAST>(
                substituteExpr(coalesce->value, subst, ctx),
                substituteExpr(coalesce->fallback, subst, ctx)
            );
            newCoalesce->loc = coalesce->loc;
            return newCoalesce;
        }

        case ASTKind::AssignExpr: {
            AssignExprAST* assign = expr->as<AssignExprAST>();
            AssignExprAST* newAssign = ctx.arena.make<AssignExprAST>(assign->op);
            newAssign->lhs = substituteExpr(assign->lhs, subst, ctx);
            newAssign->rhs = substituteExpr(assign->rhs, subst, ctx);
            newAssign->loc = assign->loc;
            return newAssign;
        }

        case ASTKind::ModuleAccessExpr: {
            ModuleAccessExprAST* mod = expr->as<ModuleAccessExprAST>();
            // Module access expressions don't contain generic params
            // but we need to substitute generic args if any
            ModuleAccessExprAST* newMod = ctx.arena.make<ModuleAccessExprAST>(
                mod->moduleName, mod->memberName
            );
            
            // Substitute generic args if present
            if (!mod->genericArgs.empty()) {
                newMod->genericArgs = ctx.arena.makeSpan<TypeAST*>(
                    mod->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        return substituteType(arg, subst, ctx);
                    }
                );
            }
            
            newMod->resolvedDecl = mod->resolvedDecl;
            newMod->loc = mod->loc;
            return newMod;
        }

        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* anon = expr->as<AnonFuncExprAST>();
            TypeAST* subFuncType = substituteType(anon->funcType, subst, ctx);
            AnonFuncExprAST* newAnon = ctx.arena.make<AnonFuncExprAST>(
                subFuncType ? subFuncType->as<FuncTypeAST>() : nullptr,
                substituteStmt(anon->body, subst, ctx)
            );
            newAnon->captures = anon->captures;
            newAnon->hasClosure = anon->hasClosure;
            newAnon->isReturned = anon->isReturned;
            newAnon->loc = anon->loc;
            return newAnon;
        }

        case ASTKind::IfExpr: {
            IfExprAST* ifExpr = expr->as<IfExprAST>();
            IfExprAST* newIf = ctx.arena.make<IfExprAST>(
                substituteExpr(ifExpr->condition, subst, ctx),
                substituteExpr(ifExpr->thenBranch, subst, ctx),
                substituteExpr(ifExpr->elseBranch, subst, ctx)
            );
            newIf->loc = ifExpr->loc;
            return newIf;
        }

        case ASTKind::RangeExpr: {
            RangeExprAST* range = expr->as<RangeExprAST>();
            RangeExprAST* newRange = ctx.arena.make<RangeExprAST>(range->isExclusive);
            newRange->lo = substituteExpr(range->lo, subst, ctx);
            newRange->hi = substituteExpr(range->hi, subst, ctx);
            newRange->loc = range->loc;
            return newRange;
        }

        case ASTKind::PipelineExpr: {
            PipelineExprAST* pipe = expr->as<PipelineExprAST>();
            
            // Use makeSpan with transform for pipeline steps
            auto subSteps = ctx.arena.makeSpan<PipelineStepAST*>(
                pipe->steps,
                [&](PipelineStepAST* step) -> PipelineStepAST* {
                    // Substitute pack args if any
                    auto subPackArgs = ctx.arena.makeSpan<ExprAST*>(
                        step->packArgs,
                        [&](ExprAST* arg) -> ExprAST* {
                            return substituteExpr(arg, subst, ctx);
                        }
                    );
                    
                    PipelineStepAST* newStep = ctx.arena.make<PipelineStepAST>(
                        substituteExpr(step->callable, subst, ctx),
                        subPackArgs
                    );
                    newStep->loc = step->loc;
                    return newStep;
                }
            );
            
            PipelineExprAST* newPipe = ctx.arena.make<PipelineExprAST>(
                substituteExpr(pipe->seed, subst, ctx),
                subSteps
            );
            newPipe->loc = pipe->loc;
            return newPipe;
        }

        case ASTKind::ComposeExpr: {
            ComposeExprAST* compose = expr->as<ComposeExprAST>();
            
            // Use makeSpan with transform for compose operands
            auto subOperands = ctx.arena.makeSpan<ComposeOperandAST*>(
                compose->operands,
                [&](ComposeOperandAST* op) -> ComposeOperandAST* {
                    // Substitute generic args if any
                    auto subGenericArgs = ctx.arena.makeSpan<TypeAST*>(
                        op->genericArgs,
                        [&](TypeAST* arg) -> TypeAST* {
                            return substituteType(arg, subst, ctx);
                        }
                    );
                    
                    ComposeOperandAST* newOp = ctx.arena.make<ComposeOperandAST>(
                        substituteExpr(op->callable, subst, ctx),
                        subGenericArgs
                    );
                    newOp->loc = op->loc;
                    return newOp;
                }
            );
            
            ComposeExprAST* newCompose = ctx.arena.make<ComposeExprAST>(
                substituteExpr(compose->left, subst, ctx),
                subOperands
            );
            newCompose->loc = compose->loc;
            return newCompose;
        }

        case ASTKind::IntrinsicCallExpr: {
            IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
            IntrinsicCallExprAST* newIntrinsic = ctx.arena.make<IntrinsicCallExprAST>(
                intrinsic->intrinsicName
            );
            
            // Substitute args if any
            if (!intrinsic->args.empty()) {
                newIntrinsic->args = ctx.arena.makeSpan<ExprAST*>(
                    intrinsic->args,
                    [&](ExprAST* arg) -> ExprAST* {
                        return substituteExpr(arg, subst, ctx);
                    }
                );
            }
            
            newIntrinsic->intrinsicID = intrinsic->intrinsicID;
            newIntrinsic->loc = intrinsic->loc;
            return newIntrinsic;
        }

        case ASTKind::ArenaAccessExpr: {
            ArenaAccessExprAST* arenaAccess = expr->as<ArenaAccessExprAST>();
            ArenaAccessExprAST* newArenaAccess = ctx.arena.make<ArenaAccessExprAST>(
                arenaAccess->methodName,
                arenaAccess->isStatic,
                arenaAccess->arenaExpr ? substituteExpr(arenaAccess->arenaExpr, subst, ctx) : nullptr
            );
            
            // Substitute generic args if any
            if (!arenaAccess->genericArgs.empty()) {
                newArenaAccess->genericArgs = ctx.arena.makeSpan<TypeAST*>(
                    arenaAccess->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        return substituteType(arg, subst, ctx);
                    }
                );
            }
            
            // Substitute args if any
            if (!arenaAccess->args.empty()) {
                newArenaAccess->args = ctx.arena.makeSpan<ExprAST*>(
                    arenaAccess->args,
                    [&](ExprAST* arg) -> ExprAST* {
                        return substituteExpr(arg, subst, ctx);
                    }
                );
            }
            
            newArenaAccess->resolvedDecl = arenaAccess->resolvedDecl;
            newArenaAccess->loc = arenaAccess->loc;
            return newArenaAccess;
        }

        default:
            return expr;
    }
}

bool containsGenericParams(TypeAST* type, const GenericSubstitution& subst) {
    if (!type) return false;

    switch (type->kind) {
        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            if (subst.isParam(named->name)) return true;
            for (TypeAST* arg : named->genericArgs) {
                if (containsGenericParams(arg, subst)) return true;
            }
            return false;
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            return containsGenericParams(arr->element, subst);
        }

        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return containsGenericParams(nullable->inner, subst);
        }

        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return containsGenericParams(fallible->inner, subst);
        }

        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return containsGenericParams(combined->inner, subst);
        }

        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return containsGenericParams(ref->inner, subst);
        }

        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return containsGenericParams(ptr->inner, subst);
        }

        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                if (containsGenericParams(param->type, subst)) return true;
            }
            if (func->returnType && containsGenericParams(func->returnType, subst)) {
                return true;
            }
            return false;
        }

        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return containsGenericParams(future->inner, subst);
        }

        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return containsGenericParams(thread->inner, subst);
        }

        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            return containsGenericParams(simd->elementType, subst);
        }

        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GenericResolution Implementation
// ─────────────────────────────────────────────────────────────────────────────

GenericResolution resolveGenericInstantiation(
    DeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx
) {
    GenericResolution result;
    
    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null declaration");
        return result;
    }

    // ─── Determine if this is a function or struct ──────────────────────────
    bool isFunction = templateDecl->isa<FuncDeclAST>();
    bool isStruct = templateDecl->isa<StructDeclAST>();
    
    if (!isFunction && !isStruct) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, templateDecl,
                              "declaration '", ctx.pool.lookup(templateDecl->name),
                              "' cannot be instantiated with generic arguments");
        return result;
    }

    // ─── Get generic parameters ──────────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (isFunction) {
        genericParams = templateDecl->as<FuncDeclAST>()->genericParams;
    } else {
        genericParams = templateDecl->as<StructDeclAST>()->genericParams;
    }

    // ─── Validate arity ──────────────────────────────────────────────────────
    if (typeArgs.size() != genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
                              "declaration '", ctx.pool.lookup(templateDecl->name),
                              "' expected ", genericParams.size(),
                              " generic arguments, got ", typeArgs.size());
        return result;
    }

    // ─── Validate each type argument ─────────────────────────────────────────
    for (TypeAST* arg : typeArgs) {
        if (!arg) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, templateDecl,
                                  "invalid generic argument (null)");
            return result;
        }
        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* namedArg = arg->as<NamedTypeAST>();
            if (!namedArg->resolvedDecl) {
                resolveNamedType(namedArg, ctx);
                if (!namedArg->resolvedDecl) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, arg,
                                          "unresolved type '", ctx.pool.lookup(namedArg->name),
                                          "' in generic argument");
                    return result;
                }
            }
        }

        if (!validateNestedGenericCompatibility(templateDecl, typeArgs, ctx)) {
            return result;
        }
    }

    // ─── Specialized path (default) ──────────────────────────────────────
    if (isFunction) {
        FuncDeclAST* instantiated = createInstantiatedFunction(
            templateDecl->as<FuncDeclAST>(), typeArgs, ctx);
        if (!instantiated) {
            return result;
        }
        result.resolvedDecl = instantiated;
    } else {
        StructDeclAST* instantiated = createInstantiatedStruct(
            templateDecl->as<StructDeclAST>(), typeArgs, ctx);
        if (!instantiated) {
            return result;
        }
        result.resolvedDecl = instantiated;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Instantiated Struct Creation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create an instantiated struct from a generic template.
/// 
/// Uses the "register before recursing" pattern to handle self-referential
/// types like `Node<T> { value: T, next: *Node<T> }`.
/// 
/// ─── Algorithm ──────────────────────────────────────────────────────────────
/// 1. Check the instantiation cache. If found, return it immediately.
/// 2. Validate arity.
/// 3. Generate the mangled name.
/// 4. Create a shell struct (empty fields) and register it in the cache.
/// 5. Substitute all field types (may recursively call back).
/// 6. Create the final struct with all fields.
/// 7. Update the cache entry to point to the final struct.
/// 
/// ─── Why This Works ─────────────────────────────────────────────────────────
/// The cache is populated BEFORE any field substitution. If a field's type
/// (e.g., `*Node<T>`) triggers a recursive instantiation of the same struct,
/// the cache returns the shell, breaking the infinite loop.
/// 
/// @param templateDecl The generic struct template (must have genericParams).
/// @param typeArgs The concrete type arguments (must match arity).
/// @param ctx The semantic context.
/// @return The instantiated StructDeclAST, or nullptr on error.
StructDeclAST* createInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx) 
{
    // ─── Guard: Validate inputs ────────────────────────────────────────────
    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null struct declaration");
        return nullptr;
    }

    // ─── Step 1: Check the cache ────────────────────────────────────────────
    InstantiationKey key{templateDecl, typeArgs};
    auto it = ctx.instantiationCache.find(key);
    if (it != ctx.instantiationCache.end()) {
        return it->second ? it->second->as<StructDeclAST>() : nullptr;
    }

    // ─── Step 2: Validate arity ─────────────────────────────────────────────
    if (typeArgs.size() != templateDecl->genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
            "struct '", ctx.pool.lookup(templateDecl->name),
            "' expected ", templateDecl->genericParams.size(),
            " generic arguments, got ", typeArgs.size());
        return nullptr;
    }

    // ─── Step 3: Generate mangled name ──────────────────────────────────────
    InternedString mangledName = generateMangledNameForGeneric(
        templateDecl, typeArgs, ctx);
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.error(DiagCode::Backend_InvalidIR, templateDecl,
            "failed to generate mangled name for generic struct '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    // ─── Step 4: Create and register the shell ──────────────────────────────
    StructDeclAST* shell = createInstantiatedStructShell(
        templateDecl, typeArgs, mangledName, ctx);
    if (!shell) {
        return nullptr;
    }

    // ─── Step 5: Finalize the struct ────────────────────────────────────────
    StructDeclAST* finalStruct = finalizeInstantiatedStruct(
        templateDecl, typeArgs, shell, ctx);
    if (!finalStruct) {
        return nullptr;
    }

    // ─── Step 6: Result ─────────────────────────────────────────────────────
    Trace::detail("Created instantiated struct: ", 
                  ctx.pool.lookup(finalStruct->mangledName),
                  " (", finalStruct->fields.size(), " fields)");

    return finalStruct;
}

// ─────────────────────────────────────────────────────────────────────────────
// Instantiated Function Creation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create an instantiated function from a generic template.
/// 
/// Uses the "register before recursing" pattern to handle recursive functions.
/// 
/// ─── Algorithm ──────────────────────────────────────────────────────────────
/// 1. Check the instantiation cache. If found, return it immediately.
/// 2. Validate arity.
/// 3. Generate the mangled name.
/// 4. Create a shell function (empty body) and register it in the cache.
/// 5. Substitute the function type and body (may recursively call back).
/// 6. Create the final function with all data.
/// 7. Update the cache entry to point to the final function.
/// 
/// @param templateDecl The generic function template.
/// @param typeArgs The concrete type arguments.
/// @param ctx The semantic context.
/// @return The instantiated FuncDeclAST, or nullptr on error.
FuncDeclAST* createInstantiatedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx) 
{
    // ─── Guard: Validate inputs ────────────────────────────────────────────
    if (!templateDecl) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, nullptr,
                              "cannot instantiate null function declaration");
        return nullptr;
    }

    // ─── Step 1: Check the cache ────────────────────────────────────────────
    InstantiationKey key{templateDecl, typeArgs};
    auto it = ctx.instantiationCache.find(key);
    if (it != ctx.instantiationCache.end()) {
        return it->second ? it->second->as<FuncDeclAST>() : nullptr;
    }

    // ─── Step 2: Validate arity ─────────────────────────────────────────────
    if (typeArgs.size() != templateDecl->genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
            "function '", ctx.pool.lookup(templateDecl->name),
            "' expected ", templateDecl->genericParams.size(),
            " generic arguments, got ", typeArgs.size());
        return nullptr;
    }

    // ─── Step 3: Generate mangled name ──────────────────────────────────────
    InternedString mangledName = generateMangledNameForGeneric(
        templateDecl, typeArgs, ctx);
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.error(DiagCode::Backend_InvalidIR, templateDecl,
            "failed to generate mangled name for generic function '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    // ─── Step 4: Create and register the shell ──────────────────────────────
    FuncDeclAST* shell = createInstantiatedFunctionShell(
        templateDecl, typeArgs, mangledName, ctx);
    if (!shell) {
        return nullptr;
    }

    // ─── Step 5: Finalize the function ──────────────────────────────────────
    FuncDeclAST* finalFunc = finalizeInstantiatedFunction(
        templateDecl, typeArgs, shell, ctx);
    if (!finalFunc) {
        return nullptr;
    }

    // ─── Step 6: Result ─────────────────────────────────────────────────────
    Trace::detail("Created instantiated function: ", 
                  ctx.pool.lookup(finalFunc->mangledName));

    return finalFunc;
}

} // namespace sema