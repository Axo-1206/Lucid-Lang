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

            // ─── Check if this is a generic parameter ──────────────────
            if (sc.subst.isParam(named->name)) {
                TypeAST* result = sc.subst.lookup(named->name);
                if (result) {
                    return substituteType(result, sc);
                }
                return type;
            }

            // ─── Check if this is a generic struct with args ──────────
            if (!named->genericArgs.empty()) {
                bool changed = false;

                auto subArgs = sc.sema.arena.makeSpan<TypeAST*>(
                    named->genericArgs,
                    [&](TypeAST* arg) -> TypeAST* {
                        TypeAST* subArg = substituteType(arg, sc);
                        if (subArg != arg) changed = true;
                        return subArg;
                    }
                );

                if (changed) {
                    NamedTypeAST* newNamed = sc.sema.arena.make<NamedTypeAST>(named->name);
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
            TypeAST* subElement = substituteType(arr->element, sc);
            if (subElement != arr->element) {
                return sc.sema.getArrayType(arr->arrayKind, arr->size, subElement);
            }
            return type;
        }

        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            TypeAST* subInner = substituteType(nullable->inner, sc);
            if (subInner != nullable->inner) {
                return sc.sema.arena.make<NullableTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            TypeAST* subInner = substituteType(fallible->inner, sc);
            if (subInner != fallible->inner) {
                return sc.sema.arena.make<FallibleTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            TypeAST* subInner = substituteType(combined->inner, sc);
            if (subInner != combined->inner) {
                return sc.sema.arena.make<CombinedTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            TypeAST* subInner = substituteType(ref->inner, sc);
            if (subInner != ref->inner) {
                return sc.sema.arena.make<RefTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            TypeAST* subInner = substituteType(ptr->inner, sc);
            if (subInner != ptr->inner) {
                return sc.sema.arena.make<PtrTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();

            bool paramsChanged = false;
            auto subParams = sc.sema.arena.makeSpan<ParamAST*>(
                func->params,
                [&](ParamAST* param) -> ParamAST* {
                    if (!param->type) return param;
                    TypeAST* subType = substituteType(param->type, sc);
                    if (subType != param->type) {
                        paramsChanged = true;
                        ParamAST* newParam = sc.sema.arena.make<ParamAST>(
                            param->name, subType, param->isVariadic, param->isConstParam);
                        newParam->loc = param->loc;
                        return newParam;
                    }
                    return param;
                }
            );

            TypeAST* subReturn = func->returnType
                ? substituteType(func->returnType, sc)
                : nullptr;

            if (paramsChanged || subReturn != func->returnType) {
                FuncTypeAST* newFunc = sc.sema.arena.make<FuncTypeAST>();
                newFunc->params = subParams;
                newFunc->returnType = subReturn;
                newFunc->loc = func->loc;
                return newFunc;
            }
            return type;
        }

        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            TypeAST* subInner = substituteType(future->inner, sc);
            if (subInner != future->inner) {
                return sc.sema.arena.make<FutureTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            TypeAST* subInner = substituteType(thread->inner, sc);
            if (subInner != thread->inner) {
                return sc.sema.arena.make<ThreadTypeAST>(subInner);
            }
            return type;
        }

        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            TypeAST* subElement = substituteType(simd->elementType, sc);
            if (subElement != simd->elementType) {
                return sc.sema.arena.make<SimdTypeAST>(subElement, simd->laneCount);
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

            newBlock->scopeExits = block->scopeExits;
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
            // TODO: substitute types inside the declaration.
            return stmt;
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

        case ASTKind::BreakStmt:
        case ASTKind::ContinueStmt:
            return stmt;

        default:
            return stmt;
    }
}

// ─── substituteExpr ───────────────────────────────────────────────────

ExprAST* substituteExpr(ExprAST* expr, SubstitutionContext& sc) {
    if (!expr) return nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return expr;

        case ASTKind::IdentifierExpr: {
            // Generic function references are resolved by Sema; nothing
            // to substitute on a plain identifier.
            return expr;
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
            newField->resolvedDecl = field->resolvedDecl;
            newField->ownerType = field->ownerType;
            newField->isEnumAccess = field->isEnumAccess;
            newField->fieldIndex = field->fieldIndex;
            newField->loc = field->loc;
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
            StructLiteralExprAST* newStruct = sc.sema.arena.make<StructLiteralExprAST>(
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

            newMod->resolvedDecl = mod->resolvedDecl;
            newMod->loc = mod->loc;
            return newMod;
        }

        case ASTKind::AnonFuncExpr: {
            AnonFuncExprAST* anon = expr->as<AnonFuncExprAST>();
            TypeAST* subFuncType = substituteType(anon->funcType, sc);

            // ─── Construct newAnon without body first ──────────────────
            // We need `newAnon` to exist before recursing into its body,
            // because the body walk needs `newAnon` as the current
            // enclosing function (for nested closures' enclosingFunction).
            AnonFuncExprAST* newAnon = sc.sema.arena.make<AnonFuncExprAST>(
                subFuncType ? subFuncType->as<FuncTypeAST>() : nullptr,
                nullptr   // body, filled below
            );

            // ─── Captures are lexically invariant ──────────────────────
            // (name, functionDepth) survive substitution unchanged.
            newAnon->captures   = anon->captures;
            newAnon->hasClosure = anon->hasClosure;
            newAnon->isReturned = anon->isReturned;
            newAnon->loc        = anon->loc;

            // ─── Re-derive enclosingFunction in the specialized context ─
            // The template's node points at the template's enclosing
            // anon; that node isn't in the specialized tree. The correct
            // parent is whatever substitution is currently inside —
            // sc.enclosingFunction.
            newAnon->enclosingFunction = sc.enclosingFunction;

            // ─── Walk the body with newAnon as current enclosing ───────
            AnonFuncExprAST* prevEnclosing = sc.enclosingFunction;
            sc.enclosingFunction = newAnon;
            newAnon->body = substituteStmt(anon->body, sc);
            sc.enclosingFunction = prevEnclosing;

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

        case ASTKind::ComposeExpr: {
            ComposeExprAST* compose = expr->as<ComposeExprAST>();
            auto subOperands = sc.sema.arena.makeSpan<ComposeOperandAST*>(
                compose->operands,
                [&](ComposeOperandAST* op) -> ComposeOperandAST* {
                    auto subGenericArgs = sc.sema.arena.makeSpan<TypeAST*>(
                        op->genericArgs,
                        [&](TypeAST* arg) -> TypeAST* {
                            return substituteType(arg, sc);
                        }
                    );
                    ComposeOperandAST* newOp = sc.sema.arena.make<ComposeOperandAST>(
                        substituteExpr(op->callable, sc),
                        subGenericArgs
                    );
                    newOp->loc = op->loc;
                    return newOp;
                }
            );
            ComposeExprAST* newCompose = sc.sema.arena.make<ComposeExprAST>(
                substituteExpr(compose->left, sc),
                subOperands
            );
            newCompose->loc = compose->loc;
            return newCompose;
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
            newIntrinsic->intrinsicID = intrinsic->intrinsicID;
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
            newArenaAccess->resolvedDecl = arenaAccess->resolvedDecl;
            newArenaAccess->loc = arenaAccess->loc;
            return newArenaAccess;
        }

        default:
            return expr;
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