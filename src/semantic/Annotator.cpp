/**
 * @file Annotator.cpp
 *
 * @responsibility Phase 4 of semantic analysis: stamps compile‑time constant flags
 *   (`isConst`) and method‑call flags (`isBehaviorMember`) onto the AST using
 *   bottom‑up, post‑order traversal.
 *
 * @design
 *   - Uses a single function `annotateAll()` that dispatches on ASTKind.
 *   - No visitor classes, no virtual calls – just a big switch and recursion.
 *   - All `isConst` flags are computed from children; constant propagation is
 *     purely bottom‑up.
 *   - `isBehaviorMember` is set to `true` on every `BehaviorAccessExprAST`.
 *   - The annotator does **not** set `resolvedType` or `scopeDepth` – those are
 *     written during Phase 3.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Forward declarations for recursive helpers (mutually recursive)
// ============================================================================
static void annotateDecl(DeclAST* decl, SemanticContext& ctx);
static void annotateStmt(StmtAST* stmt, SemanticContext& ctx);
static void annotateExpr(ExprAST* expr, SemanticContext& ctx);
static void annotateType(TypeAST* type, SemanticContext& ctx);
static void annotatePattern(PatternAST* pattern, SemanticContext& ctx);
static void annotateArm(MatchArmAST* arm, SemanticContext& ctx);
static void annotateDefaultArm(DefaultArmAST* arm, SemanticContext& ctx);
static void annotateFieldPattern(FieldPatternAST* fp, SemanticContext& ctx);
static void annotateOkArm(OkArmAST* arm, SemanticContext& ctx);
static void annotateErrArm(ErrArmAST* arm, SemanticContext& ctx);
static void annotateSwitchCase(SwitchCaseAST* cas, SemanticContext& ctx);

// ============================================================================
// Expression annotation (bottom‑up)
// ============================================================================
static void annotateExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::LiteralExpr: {
            auto* lit = static_cast<LiteralExprAST*>(expr);
            lit->isConst = (lit->kind != LiteralKind::Nil);
            break;
        }
        case ASTKind::IdentifierExpr: {
            auto* id = static_cast<IdentifierExprAST*>(expr);
            Symbol* sym = ctx.symbols->lookup(id->name);
            id->isConst = (sym && sym->declKw == DeclKeyword::Const);
            break;
        }
        case ASTKind::ArrayLiteralExpr: {
            auto* arr = static_cast<ArrayLiteralExprAST*>(expr);
            bool allConst = true;
            for (auto& elem : arr->elements) {
                annotateExpr(elem.get(), ctx);
                if (!elem->isConst) allConst = false;
            }
            arr->isConst = allConst;
            break;
        }
        case ASTKind::StructLiteralExpr: {
            auto* sl = static_cast<StructLiteralExprAST*>(expr);
            bool allConst = true;
            for (auto& init : sl->inits) {
                if (init && init->value) annotateExpr(init->value.get(), ctx);
                if (!init || !init->value || !init->value->isConst) allConst = false;
            }
            sl->isConst = allConst;
            break;
        }
        case ASTKind::FieldInit: {
            auto* fi = static_cast<FieldInitAST*>(expr);
            if (fi->value) annotateExpr(fi->value.get(), ctx);
            fi->isConst = (fi->value && fi->value->isConst);
            break;
        }
        case ASTKind::FieldAccessExpr: {
            auto* fa = static_cast<FieldAccessExprAST*>(expr);
            annotateExpr(fa->object.get(), ctx);
            fa->isConst = (fa->object && fa->object->isConst);
            break;
        }
        case ASTKind::BehaviorAccessExpr: {
            auto* ba = static_cast<BehaviorAccessExprAST*>(expr);
            ba->isBehaviorMember = true;
            ba->isConst = false;
            break;
        }
        case ASTKind::BinaryExpr: {
            auto* bin = static_cast<BinaryExprAST*>(expr);
            annotateExpr(bin->left.get(), ctx);
            annotateExpr(bin->right.get(), ctx);
            bin->isConst = bin->left->isConst && bin->right->isConst;
            break;
        }
        case ASTKind::UnaryExpr: {
            auto* un = static_cast<UnaryExprAST*>(expr);
            annotateExpr(un->operand.get(), ctx);
            un->isConst = un->operand->isConst;
            break;
        }
        case ASTKind::AssignExpr: {
            auto* as = static_cast<AssignExprAST*>(expr);
            annotateExpr(as->lhs.get(), ctx);
            annotateExpr(as->rhs.get(), ctx);
            as->isConst = false;
            break;
        }
        case ASTKind::CallExpr: {
            auto* call = static_cast<CallExprAST*>(expr);
            annotateExpr(call->callee.get(), ctx);
            for (auto& arg : call->args) annotateExpr(arg.get(), ctx);
            call->isConst = false;
            break;
        }
        case ASTKind::IndexExpr: {
            auto* idx = static_cast<IndexExprAST*>(expr);
            annotateExpr(idx->target.get(), ctx);
            annotateExpr(idx->index.get(), ctx);
            if (idx->sliceEnd) annotateExpr(idx->sliceEnd.get(), ctx);
            idx->isConst = false;
            break;
        }
        case ASTKind::IsExpr: {
            auto* ise = static_cast<IsExprAST*>(expr);
            annotateExpr(ise->expr.get(), ctx);
            ise->isConst = false;
            break;
        }
        case ASTKind::NullableChainExpr: {
            auto* nc = static_cast<NullableChainExprAST*>(expr);
            annotateExpr(nc->object.get(), ctx);
            nc->isConst = false;
            break;
        }
        case ASTKind::NullCoalesceExpr: {
            auto* nc = static_cast<NullCoalesceExprAST*>(expr);
            annotateExpr(nc->value.get(), ctx);
            annotateExpr(nc->fallback.get(), ctx);
            nc->isConst = false;
            break;
        }
        case ASTKind::PipelineExpr: {
            auto* pipe = static_cast<PipelineExprAST*>(expr);
            annotateExpr(pipe->seed.get(), ctx);
            for (auto& step : pipe->steps) {
                if (!step) continue;
                for (auto& arg : step->packArgs) annotateExpr(arg.get(), ctx);
                if (step->callable) annotateExpr(step->callable.get(), ctx);
            }
            pipe->isConst = false;
            break;
        }
        case ASTKind::ComposeExpr: {
            auto* comp = static_cast<ComposeExprAST*>(expr);
            annotateExpr(comp->left.get(), ctx);
            for (auto& op : comp->operands) {
                if (op && op->callable) annotateExpr(op->callable.get(), ctx);
            }
            comp->isConst = false;
            break;
        }
        case ASTKind::AnonFuncExpr: {
            auto* af = static_cast<AnonFuncExprAST*>(expr);
            for (auto& param : af->sig.allParams) {
                if (param) {
                    param->isConst = false;
                    if (param->type) annotateType(param->type.get(), ctx);
                }
            }
            if (af->body) annotateStmt(af->body.get(), ctx);
            af->isConst = false;
            break;
        }
        case ASTKind::AwaitExpr: {
            auto* aw = static_cast<AwaitExprAST*>(expr);
            annotateExpr(aw->inner.get(), ctx);
            aw->isConst = false;
            break;
        }
        case ASTKind::ResolveExpr: {
            auto* re = static_cast<ResolveExprAST*>(expr);
            annotateExpr(re->subject.get(), ctx);
            if (re->okArm) annotateOkArm(re->okArm.get(), ctx);
            if (re->errArm) annotateErrArm(re->errArm.get(), ctx);
            re->isConst = false;
            break;
        }
        case ASTKind::MatchExpr: {
            auto* me = static_cast<MatchExprAST*>(expr);
            annotateExpr(me->subject.get(), ctx);
            for (auto& arm : me->arms) annotateArm(arm.get(), ctx);
            if (me->defaultBody) annotateDefaultArm(me->defaultBody.get(), ctx);
            me->isConst = false;
            break;
        }
        case ASTKind::IfExpr: {
            auto* ie = static_cast<IfExprAST*>(expr);
            annotateExpr(ie->condition.get(), ctx);
            annotateExpr(ie->thenBranch.get(), ctx);
            annotateExpr(ie->elseBranch.get(), ctx);
            ie->isConst = false;
            break;
        }
        case ASTKind::RangeExpr: {
            auto* re = static_cast<RangeExprAST*>(expr);
            annotateExpr(re->lo.get(), ctx);
            annotateExpr(re->hi.get(), ctx);
            re->isConst = re->lo->isConst && re->hi->isConst;
            break;
        }
        case ASTKind::TypeConvExpr: {
            auto* tc = static_cast<TypeConvExprAST*>(expr);
            annotateExpr(tc->expr.get(), ctx);
            tc->isConst = tc->expr->isConst;
            break;
        }
        case ASTKind::IntrinsicCallExpr: {
            auto* ic = static_cast<IntrinsicCallExprAST*>(expr);
            for (auto& arg : ic->args) annotateExpr(arg.get(), ctx);
            std::string_view name = ctx.pool.lookup(ic->intrinsicName);
            bool isCompileTime = (name == "sizeof" || name == "alignof");
            ic->isConst = isCompileTime;
            break;
        }
        case ASTKind::CallableRefExpr: {
            auto* cr = static_cast<CallableRefExprAST*>(expr);
            if (cr->entity) annotateExpr(cr->entity.get(), ctx);
            cr->isConst = false;
            break;
        }
        default:
            LUC_LOG_SEMANTIC("annotateExpr: unhandled kind " << static_cast<int>(expr->kind));
            expr->isConst = false;
            break;
    }
}

// ============================================================================
// Statement annotation (recursive)
// ============================================================================
static void annotateStmt(StmtAST* stmt, SemanticContext& ctx) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            auto* block = static_cast<BlockStmtAST*>(stmt);
            for (auto& s : block->stmts) annotateStmt(s.get(), ctx);
            break;
        }
        case ASTKind::ExprStmt: {
            auto* es = static_cast<ExprStmtAST*>(stmt);
            annotateExpr(es->expr.get(), ctx);
            break;
        }
        case ASTKind::DeclStmt: {
            auto* ds = static_cast<DeclStmtAST*>(stmt);
            annotateDecl(ds->decl.get(), ctx);
            break;
        }
        case ASTKind::IfStmt: {
            auto* ifs = static_cast<IfStmtAST*>(stmt);
            annotateExpr(ifs->condition.get(), ctx);
            annotateStmt(ifs->thenBranch.get(), ctx);
            if (ifs->elseBranch) annotateStmt(ifs->elseBranch.get(), ctx);
            break;
        }
        case ASTKind::SwitchStmt: {
            auto* sw = static_cast<SwitchStmtAST*>(stmt);
            annotateExpr(sw->subject.get(), ctx);
            for (auto& cas : sw->cases) annotateSwitchCase(cas.get(), ctx);
            if (sw->defaultBody) annotateStmt(sw->defaultBody.get(), ctx);
            break;
        }
        case ASTKind::ForStmt: {
            auto* forStmt = static_cast<ForStmtAST*>(stmt);
            if (forStmt->iterVar) {
                forStmt->iterVar->isConst = false;
                if (forStmt->iterVar->type) annotateType(forStmt->iterVar->type.get(), ctx);
            }
            annotateExpr(forStmt->iterable.get(), ctx);
            if (forStmt->step) annotateExpr(forStmt->step.get(), ctx);
            annotateStmt(forStmt->body.get(), ctx);
            break;
        }
        case ASTKind::WhileStmt: {
            auto* whileStmt = static_cast<WhileStmtAST*>(stmt);
            annotateExpr(whileStmt->condition.get(), ctx);
            annotateStmt(whileStmt->body.get(), ctx);
            break;
        }
        case ASTKind::DoWhileStmt: {
            auto* dowhile = static_cast<DoWhileStmtAST*>(stmt);
            annotateStmt(dowhile->body.get(), ctx);
            annotateExpr(dowhile->condition.get(), ctx);
            break;
        }
        case ASTKind::ReturnStmt: {
            auto* ret = static_cast<ReturnStmtAST*>(stmt);
            for (auto& val : ret->values) annotateExpr(val.get(), ctx);
            break;
        }
        case ASTKind::BreakStmt:
        case ASTKind::ContinueStmt:
            // nothing to annotate
            break;
        case ASTKind::MultiVarDecl: {
            auto* mvd = static_cast<MultiVarDeclAST*>(stmt);
            mvd->isConst = (mvd->keyword == DeclKeyword::Const);
            annotateExpr(mvd->rhs.get(), ctx);
            break;
        }
        case ASTKind::MultiAssignStmt: {
            auto* mas = static_cast<MultiAssignStmtAST*>(stmt);
            for (auto& lhs : mas->lhs) annotateExpr(lhs.get(), ctx);
            annotateExpr(mas->rhs.get(), ctx);
            break;
        }
        default:
            LUC_LOG_SEMANTIC("annotateStmt: unhandled kind " << static_cast<int>(stmt->kind));
            break;
    }
}

// ============================================================================
// SwitchCase annotation
// ============================================================================
static void annotateSwitchCase(SwitchCaseAST* cas, SemanticContext& ctx) {
    if (!cas) return;
    for (auto& val : cas->values) annotateExpr(val.get(), ctx);
    if (cas->body) annotateStmt(cas->body.get(), ctx);
}

// ============================================================================
// Pattern and arm annotation (match expressions)
// ============================================================================
static void annotatePattern(PatternAST* pattern, SemanticContext& ctx) {
    if (!pattern) return;
    switch (pattern->kind) {
        case ASTKind::BindPattern:
        case ASTKind::WildcardPattern:
            // no children
            break;
        case ASTKind::TypePattern: {
            auto* tp = static_cast<TypePatternAST*>(pattern);
            if (tp->checkType) annotateType(tp->checkType.get(), ctx);
            break;
        }
        case ASTKind::StructPattern: {
            auto* sp = static_cast<StructPatternAST*>(pattern);
            for (auto& fp : sp->fields) annotateFieldPattern(fp.get(), ctx);
            break;
        }
        case ASTKind::PatternExpr: {
            auto* pe = static_cast<PatternExprAST*>(pattern);
            if (pe->inner) annotateExpr(pe->inner.get(), ctx);
            break;
        }
        default:
            break;
    }
}

static void annotateFieldPattern(FieldPatternAST* fp, SemanticContext& ctx) {
    if (!fp) return;
    if (fp->subPattern) annotatePattern(fp->subPattern.get(), ctx);
}

static void annotateArm(MatchArmAST* arm, SemanticContext& ctx) {
    if (!arm) return;
    for (auto& pat : arm->patterns) annotatePattern(pat.get(), ctx);
    if (arm->guard) annotateExpr(arm->guard.get(), ctx);
    for (auto& expr : arm->exprs) annotateExpr(expr.get(), ctx);
}

static void annotateDefaultArm(DefaultArmAST* arm, SemanticContext& ctx) {
    if (!arm) return;
    for (auto& expr : arm->exprs) annotateExpr(expr.get(), ctx);
}

static void annotateOkArm(OkArmAST* arm, SemanticContext& ctx) {
    if (!arm) return;
    if (arm->bindType) annotateType(arm->bindType.get(), ctx);
    if (arm->body) annotateStmt(arm->body.get(), ctx);
}

static void annotateErrArm(ErrArmAST* arm, SemanticContext& ctx) {
    if (!arm) return;
    if (arm->bindType) annotateType(arm->bindType.get(), ctx);
    if (arm->body) annotateStmt(arm->body.get(), ctx);
}

// ============================================================================
// Type annotation (only needed for children that are expressions, e.g., default values)
// ============================================================================
static void annotateType(TypeAST* type, SemanticContext& ctx) {
    if (!type) return;

    switch (type->kind) {
        case ASTKind::NullableType: {
            auto* nt = static_cast<NullableTypeAST*>(type);
            annotateType(nt->inner.get(), ctx);
            break;
        }
        case ASTKind::ArrayType: {
            auto* at = static_cast<ArrayTypeAST*>(type);
            annotateType(at->element.get(), ctx);
            break;
        }
        case ASTKind::GenericArrayType: {
            auto* gat = static_cast<GenericArrayTypeAST*>(type);
            annotateType(gat->element.get(), ctx);
            break;
        }
        case ASTKind::RefType: {
            auto* rt = static_cast<RefTypeAST*>(type);
            annotateType(rt->inner.get(), ctx);
            break;
        }
        case ASTKind::PtrType: {
            auto* pt = static_cast<PtrTypeAST*>(type);
            annotateType(pt->inner.get(), ctx);
            break;
        }
        case ASTKind::FuncType: {
            auto* ft = static_cast<FuncTypeAST*>(type);
            for (auto& param : ft->sig.allParams) {
                if (param) annotateType(param->type.get(), ctx);
            }
            for (auto& ret : ft->sig.returnTypes) {
                if (ret) annotateType(ret.get(), ctx);
            }
            break;
        }
        default:
            // PrimitiveTypeAST, NamedTypeAST, ResultTypeAST have no expression children
            break;
    }
}

// ============================================================================
// Declaration annotation (top‑level and local)
// ============================================================================
static void annotateDecl(DeclAST* decl, SemanticContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::VarDecl: {
            auto* var = static_cast<VarDeclAST*>(decl);
            var->isConst = (var->keyword == DeclKeyword::Const);
            if (var->init) annotateExpr(var->init.get(), ctx);
            if (var->type) annotateType(var->type.get(), ctx);
            break;
        }
        case ASTKind::FuncDecl: {
            auto* func = static_cast<FuncDeclAST*>(decl);
            func->isConst = (func->keyword == DeclKeyword::Const);
            for (auto& param : func->funcType->sig.allParams) {
                if (param) {
                    param->isConst = false;
                    if (param->type) annotateType(param->type.get(), ctx);
                }
            }
            if (func->body) annotateStmt(func->body.get(), ctx);
            break;
        }
        case ASTKind::StructDecl: {
            auto* st = static_cast<StructDeclAST*>(decl);
            for (auto& field : st->fields) {
                if (field->defaultVal) annotateExpr(field->defaultVal.get(), ctx);
                if (field->type) annotateType(field->type.get(), ctx);
            }
            break;
        }
        case ASTKind::EnumDecl: {
            auto* en = static_cast<EnumDeclAST*>(decl);
            en->isConst = true;
            for (auto& var : en->variants) var->isConst = true;
            break;
        }
        case ASTKind::TraitDecl: {
            auto* tr = static_cast<TraitDeclAST*>(decl);
            for (auto& method : tr->methods) {
                if (method->funcType) annotateType(method->funcType.get(), ctx);
            }
            break;
        }
        case ASTKind::ImplDecl: {
            auto* impl = static_cast<ImplDeclAST*>(decl);
            for (auto& method : impl->methods) {
                if (method->funcType) annotateType(method->funcType.get(), ctx);
                if (method->body) annotateStmt(method->body.get(), ctx);
            }
            break;
        }
        case ASTKind::FromDecl: {
            auto* from = static_cast<FromDeclAST*>(decl);
            for (auto& entry : from->entries) {
                for (auto& param : entry->sig.allParams) {
                    if (param && param->type) annotateType(param->type.get(), ctx);
                }
                if (entry->returnType) annotateType(entry->returnType.get(), ctx);
                if (entry->body) annotateStmt(entry->body.get(), ctx);
            }
            break;
        }
        case ASTKind::TypeAliasDecl: {
            auto* ta = static_cast<TypeAliasDeclAST*>(decl);
            if (ta->aliasedType) annotateType(ta->aliasedType.get(), ctx);
            break;
        }
        default:
            // PackageDecl, UseDecl, etc. have no runtime annotations
            break;
    }
}

// ============================================================================
// Public entry point
// ============================================================================
void annotateAll(std::vector<ProgramAST*>& files, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: annotating " << files.size() << " files");

    for (auto* prog : files) {
        if (!prog) continue;
        ctx.currentFile = prog->filePath;
        for (auto& decl : prog->decls) {
            annotateDecl(decl.get(), ctx);
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: annotation pass complete");
}