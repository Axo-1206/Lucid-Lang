/// @file sema/context/Generic.cpp
/// @brief Generic substitution — mechanical AST rewriting.
///
/// See Generic.hpp for the split between substitution and instantiation.
/// This file implements only substitution: given a GenericSubstitution
/// and a SubstitutionContext, produce a copy of the input AST with every
/// occurrence of a generic parameter replaced by its concrete type.
///
/// No caches, no arity validation, no shell/finalize — those live in
/// Instantiation.cpp. Substitution is a pure tree-rewriting pass.

#include "Generic.hpp"
#include "core/trace/Trace.hpp"
#include "sema/types/SemaType.hpp"

namespace sema {

// ─── GenericSubstitution ──────────────────────────────────────────────
// (These stay here because they're small and closely tied to substitution.)

bool GenericSubstitution::isParam(InternedString name) const {
    for (GenericParamDeclAST* param : genericParams) {
        if (param->name == name) return true;
    }
    return false;
}

TypeAST* GenericSubstitution::lookup(InternedString name) const {
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i]->name == name) {
            return i < typeArgs.size() ? typeArgs[i] : nullptr;
        }
    }
    return nullptr;
}

// ─── substituteType ───────────────────────────────────────────────────

TypeAST* substituteType(TypeAST* type, SubstitutionContext& sc) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return type;

        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();

            if (sc.subst.isParam(named->name)) {
                TypeAST* result = sc.subst.lookup(named->name);
                if (result) {
                    // Substituting a generic parameter yields the concrete
                    // argument. We do NOT recurse into substituteType(result)
                    // here — the argument is already concrete (or a nested
                    // generic parameter in a re-substitution, but that would
                    // have been resolved by an outer substitution pass). If
                    // it's another parameter, we still return it as-is; the
                    // caller's subsequent resolution will see it correctly.
                    //
                    // (The previous `substituteType(result, sc)` was the
                    // source of the infinite loop on T → T.)
                    return result;
                }
                return type;
            }

            // Always build a fresh node, even if no type argument changed.
            // Sharing with the template would let resolution write into the
            // template's AST.
            auto subArgs = sc.sema.arena.makeSpan<TypeAST*>(
                named->genericArgs,
                [&](TypeAST* arg) -> TypeAST* {
                    return substituteType(arg, sc);
                }
            );

            NamedTypeAST* newNamed = sc.sema.arena.make<NamedTypeAST>(named->name);
            newNamed->genericArgs = subArgs;
            newNamed->loc = named->loc;
            // resolvedDecl left null; will be set by re-resolution.
            return newNamed;
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            TypeAST* subElement = substituteType(arr->element, sc);
            if (subElement != arr->element) {
                return sc.sema.getArrayType(arr->arrayKind, arr->size, subElement);
            }
            return type;
        }

        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            TypeAST* subInner = substituteType(nullable->inner, sc);
            return sc.sema.arena.make<NullableTypeAST>(subInner);
        }

        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            TypeAST* subInner = substituteType(fallible->inner, sc);
            return sc.sema.arena.make<FallibleTypeAST>(subInner);
        }

        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            TypeAST* subInner = substituteType(combined->inner, sc);
            return sc.sema.arena.make<CombinedTypeAST>(subInner);
        }

        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            TypeAST* subInner = substituteType(ref->inner, sc);
            return sc.sema.arena.make<RefTypeAST>(subInner);
        }

        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            TypeAST* subInner = substituteType(ptr->inner, sc);
            return sc.sema.arena.make<PtrTypeAST>(subInner);
        }

        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();

            auto subParams = sc.sema.arena.makeSpan<ParamAST*>(
                func->params,
                [&](ParamAST* param) -> ParamAST* {
                    TypeAST* subType = param->type ? substituteType(param->type, sc) : nullptr;
                    ParamAST* newParam = sc.sema.arena.make<ParamAST>(
                        param->name, subType, param->isVariadic, param->isConstParam);
                    newParam->loc = param->loc;
                    return newParam;
                }
            );

            TypeAST* subReturn = func->returnType
                ? substituteType(func->returnType, sc)
                : nullptr;

            FuncTypeAST* newFunc = sc.sema.arena.make<FuncTypeAST>();
            newFunc->params = subParams;
            newFunc->returnType = subReturn;
            newFunc->loc = func->loc;
            return newFunc;
        }

        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            TypeAST* subInner = substituteType(future->inner, sc);
            return sc.sema.arena.make<FutureTypeAST>(subInner);
        }

        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            TypeAST* subInner = substituteType(thread->inner, sc);
            return sc.sema.arena.make<ThreadTypeAST>(subInner);
        }

        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            TypeAST* subElement = substituteType(simd->elementType, sc);
            return sc.sema.arena.make<SimdTypeAST>(subElement, simd->laneCount);
        }

        case ASTKind::ArenaType:
        case ASTKind::ArenaDescriptorType:
        case ASTKind::ModuleTypeAccess:
            return type;

        default:
            return type;
    }
}

// ─── substituteStmt ───────────────────────────────────────────────────

StmtAST* substituteStmt(StmtAST* stmt, SubstitutionContext& sc) {
    if (!stmt) return nullptr;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            BlockStmtAST* block = stmt->as<BlockStmtAST>();
            BlockStmtAST* newBlock = sc.sema.arena.make<BlockStmtAST>();

            newBlock->stmts = sc.sema.arena.makeSpan<StmtAST*>(
                block->stmts,
                [&](StmtAST* s) -> StmtAST* {
                    return substituteStmt(s, sc);
                }
            );

            // scopeExits is populated during body resolution by
            // validateScopeExit. The template body was not resolved, so
            // the specialized copy accumulates its own list during the
            // post-substitution resolution pass.
            return newBlock;
        }

        case ASTKind::ReturnStmt: {
            ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            ReturnStmtAST* newRet = sc.sema.arena.make<ReturnStmtAST>();
            if (ret->value) {
                newRet->value = substituteExpr(ret->value, sc);
            }
            newRet->loc = ret->loc;
            return newRet;
        }

        case ASTKind::ExprStmt: {
            ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
            ExprStmtAST* newExprStmt = sc.sema.arena.make<ExprStmtAST>(
                substituteExpr(exprStmt->expr, sc)
            );
            newExprStmt->loc = exprStmt->loc;
            return newExprStmt;
        }

        case ASTKind::DeclStmt: {
            DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            DeclAST* newDecl = substituteDecl(declStmt->decl, sc);
            DeclStmtAST* newDeclStmt = sc.sema.arena.make<DeclStmtAST>(newDecl);
            newDeclStmt->loc = declStmt->loc;
            return newDeclStmt;
        }

        case ASTKind::IfStmt: {
            IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            IfStmtAST* newIf = sc.sema.arena.make<IfStmtAST>();
            newIf->condition = substituteExpr(ifStmt->condition, sc);
            newIf->thenBranch = substituteStmt(ifStmt->thenBranch, sc);
            newIf->elseBranch = ifStmt->elseBranch
                ? substituteStmt(ifStmt->elseBranch, sc)
                : nullptr;
            newIf->loc = ifStmt->loc;
            return newIf;
        }

        case ASTKind::WhileStmt: {
            WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            WhileStmtAST* newWhile = sc.sema.arena.make<WhileStmtAST>();
            newWhile->condition = substituteExpr(whileStmt->condition, sc);
            newWhile->body = substituteStmt(whileStmt->body, sc);
            newWhile->loc = whileStmt->loc;
            return newWhile;
        }

        case ASTKind::DoWhileStmt: {
            DoWhileStmtAST* doWhileStmt = stmt->as<DoWhileStmtAST>();
            DoWhileStmtAST* newDoWhile = sc.sema.arena.make<DoWhileStmtAST>();
            newDoWhile->body = substituteStmt(doWhileStmt->body, sc);
            newDoWhile->condition = substituteExpr(doWhileStmt->condition, sc);
            newDoWhile->loc = doWhileStmt->loc;
            return newDoWhile;
        }

        case ASTKind::ForStmt: {
            ForStmtAST* forStmt = stmt->as<ForStmtAST>();
            ForStmtAST* newFor = sc.sema.arena.make<ForStmtAST>();
            newFor->indexVar = forStmt->indexVar;
            newFor->valueVar = forStmt->valueVar;
            newFor->iterable = substituteExpr(forStmt->iterable, sc);
            newFor->step = forStmt->step ? substituteExpr(forStmt->step, sc) : nullptr;
            newFor->body = substituteStmt(forStmt->body, sc);
            newFor->loc = forStmt->loc;
            return newFor;
        }

        case ASTKind::SwitchStmt: {
            SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
            SwitchStmtAST* newSwitch = sc.sema.arena.make<SwitchStmtAST>();
            newSwitch->subject = substituteExpr(switchStmt->subject, sc);

            newSwitch->cases = sc.sema.arena.makeSpan<SwitchCaseAST*>(
                switchStmt->cases,
                [&](SwitchCaseAST* caseNode) -> SwitchCaseAST* {
                    SwitchCaseAST* newCase = sc.sema.arena.make<SwitchCaseAST>();
                    newCase->values = sc.sema.arena.makeSpan<ExprAST*>(
                        caseNode->values,
                        [&](ExprAST* val) -> ExprAST* {
                            return substituteExpr(val, sc);
                        }
                    );
                    newCase->body = substituteStmt(caseNode->body, sc)->as<BlockStmtAST>();
                    newCase->loc = caseNode->loc;
                    return newCase;
                }
            );

            newSwitch->defaultBody = switchStmt->defaultBody
                ? substituteStmt(switchStmt->defaultBody, sc)->as<BlockStmtAST>()
                : nullptr;
            newSwitch->defaultLoc = switchStmt->defaultLoc;
            newSwitch->loc = switchStmt->loc;
            return newSwitch;
        }

        case ASTKind::BreakStmt: {
            BreakStmtAST* breakStmt = stmt->as<BreakStmtAST>();
            BreakStmtAST* newBreakStmt = sc.sema.arena.make<BreakStmtAST>();
            newBreakStmt->loc = breakStmt->loc;
            return newBreakStmt;
        }

        case ASTKind::ContinueStmt: {
            ContinueStmtAST* continueStmt = stmt->as<ContinueStmtAST>();
            ContinueStmtAST* newContinueStmt = sc.sema.arena.make<ContinueStmtAST>();
            newContinueStmt->loc = continueStmt->loc;
            return newContinueStmt;
        }

        default:
            return stmt;
    }
}

// ─── substituteExpr ───────────────────────────────────────────────────

ExprAST* substituteExpr(ExprAST* expr, SubstitutionContext& sc) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr: {
            LiteralExprAST* lit = expr->as<LiteralExprAST>();
            LiteralExprAST* newLit = sc.sema.arena.make<LiteralExprAST>(lit->kind, lit->value);
            newLit->loc = lit->loc;
            return newLit;
        }

        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();

            // Always allocate a fresh identifier node. Resolution writes
            // semantic state onto each identifier it visits, and sharing a
            // template node would leak that state back into the template.
            auto subArgs = sc.sema.arena.makeSpan<TypeAST*>(
                id->genericArgs,
                [&](TypeAST* arg) -> TypeAST* {
                    return substituteType(arg, sc);
                }
            );

            IdentifierExprAST* newId =
                sc.sema.arena.make<IdentifierExprAST>(id->name);
            newId->genericArgs = subArgs;
            newId->isType = id->isType;
            newId->loc = id->loc;

            // Semantic fields (resolvedDecl, resolvedType, valueState,
            // isConst, isLValue, isImplicitFieldAccess, selfObject,
            // fieldIndex, resolvedTypeNode) are deliberately left unset.
            // The specialized body has not been resolved yet; resolution
            // runs after substitution, and the nodes must be re-bound from
            // scratch in that pass.

            return newId;
        }

        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            BinaryExprAST* newBin = sc.sema.arena.make<BinaryExprAST>(bin->op);
            newBin->left = substituteExpr(bin->left, sc);
            newBin->right = substituteExpr(bin->right, sc);
            newBin->loc = bin->loc;
            return newBin;
        }

        case ASTKind::UnaryExpr: {
            UnaryExprAST* unary = expr->as<UnaryExprAST>();
            UnaryExprAST* newUnary = sc.sema.arena.make<UnaryExprAST>(unary->op);
            newUnary->operand = substituteExpr(unary->operand, sc);
            newUnary->loc = unary->loc;
            return newUnary;
        }

        case ASTKind::CallExpr: {
            CallExprAST* call = expr->as<CallExprAST>();
            CallExprAST* newCall = sc.sema.arena.make<CallExprAST>(call->hasArgPack);
            newCall->callee = substituteExpr(call->callee, sc);
            newCall->args = sc.sema.arena.makeSpan<ExprAST*>(
                call->args,
                [&](ExprAST* arg) -> ExprAST* {
                    return substituteExpr(arg, sc);
                }
            );
            newCall->loc = call->loc;
            return newCall;
        }

        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            FieldAccessExprAST* newField = sc.sema.arena.make<FieldAccessExprAST>(field->fieldName);
            newField->object = substituteExpr(field->object, sc);
            newField->loc = field->loc;

            // Semantic fields (resolvedDecl, ownerType, isEnumAccess,
            // fieldIndex) are left unset so the specialized copy resolves
            // fresh during the body-resolution pass.
            return newField;
        }

        case ASTKind::ArrayLiteralExpr: {
            ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            auto subElements = sc.sema.arena.makeSpan<ExprAST*>(
                arr->elements,
                [&](ExprAST* elem) -> ExprAST* {
                    return substituteExpr(elem, sc);
                }
            );
            ArrayLiteralExprAST* newArr = sc.sema.arena.make<ArrayLiteralExprAST>(subElements);
            newArr->loc = arr->loc;
            return newArr;
        }

        case ASTKind::StructLiteralExpr: {
            StructLiteralExprAST* structExpr = expr->as<StructLiteralExprAST>();
            auto subInits = sc.sema.arena.makeSpan<FieldInitAST*>(
                structExpr->inits,
                [&](FieldInitAST* init) -> FieldInitAST* {
                    FieldInitAST* newInit = sc.sema.arena.make<FieldInitAST>(
                        init->name,
                        substituteExpr(init->value, sc)
                    );
                    newInit->loc = init->loc;
                    return newInit;
                }
            );

            auto subGenericArgs = sc.sema.arena.makeSpan<TypeAST*>(
                structExpr->genericArgs,
                [&](TypeAST* arg) -> TypeAST* {
                    return substituteType(arg, sc);
                }
            );

            StructLiteralExprAST* newStruct = sc.sema.arena.make<StructLiteralExprAST>(
                structExpr->typeName,
                subGenericArgs,
                subInits
            );
            newStruct->loc = structExpr->loc;

            // resolvedDecl is left unset; the specialized struct literal
            // resolves from scratch during the body-resolution pass.
            return newStruct;
        }

        case ASTKind::IndexExpr: {
            IndexExprAST* index = expr->as<IndexExprAST>();
            IndexExprAST* newIndex = sc.sema.arena.make<IndexExprAST>(
                substituteExpr(index->target, sc),
                substituteExpr(index->index, sc)
            );
            newIndex->loc = index->loc;
            return newIndex;
        }

        case ASTKind::SliceExpr: {
            SliceExprAST* slice = expr->as<SliceExprAST>();
            SliceExprAST* newSlice = sc.sema.arena.make<SliceExprAST>(
                substituteExpr(slice->target, sc),
                slice->start ? substituteExpr(slice->start, sc) : nullptr,
                slice->end ? substituteExpr(slice->end, sc) : nullptr,
                slice->isExclusive
            );
            newSlice->loc = slice->loc;
            return newSlice;
        }

        case ASTKind::NullCoalesceExpr: {
            NullCoalesceExprAST* coalesce = expr->as<NullCoalesceExprAST>();
            NullCoalesceExprAST* newCoalesce = sc.sema.arena.make<NullCoalesceExprAST>(
                substituteExpr(coalesce->value, sc),
                substituteExpr(coalesce->fallback, sc)
            );
            newCoalesce->loc = coalesce->loc;
            return newCoalesce;
        }

        case ASTKind::AssignExpr: {
            AssignExprAST* assign = expr->as<AssignExprAST>();
            AssignExprAST* newAssign = sc.sema.arena.make<AssignExprAST>(assign->op);
            newAssign->lhs = substituteExpr(assign->lhs, sc);
            newAssign->rhs = substituteExpr(assign->rhs, sc);
            newAssign->loc = assign->loc;
            return newAssign;
        }

        case ASTKind::ModuleAccessExpr: {
            ModuleAccessExprAST* mod = expr->as<ModuleAccessExprAST>();
            ModuleAccessExprAST* newMod = sc.sema.arena.make<ModuleAccessExprAST>(
                mod->moduleName, mod->memberName
            );

            if (!mod->genericArgs.empty()) {
                newMod->genericArgs = sc.sema.arena.makeSpan<TypeAST*>(
                    mod->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        return substituteType(arg, sc);
                    }
                );
            }

            newMod->loc = mod->loc;
            return newMod;
        }

        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* anon = expr->as<AnonFuncExprAST>();
            TypeAST* subFuncType = substituteType(anon->funcType, sc);

            AnonFuncExprAST* newAnon = sc.sema.arena.make<AnonFuncExprAST>(
                subFuncType ? subFuncType->as<FuncTypeAST>() : nullptr,
                nullptr
            );
            newAnon->loc = anon->loc;
            newAnon->body = substituteStmt(anon->body, sc);
            return newAnon;
        }

        case ASTKind::IfExpr: {
            IfExprAST* ifExpr = expr->as<IfExprAST>();
            IfExprAST* newIf = sc.sema.arena.make<IfExprAST>(
                substituteExpr(ifExpr->condition, sc),
                substituteExpr(ifExpr->thenBranch, sc),
                substituteExpr(ifExpr->elseBranch, sc)
            );
            newIf->loc = ifExpr->loc;
            return newIf;
        }

        case ASTKind::RangeExpr: {
            RangeExprAST* range = expr->as<RangeExprAST>();
            RangeExprAST* newRange = sc.sema.arena.make<RangeExprAST>(range->isExclusive);
            newRange->lo = substituteExpr(range->lo, sc);
            newRange->hi = substituteExpr(range->hi, sc);
            newRange->loc = range->loc;
            return newRange;
        }

        case ASTKind::PipelineExpr: {
            PipelineExprAST* pipe = expr->as<PipelineExprAST>();
            auto subSteps = sc.sema.arena.makeSpan<PipelineStepAST*>(
                pipe->steps,
                [&](PipelineStepAST* step) -> PipelineStepAST* {
                    auto subPackArgs = sc.sema.arena.makeSpan<ExprAST*>(
                        step->packArgs,
                        [&](ExprAST* arg) -> ExprAST* {
                            return substituteExpr(arg, sc);
                        }
                    );
                    PipelineStepAST* newStep = sc.sema.arena.make<PipelineStepAST>(
                        substituteExpr(step->callable, sc),
                        subPackArgs
                    );
                    newStep->loc = step->loc;
                    return newStep;
                }
            );
            PipelineExprAST* newPipe = sc.sema.arena.make<PipelineExprAST>(
                substituteExpr(pipe->seed, sc),
                subSteps
            );
            newPipe->loc = pipe->loc;
            return newPipe;
        }

        case ASTKind::IntrinsicCallExpr: {
            IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
            IntrinsicCallExprAST* newIntrinsic = sc.sema.arena.make<IntrinsicCallExprAST>(
                intrinsic->intrinsicName
            );
            if (!intrinsic->args.empty()) {
                newIntrinsic->args = sc.sema.arena.makeSpan<ExprAST*>(
                    intrinsic->args,
                    [&](ExprAST* arg) -> ExprAST* {
                        return substituteExpr(arg, sc);
                    }
                );
            }
            newIntrinsic->loc = intrinsic->loc;
            return newIntrinsic;
        }

        case ASTKind::ArenaAccessExpr: {
            ArenaAccessExprAST* arenaAccess = expr->as<ArenaAccessExprAST>();
            ArenaAccessExprAST* newArenaAccess = sc.sema.arena.make<ArenaAccessExprAST>(
                arenaAccess->methodName,
                arenaAccess->isStatic,
                arenaAccess->arenaExpr ? substituteExpr(arenaAccess->arenaExpr, sc) : nullptr
            );
            if (!arenaAccess->genericArgs.empty()) {
                newArenaAccess->genericArgs = sc.sema.arena.makeSpan<TypeAST*>(
                    arenaAccess->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        return substituteType(arg, sc);
                    }
                );
            }
            if (!arenaAccess->args.empty()) {
                newArenaAccess->args = sc.sema.arena.makeSpan<ExprAST*>(
                    arenaAccess->args,
                    [&](ExprAST* arg) -> ExprAST* {
                        return substituteExpr(arg, sc);
                    }
                );
            }
            newArenaAccess->loc = arenaAccess->loc;
            return newArenaAccess;
        }

        default:
            return expr;
    }
}

// ─── substituteDecl ───────────────────────────────────────────────────

DeclAST* substituteDecl(DeclAST* decl, SubstitutionContext& sc) {
    if (!decl) return nullptr;

    switch (decl->kind) {
        // ─── VarDeclAST ────────────────────────────────────────────────
        //
        // The common case: `let x T = ...` or `const x T = ...` inside a
        // generic body. Both `type` and `init` may reference `T`.
        //
        // The keyword is preserved (`let` stays `let`, `const` stays
        // `const`) — substitution never changes mutability. The name
        // stays the same. Only the type and the initializer change.
        case ASTKind::VarDecl: {
            VarDeclAST* var = decl->as<VarDeclAST>();

            TypeAST* newType = var->type
                ? substituteType(var->type, sc)
                : nullptr;

            ExprAST* newInit = var->init
                ? substituteExpr(var->init, sc)
                : nullptr;

            VarDeclAST* newVar = sc.sema.arena.make<VarDeclAST>(
                var->name,
                var->keyword,
                newType,
                newInit
            );
            newVar->loc = var->loc;
            // attributes are copied verbatim — attributes are not
            // types and never reference generic parameters.
            newVar->attributes = var->attributes;
            return newVar;
        }

        // ─── FuncDeclAST ───────────────────────────────────────────────
        //
        // A local function declaration inside a generic body. Its
        // signature and initializer may reference `T`. Local functions
        // cannot have their *own* generic parameters — that would be a
        // second level of genericity nested inside the first, and the
        // grammar and Sema already reject it.
        //
        // Note: we substitute `funcType` and `init` but do NOT re-resolve
        // them here. Resolution of the specialized declaration happens
        // when the enclosing body's resolution walk reaches this node,
        // through the same machinery as any other local declaration.
        case ASTKind::FuncDecl: {
            FuncDeclAST* func = decl->as<FuncDeclAST>();

            TypeAST* newFuncType = func->funcType
                ? substituteType(func->funcType, sc)
                : nullptr;

            ExprAST* newInit = func->init
                ? substituteExpr(func->init, sc)
                : nullptr;

            FuncDeclAST* newFunc = sc.sema.arena.make<FuncDeclAST>(
                func->name,
                func->keyword,
                sc.sema.arena.emptySpan<GenericParamDeclAST*>(),  // local funcs are not generic
                newFuncType ? newFuncType->as<FuncTypeAST>() : nullptr,
                newInit
            );
            newFunc->loc = func->loc;
            newFunc->attributes = func->attributes;
            newFunc->isForeignFunction = func->isForeignFunction;
            newFunc->isInline = func->isInline;
            newFunc->isNoInline = func->isNoInline;
            return newFunc;
        }

        // ─── StructDeclAST ─────────────────────────────────────────────
        //
        // A local struct declaration. Structs are a compile-time-only
        // construct — they produce no runtime value — but a *local*
        // struct that mentions the enclosing function's `T` in a field
        // type must have that field type substituted.
        //
        // A local struct's own generic parameters are independent of the
        // enclosing function's, so any `T` in this struct's `genericParams`
        // is a *different* `T` and must not be substituted by the
        // enclosing substitution. That's why we only substitute field
        // types whose generic parameters are the enclosing function's —
        // i.e., any field type that references a name not bound by this
        // struct's own parameter list.
        //
        // The cleanest way to express this without shadowing analysis is
        // to check, per field type, whether the substitution applies. The
        // `GenericSubstitution` only knows about the enclosing function's
        // parameters; if the local struct declares a parameter with the
        // same name, the substitution's `isParam` will still return true
        // and we'd wrongly substitute. To avoid that, we skip substitution
        // entirely for a local generic struct whose parameter list shadows
        // any of the enclosing function's parameters.
        //
        // (This is a rare case; in practice local structs inside generic
        //  functions don't have their own generic parameters.)
        case ASTKind::StructDecl: {
            StructDeclAST* strct = decl->as<StructDeclAST>();

            // If the local struct has its own generic parameters that
            // shadow the enclosing function's, skip substitution on its
            // fields. Its fields reference its own parameters, not the
            // enclosing function's.
            bool hasShadowingParam = false;
            for (GenericParamDeclAST* param : strct->genericParams) {
                if (sc.subst.isParam(param->name)) {
                    hasShadowingParam = true;
                    break;
                }
            }
            if (hasShadowingParam) {
                // Reuse the original node unchanged. A correct
                // implementation would substitute only the fields that
                // reference the enclosing function's parameters, but that
                // requires shadowing-aware type substitution, which is
                // beyond what this pass does today.
                return strct;
            }

            auto subFields = sc.sema.arena.makeSpan<FieldDeclAST*>(
                strct->fields,
                [&](FieldDeclAST* field) -> FieldDeclAST* {
                    TypeAST* newFieldType = field->type
                        ? substituteType(field->type, sc)
                        : nullptr;
                    ExprAST* newDefault = field->defaultVal
                        ? substituteExpr(field->defaultVal, sc)
                        : nullptr;
                    FieldDeclAST* newField = sc.sema.arena.make<FieldDeclAST>(
                        field->name,
                        newFieldType,
                        newDefault,
                        field->isConstField
                    );
                    newField->loc = field->loc;
                    newField->attributes = field->attributes;
                    return newField;
                }
            );

            StructDeclAST* newStruct = sc.sema.arena.make<StructDeclAST>(
                strct->name,
                strct->genericParams,      // local struct's own params, unchanged
                subFields,
                strct->traitRefs,
                strct->isPacked
            );
            newStruct->loc = strct->loc;
            newStruct->attributes = strct->attributes;
            return newStruct;
        }

        // ─── EnumDeclAST ───────────────────────────────────────────────
        //
        // Enums have no fields with generic types in Lucid (variants are
        // integer-valued). But a local enum's backing type is a
        // PrimitiveTypeAST and can't reference `T`. The only thing that
        // could reference `T` is... nothing. A local enum is
        // type-substitution-neutral in the current language.
        //
        // So we could return `decl` unchanged. For uniformity with the
        // other cases, allocate a fresh node so the specialized body
        // doesn't share the template's enum node. The enum's variant
        // list is not mutated by resolution (variants have no semantic
        // fields beyond the ones set at parse time), so sharing the
        // variant nodes would be safe — but a fresh enum node keeps the
        // invariant simple.
        case ASTKind::EnumDecl: {
            EnumDeclAST* enm = decl->as<EnumDeclAST>();
            EnumDeclAST* newEnum = sc.sema.arena.make<EnumDeclAST>(
                enm->name,
                enm->variants,
                enm->backingType
            );
            newEnum->loc = enm->loc;
            newEnum->attributes = enm->attributes;
            return newEnum;
        }

        // ─── TraitDeclAST ──────────────────────────────────────────────
        //
        // Same reasoning as enum: a trait is a set of field *contracts*,
        // not actual fields. The field types in a trait may mention the
        // trait's own generic parameters, and those are independent of
        // any enclosing function's. A local trait referenced from inside
        // a generic function's body is unusual, but if it happens, its
        // field types are expressed in terms of the trait's own
        // parameters, not the enclosing function's.
        //
        // Return unchanged.
        case ASTKind::TraitDecl: {
            TraitDeclAST* trait = decl->as<TraitDeclAST>();
            TraitDeclAST* newTrait = sc.sema.arena.make<TraitDeclAST>(
                trait->name,
                trait->genericParams,
                trait->fields
            );
            newTrait->loc = trait->loc;
            newTrait->attributes = trait->attributes;
            return newTrait;
        }

        // ─── Other kinds ───────────────────────────────────────────────
        //
        // ImportDeclAST cannot appear inside a function body. Any other
        // kind reaching here is either already handled by its own
        // substituter (`VarDeclAST` and `FuncDeclAST` above) or cannot
        // reference generic parameters. Return unchanged.
        default:
            return decl;
    }
}

// ─── containsGenericParams ────────────────────────────────────────────

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
        case ASTKind::ArrayType:
            return containsGenericParams(type->as<ArrayTypeAST>()->element, subst);
        case ASTKind::NullableType:
            return containsGenericParams(type->as<NullableTypeAST>()->inner, subst);
        case ASTKind::FallibleType:
            return containsGenericParams(type->as<FallibleTypeAST>()->inner, subst);
        case ASTKind::CombinedType:
            return containsGenericParams(type->as<CombinedTypeAST>()->inner, subst);
        case ASTKind::RefType:
            return containsGenericParams(type->as<RefTypeAST>()->inner, subst);
        case ASTKind::PtrType:
            return containsGenericParams(type->as<PtrTypeAST>()->inner, subst);
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
        case ASTKind::FutureType:
            return containsGenericParams(type->as<FutureTypeAST>()->inner, subst);
        case ASTKind::ThreadType:
            return containsGenericParams(type->as<ThreadTypeAST>()->inner, subst);
        case ASTKind::SimdType:
            return containsGenericParams(type->as<SimdTypeAST>()->elementType, subst);
        default:
            return false;
    }
}

} // namespace sema