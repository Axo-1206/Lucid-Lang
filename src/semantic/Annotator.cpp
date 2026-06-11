/**
 * @file Annotator.cpp
 * @brief Implementation of Phase 4 annotation pass.
 */

#include "Annotator.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace luc {

// ============================================================================
// Constructor & Public Entry Points
// ============================================================================

Annotator::Annotator(SemanticContext& ctx) : ctx_(ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("Annotator constructed");
}

void Annotator::annotateAll(std::vector<ProgramAST*>& files) {
    for (auto* program : files) {
        annotateProgram(program);
    }
}

void Annotator::annotateProgram(ProgramAST* program) {
    if (!program) return;
    
    LUC_LOG_SEMANTIC_VERBOSE("Annotator: annotating program " 
                             << ctx_.pool.lookup(program->filePath));
    
    for (auto* decl : program->decls) {
        annotateDecl(decl);
    }
}

// ============================================================================
// Declaration Annotation
// ============================================================================

void Annotator::annotateDecl(DeclAST* decl) {
    if (!decl) return;
    
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateDecl: kind=" 
                             << LucDebug::kindToString(decl->kind));
    
    switch (decl->kind) {
        case ASTKind::VarDecl:
            annotateVarDecl(decl->as<VarDeclAST>());
            break;
        case ASTKind::FuncDecl:
            annotateFuncDecl(decl->as<FuncDeclAST>());
            break;
        case ASTKind::StructDecl:
            annotateStructDecl(decl->as<StructDeclAST>());
            break;
        case ASTKind::EnumDecl:
            annotateEnumDecl(decl->as<EnumDeclAST>());
            break;
        case ASTKind::TraitDecl:
            annotateTraitDecl(decl->as<TraitDeclAST>());
            break;
        case ASTKind::ImplDecl:
            annotateImplDecl(decl->as<ImplDeclAST>());
            break;
        case ASTKind::FromDecl:
            annotateFromDecl(decl->as<FromDeclAST>());
            break;
        case ASTKind::TypeAliasDecl:
            annotateTypeAliasDecl(decl->as<TypeAliasDeclAST>());
            break;
        default:
            break;
    }
}

void Annotator::annotateVarDecl(VarDeclAST* var) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateVarDecl: " 
                             << ctx_.pool.lookup(var->name));
    
    // Mark var as const if keyword is Const
    var->isConst = (var->keyword == DeclKeyword::Const);
    
    // Annotate initializer if present
    if (var->init) {
        annotateExpr(var->init);
    }
}

void Annotator::annotateFuncDecl(FuncDeclAST* func) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateFuncDecl: " 
                             << ctx_.pool.lookup(func->name));
    
    // Mark func as const if keyword is Const
    func->isConst = (func->keyword == DeclKeyword::Const);
    
    // Annotate body
    if (func->body) {
        annotateStmt(func->body);
    }
}

void Annotator::annotateStructDecl(StructDeclAST* structDecl) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateStructDecl: " 
                             << ctx_.pool.lookup(structDecl->name));
    
    // Annotate field default values
    for (auto* field : structDecl->fields) {
        if (field->defaultVal) {
            annotateExpr(field->defaultVal);
        }
    }
}

void Annotator::annotateEnumDecl(EnumDeclAST* enumDecl) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateEnumDecl: " 
                             << ctx_.pool.lookup(enumDecl->name));
    
    // Annotate variant explicit values (already const by nature)
    // No expression to annotate – explicit values are literals
}

void Annotator::annotateTraitDecl(TraitDeclAST* trait) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateTraitDecl: " 
                             << ctx_.pool.lookup(trait->name));
    // Trait methods have no bodies to annotate
}

void Annotator::annotateImplDecl(ImplDeclAST* impl) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateImplDecl");
    
    // Annotate each method
    for (auto* method : impl->methods) {
        if (method->isInlineBody() && method->body) {
            annotateStmt(method->body);
        } else if (method->assignmentRef) {
            annotateExpr(method->assignmentRef);
        }
    }
}

void Annotator::annotateFromDecl(FromDeclAST* from) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateFromDecl");
    
    // Annotate each entry
    for (auto* entry : from->entries) {
        if (entry->kind == FromEntryKind::Inline && entry->body) {
            annotateStmt(entry->body);
        } else if (entry->kind == FromEntryKind::Path && entry->path) {
            annotateExpr(entry->path);
        }
    }
}

void Annotator::annotateTypeAliasDecl(TypeAliasDeclAST* alias) {
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateTypeAliasDecl: " 
                             << ctx_.pool.lookup(alias->name));
    // Type aliases have no expressions to annotate
}

// ============================================================================
// Statement Annotation
// ============================================================================

void Annotator::annotateStmt(StmtAST* stmt) {
    if (!stmt) return;
    
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateStmt: kind=" 
                             << LucDebug::kindToString(stmt->kind));
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            annotateBlockStmt(stmt->as<BlockStmtAST>());
            break;
        case ASTKind::ExprStmt:
            annotateExprStmt(stmt->as<ExprStmtAST>());
            break;
        case ASTKind::DeclStmt:
            annotateDeclStmt(stmt->as<DeclStmtAST>());
            break;
        case ASTKind::IfStmt:
            annotateIfStmt(stmt->as<IfStmtAST>());
            break;
        case ASTKind::SwitchStmt:
            annotateSwitchStmt(stmt->as<SwitchStmtAST>());
            break;
        case ASTKind::ForStmt:
            annotateForStmt(stmt->as<ForStmtAST>());
            break;
        case ASTKind::WhileStmt:
            annotateWhileStmt(stmt->as<WhileStmtAST>());
            break;
        case ASTKind::DoWhileStmt:
            annotateDoWhileStmt(stmt->as<DoWhileStmtAST>());
            break;
        case ASTKind::ReturnStmt:
            annotateReturnStmt(stmt->as<ReturnStmtAST>());
            break;
        case ASTKind::MultiVarDecl:
            annotateMultiVarDecl(stmt->as<MultiVarDeclAST>());
            break;
        case ASTKind::MultiAssignStmt:
            annotateMultiAssignStmt(stmt->as<MultiAssignStmtAST>());
            break;
        default:
            break;
    }
}

void Annotator::annotateBlockStmt(BlockStmtAST* block) {
    for (auto* stmt : block->stmts) {
        annotateStmt(stmt);
    }
}

void Annotator::annotateExprStmt(ExprStmtAST* exprStmt) {
    if (exprStmt->expr) {
        annotateExpr(exprStmt->expr);
    }
}

void Annotator::annotateDeclStmt(DeclStmtAST* declStmt) {
    if (declStmt->decl) {
        annotateDecl(declStmt->decl);
    }
}

void Annotator::annotateIfStmt(IfStmtAST* ifStmt) {
    if (ifStmt->condition) {
        annotateExpr(ifStmt->condition);
    }
    if (ifStmt->thenBranch) {
        annotateStmt(ifStmt->thenBranch);
    }
    if (ifStmt->elseBranch) {
        annotateStmt(ifStmt->elseBranch);
    }
}

void Annotator::annotateSwitchStmt(SwitchStmtAST* switchStmt) {
    if (switchStmt->subject) {
        annotateExpr(switchStmt->subject);
    }
    
    for (auto* caseClause : switchStmt->cases) {
        for (auto* value : caseClause->values) {
            annotateExpr(value);
        }
        if (caseClause->body) {
            annotateStmt(caseClause->body);
        }
    }
    
    if (switchStmt->defaultBody) {
        annotateStmt(switchStmt->defaultBody);
    }
}

void Annotator::annotateForStmt(ForStmtAST* forStmt) {
    if (forStmt->iterable) {
        annotateExpr(forStmt->iterable);
    }
    if (forStmt->step) {
        annotateExpr(forStmt->step);
    }
    if (forStmt->body) {
        annotateStmt(forStmt->body);
    }
}

void Annotator::annotateWhileStmt(WhileStmtAST* whileStmt) {
    if (whileStmt->condition) {
        annotateExpr(whileStmt->condition);
    }
    if (whileStmt->body) {
        annotateStmt(whileStmt->body);
    }
}

void Annotator::annotateDoWhileStmt(DoWhileStmtAST* doWhileStmt) {
    if (doWhileStmt->body) {
        annotateStmt(doWhileStmt->body);
    }
    if (doWhileStmt->condition) {
        annotateExpr(doWhileStmt->condition);
    }
}

void Annotator::annotateReturnStmt(ReturnStmtAST* retStmt) {
    for (auto* value : retStmt->values) {
        annotateExpr(value);
    }
}

void Annotator::annotateMultiVarDecl(MultiVarDeclAST* multiDecl) {
    if (multiDecl->rhs) {
        annotateExpr(multiDecl->rhs);
    }
}

void Annotator::annotateMultiAssignStmt(MultiAssignStmtAST* multiAssign) {
    for (auto* lhs : multiAssign->lhs) {
        annotateExpr(lhs);
    }
    if (multiAssign->rhs) {
        annotateExpr(multiAssign->rhs);
    }
}

// ============================================================================
// Expression Annotation (Constness)
// ============================================================================

bool Annotator::annotateExpr(ExprAST* expr) {
    if (!expr) return false;
    
    // If already annotated, return cached value
    if (expr->isConst) return true;
    
    LUC_LOG_SEMANTIC_EXTREME("Annotator::annotateExpr: kind=" 
                             << LucDebug::kindToString(expr->kind));
    
    bool isConst = false;
    
    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            isConst = annotateLiteralExpr(expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr:
            isConst = annotateIdentifierExpr(expr->as<IdentifierExprAST>());
            break;
        case ASTKind::BinaryExpr:
            isConst = annotateBinaryExpr(expr->as<BinaryExprAST>());
            break;
        case ASTKind::UnaryExpr:
            isConst = annotateUnaryExpr(expr->as<UnaryExprAST>());
            break;
        case ASTKind::CallExpr:
            isConst = annotateCallExpr(expr->as<CallExprAST>());
            break;
        case ASTKind::ArrayLiteralExpr:
            isConst = annotateArrayLiteralExpr(expr->as<ArrayLiteralExprAST>());
            break;
        case ASTKind::StructLiteralExpr:
            isConst = annotateStructLiteralExpr(expr->as<StructLiteralExprAST>());
            break;
        case ASTKind::FieldAccessExpr:
            isConst = annotateFieldAccessExpr(expr->as<FieldAccessExprAST>());
            break;
        case ASTKind::BehaviorAccessExpr:
            isConst = annotateBehaviorAccessExpr(expr->as<BehaviorAccessExprAST>());
            expr->isBehaviorMember = true;
            break;
        case ASTKind::IndexExpr:
            isConst = annotateIndexExpr(expr->as<IndexExprAST>());
            break;
        case ASTKind::SliceExpr:
            isConst = annotateSliceExpr(expr->as<SliceExprAST>());
            break;
        case ASTKind::AssignExpr:
            isConst = annotateAssignExpr(expr->as<AssignExprAST>());
            break;
        case ASTKind::IsExpr:
            isConst = annotateIsExpr(expr->as<IsExprAST>());
            break;
        case ASTKind::NullCoalesceExpr:
            isConst = annotateNullCoalesceExpr(expr->as<NullCoalesceExprAST>());
            break;
        case ASTKind::NullableChainExpr:
            isConst = annotateNullableChainExpr(expr->as<NullableChainExprAST>());
            break;
        case ASTKind::AnonFuncExpr:
            isConst = annotateAnonFuncExpr(expr->as<AnonFuncExprAST>());
            break;
        case ASTKind::AwaitExpr:
            isConst = annotateAwaitExpr(expr->as<AwaitExprAST>());
            break;
        case ASTKind::RangeExpr:
            isConst = annotateRangeExpr(expr->as<RangeExprAST>());
            break;
        default:
            break;
    }
    
    expr->isConst = isConst;
    return isConst;
}

bool Annotator::annotateLiteralExpr(LiteralExprAST* expr) {
    // All literals are const (except maybe nil?)
    return expr->kind != LiteralKind::Nil;
}

bool Annotator::annotateIdentifierExpr(IdentifierExprAST* expr) {
    // Look up the declaration
    ValueDeclAST* decl = ctx_.scope.lookupValue(expr->name);
    if (!decl) {
        // Also check type namespace (type as value)
        TypeDeclAST* typeDecl = ctx_.scope.lookupType(expr->name);
        if (typeDecl) {
            // Type references are const (they're compile-time)
            return true;
        }
        return false;
    }
    
    return isConstDecl(decl);
}

bool Annotator::annotateBinaryExpr(BinaryExprAST* expr) {
    bool leftConst = annotateExpr(expr->left);
    bool rightConst = annotateExpr(expr->right);
    
    // Binary operations are const if both operands are const
    return leftConst && rightConst;
}

bool Annotator::annotateUnaryExpr(UnaryExprAST* expr) {
    // Unary operations are const if operand is const
    return annotateExpr(expr->operand);
}

bool Annotator::annotateCallExpr(CallExprAST* expr) {
    bool calleeConst = annotateExpr(expr->callee);
    
    bool allArgsConst = true;
    for (auto* arg : expr->args) {
        if (!annotateExpr(arg)) {
            allArgsConst = false;
        }
    }
    
    // A call is const if:
    //   1. The callee is const (function marked const)
    //   2. All arguments are const
    //   3. The function is pure (no side effects)
    // For now, just check callee and arguments
    return calleeConst && allArgsConst;
}

bool Annotator::annotateArrayLiteralExpr(ArrayLiteralExprAST* expr) {
    // All elements must be const
    for (auto* elem : expr->elements) {
        if (!annotateExpr(elem)) {
            return false;
        }
    }
    return true;
}

bool Annotator::annotateStructLiteralExpr(StructLiteralExprAST* expr) {
    // All field initializers must be const
    for (auto* init : expr->inits) {
        if (init->value && !annotateExpr(init->value)) {
            return false;
        }
    }
    return true;
}

bool Annotator::annotateFieldAccessExpr(FieldAccessExprAST* expr) {
    // Field access is const if the object expression is const
    return annotateExpr(expr->object);
}

bool Annotator::annotateBehaviorAccessExpr(BehaviorAccessExprAST* expr) {
    // Method reference is const if the object expression is const
    expr->isBehaviorMember = true;
    return annotateExpr(expr->object);
}

bool Annotator::annotateIndexExpr(IndexExprAST* expr) {
    // Array indexing is const if the array and index are const
    bool targetConst = annotateExpr(expr->target);
    bool indexConst = annotateExpr(expr->index);
    return targetConst && indexConst;
}

bool Annotator::annotateSliceExpr(SliceExprAST* expr) {
    // Slice is const if the target, start, and end are const
    bool targetConst = annotateExpr(expr->target);
    bool startConst = expr->start ? annotateExpr(expr->start) : true;
    bool endConst = expr->end ? annotateExpr(expr->end) : true;
    return targetConst && startConst && endConst;
}

bool Annotator::annotateAssignExpr(AssignExprAST* expr) {
    // Assignments are never const (they modify state)
    annotateExpr(expr->lhs);
    annotateExpr(expr->rhs);
    return false;
}

bool Annotator::annotateIsExpr(IsExprAST* expr) {
    // Type tests can be const at compile time if the expression is const
    return annotateExpr(expr->expr);
}

bool Annotator::annotateNullCoalesceExpr(NullCoalesceExprAST* expr) {
    // Null coalesce is const if both sides are const
    bool valueConst = annotateExpr(expr->value);
    bool fallbackConst = annotateExpr(expr->fallback);
    return valueConst && fallbackConst;
}

bool Annotator::annotateNullableChainExpr(NullableChainExprAST* expr) {
    // Nullable chain is const if the object is const
    return annotateExpr(expr->object);
}

bool Annotator::annotateAnonFuncExpr(AnonFuncExprAST* expr) {
    // Anonymous functions are const (they're values)
    if (expr->body) {
        annotateStmt(expr->body);
    }
    return true;
}

bool Annotator::annotateAwaitExpr(AwaitExprAST* expr) {
    // Await expressions are never const (they depend on runtime)
    annotateExpr(expr->inner);
    return false;
}

bool Annotator::annotateRangeExpr(RangeExprAST* expr) {
    // Range is const if both bounds are const
    bool loConst = annotateExpr(expr->lo);
    bool hiConst = annotateExpr(expr->hi);
    return loConst && hiConst;
}

// ============================================================================
// Helpers
// ============================================================================

bool Annotator::isConstDecl(ValueDeclAST* decl) {
    if (!decl) return false;
    
    // For variables and functions, check the keyword
    if (auto* var = decl->as<VarDeclAST>()) {
        return var->keyword == DeclKeyword::Const;
    }
    if (auto* func = decl->as<FuncDeclAST>()) {
        return func->keyword == DeclKeyword::Const;
    }
    
    // Parameters and fields are not const (they're runtime values)
    return false;
}

bool Annotator::getDeclConstness(DeclAST* decl) {
    if (auto* valueDecl = decl->as<ValueDeclAST>()) {
        return isConstDecl(valueDecl);
    }
    return false;
}

// ============================================================================
// Convenience Function
// ============================================================================

void annotateAll(std::vector<ProgramAST*>& files, SemanticContext& ctx) {
    Annotator annotator(ctx);
    annotator.annotateAll(files);
}

} // namespace luc