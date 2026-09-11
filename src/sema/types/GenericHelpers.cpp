/// @file sema/types/GenericHelpers.cpp
/// @brief Implementation of generic type checking helpers.

#include "GenericHelpers.hpp"
#include "SemaType.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/trace/Trace.hpp"
#include "sema/context/Generic.hpp"
#include "sema/context/SemaContext.hpp"

#include <unordered_set>
#include <vector>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter Detection
// ─────────────────────────────────────────────────────────────────────────────

bool isGenericParameterType(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        
        // Check if resolvedDecl is a GenericParamDeclAST
        if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
            return true;
        }
        
        // Fallback: check if the name matches a generic param in the current scope
        if (ctx.isGenericParam(named->name)) {
            return true;
        }
    }
    
    return false;
}

bool containsGenericParameter(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    
    switch (type->kind) {
        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            if (isGenericParameterType(named, ctx)) return true;
            for (TypeAST* arg : named->genericArgs) {
                if (containsGenericParameter(arg, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            return containsGenericParameter(arr->element, ctx);
        }
        
        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return containsGenericParameter(nullable->inner, ctx);
        }
        
        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return containsGenericParameter(fallible->inner, ctx);
        }
        
        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return containsGenericParameter(combined->inner, ctx);
        }
        
        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return containsGenericParameter(ref->inner, ctx);
        }
        
        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return containsGenericParameter(ptr->inner, ctx);
        }
        
        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                if (containsGenericParameter(param->type, ctx)) return true;
            }
            if (func->returnType && containsGenericParameter(func->returnType, ctx)) {
                return true;
            }
            return false;
        }
        
        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return containsGenericParameter(future->inner, ctx);
        }
        
        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return containsGenericParameter(thread->inner, ctx);
        }
        
        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            return containsGenericParameter(simd->elementType, ctx);
        }
        
        default:
            return false;
    }
}

bool isTypeErasedGeneric(TypeAST* type, SemaContext& ctx) {
    (void)type;
    (void)ctx;
    return false;
}

bool isConcreteType(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    if (isGenericParameterType(type, ctx)) return false;
    if (containsGenericParameter(type, ctx)) return false;
    return true;
}

DeclAST* getInnermostGenericDeclaration(SemaContext& ctx) {
    // Check if we're inside a function
    if (ctx.stack.insideFunction()) {
        FuncDeclAST* func = ctx.getInnermostFunction();
        if (func && !func->genericParams.empty()) {
            return func;
        }
    }
    
    // Check if we're inside a struct definition
    TypeDeclAST* typeDecl = ctx.currentDefiningType();
    if (typeDecl && typeDecl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
        if (!structDecl->genericParams.empty()) {
            return structDecl;
        }
    }
    
    return nullptr;
}

bool isCurrentContextErased(SemaContext& ctx) {
    (void)ctx;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reflection Validation - Only check isTypeErasedGeneric
// ─────────────────────────────────────────────────────────────────────────────

bool validateConcreteTypeForReflection(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& intrinsicName
) {
    (void)type;
    (void)node;
    (void)ctx;
    (void)intrinsicName;
    return true;
}

bool validateConcreteTypeForSimd(TypeAST* type, BaseAST* node, SemaContext& ctx) {
    (void)type;
    (void)node;
    (void)ctx;
    return true;
}

bool validateConcreteTypeForAlloc(TypeAST* type, BaseAST* node, SemaContext& ctx) {
    (void)type;
    (void)node;
    (void)ctx;
    return true;
}

bool validateConcreteTypeForBitcast(TypeAST* type, BaseAST* node, SemaContext& ctx) {
    (void)type;
    (void)node;
    (void)ctx;
    return true;
}

bool validateConcreteTypeForArenaAlloc(TypeAST* type, BaseAST* node, SemaContext& ctx) {
    (void)type;
    (void)node;
    (void)ctx;
    return true;
}

bool validateConcreteTypeForArenaSpace(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& methodName
) {
    (void)type;
    (void)node;
    (void)ctx;
    (void)methodName;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Declaration Validation
// ─────────────────────────────────────────────────────────────────────────────

bool validateTypeErasedEligibility(DeclAST* decl, SemaContext& ctx) {
    (void)decl;
    (void)ctx;
    return true;
}

// ─── containsForbiddenConstructs ──────────────────────────────────────────

bool containsForbiddenConstructs(ExprAST* expr, const std::unordered_set<InternedString>& genericParamNames, SemaContext& ctx) {
    if (!expr) return false;
    
    // ─── Recurse into sub-expressions ─────────────────────────────────────
    switch (expr->kind) {
        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            return containsForbiddenConstructs(bin->left, genericParamNames, ctx) ||
                   containsForbiddenConstructs(bin->right, genericParamNames, ctx);
        }
        
        case ASTKind::UnaryExpr: {
            return containsForbiddenConstructs(expr->as<UnaryExprAST>()->operand, genericParamNames, ctx);
        }
        
        case ASTKind::CallExpr: {
            CallExprAST* call = expr->as<CallExprAST>();
            if (containsForbiddenConstructs(call->callee, genericParamNames, ctx)) return true;
            for (ExprAST* arg : call->args) {
                if (containsForbiddenConstructs(arg, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::ArrayLiteralExpr: {
            ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (ExprAST* elem : arr->elements) {
                if (containsForbiddenConstructs(elem, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::StructLiteralExpr: {
            StructLiteralExprAST* structLit = expr->as<StructLiteralExprAST>();
            for (FieldInitAST* init : structLit->inits) {
                if (containsForbiddenConstructs(init->value, genericParamNames, ctx)) return true;
            }
            // Check generic args
            for (TypeAST* arg : structLit->genericArgs) {
                if (typeContainsGenericParam(arg, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::PipelineExpr: {
            PipelineExprAST* pipe = expr->as<PipelineExprAST>();
            if (containsForbiddenConstructs(pipe->seed, genericParamNames, ctx)) return true;
            for (PipelineStepAST* step : pipe->steps) {
                if (containsForbiddenConstructs(step->callable, genericParamNames, ctx)) return true;
                for (ExprAST* arg : step->packArgs) {
                    if (containsForbiddenConstructs(arg, genericParamNames, ctx)) return true;
                }
            }
            return false;
        }
        
        case ASTKind::ComposeExpr: {
            ComposeExprAST* compose = expr->as<ComposeExprAST>();
            for (ComposeOperandAST* op : compose->operands) {
                if (containsForbiddenConstructs(op->callable, genericParamNames, ctx)) return true;
                for (TypeAST* arg : op->genericArgs) {
                    if (typeContainsGenericParam(arg, genericParamNames, ctx)) return true;
                }
            }
            return false;
        }
        
        case ASTKind::IndexExpr: {
            IndexExprAST* index = expr->as<IndexExprAST>();
            return containsForbiddenConstructs(index->target, genericParamNames, ctx) ||
                   containsForbiddenConstructs(index->index, genericParamNames, ctx);
        }
        
        case ASTKind::SliceExpr: {
            SliceExprAST* slice = expr->as<SliceExprAST>();
            return containsForbiddenConstructs(slice->target, genericParamNames, ctx) ||
                   containsForbiddenConstructs(slice->start, genericParamNames, ctx) ||
                   containsForbiddenConstructs(slice->end, genericParamNames, ctx);
        }
        
        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            return containsForbiddenConstructs(field->object, genericParamNames, ctx);
        }
        
        case ASTKind::ModuleAccessExpr: {
            ModuleAccessExprAST* mod = expr->as<ModuleAccessExprAST>();
            for (TypeAST* arg : mod->genericArgs) {
                if (typeContainsGenericParam(arg, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::NullCoalesceExpr: {
            NullCoalesceExprAST* coalesce = expr->as<NullCoalesceExprAST>();
            return containsForbiddenConstructs(coalesce->value, genericParamNames, ctx) ||
                   containsForbiddenConstructs(coalesce->fallback, genericParamNames, ctx);
        }
        
        case ASTKind::AssignExpr: {
            AssignExprAST* assign = expr->as<AssignExprAST>();
            return containsForbiddenConstructs(assign->lhs, genericParamNames, ctx) ||
                   containsForbiddenConstructs(assign->rhs, genericParamNames, ctx);
        }
        
        case ASTKind::IfExpr: {
            IfExprAST* ifExpr = expr->as<IfExprAST>();
            return containsForbiddenConstructs(ifExpr->condition, genericParamNames, ctx) ||
                   containsForbiddenConstructs(ifExpr->thenBranch, genericParamNames, ctx) ||
                   containsForbiddenConstructs(ifExpr->elseBranch, genericParamNames, ctx);
        }
        
        case ASTKind::RangeExpr: {
            RangeExprAST* range = expr->as<RangeExprAST>();
            return containsForbiddenConstructs(range->lo, genericParamNames, ctx) ||
                   containsForbiddenConstructs(range->hi, genericParamNames, ctx);
        }
        
        default:
            return false;
    }
}

bool containsForbiddenConstructs(StmtAST* stmt, const std::unordered_set<InternedString>& genericParamNames, SemaContext& ctx) {
    if (!stmt) return false;
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            BlockStmtAST* block = stmt->as<BlockStmtAST>();
            for (StmtAST* s : block->stmts) {
                if (containsForbiddenConstructs(s, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::ExprStmt: {
            return containsForbiddenConstructs(stmt->as<ExprStmtAST>()->expr, genericParamNames, ctx);
        }
        
        case ASTKind::ReturnStmt: {
            ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            return ret->value ? containsForbiddenConstructs(ret->value, genericParamNames, ctx) : false;
        }
        
        case ASTKind::IfStmt: {
            IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            return containsForbiddenConstructs(ifStmt->condition, genericParamNames, ctx) ||
                   containsForbiddenConstructs(ifStmt->thenBranch, genericParamNames, ctx) ||
                   containsForbiddenConstructs(ifStmt->elseBranch, genericParamNames, ctx);
        }
        
        case ASTKind::WhileStmt: {
            WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            return containsForbiddenConstructs(whileStmt->condition, genericParamNames, ctx) ||
                   containsForbiddenConstructs(whileStmt->body, genericParamNames, ctx);
        }
        
        case ASTKind::ForStmt: {
            ForStmtAST* forStmt = stmt->as<ForStmtAST>();
            return containsForbiddenConstructs(forStmt->iterable, genericParamNames, ctx) ||
                   containsForbiddenConstructs(forStmt->step, genericParamNames, ctx) ||
                   containsForbiddenConstructs(forStmt->body, genericParamNames, ctx);
        }
        
        case ASTKind::SwitchStmt: {
            SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
            if (containsForbiddenConstructs(switchStmt->subject, genericParamNames, ctx)) return true;
            for (SwitchCaseAST* caseNode : switchStmt->cases) {
                for (ExprAST* val : caseNode->values) {
                    if (containsForbiddenConstructs(val, genericParamNames, ctx)) return true;
                }
                if (containsForbiddenConstructs(caseNode->body, genericParamNames, ctx)) return true;
            }
            if (switchStmt->defaultBody) {
                if (containsForbiddenConstructs(switchStmt->defaultBody, genericParamNames, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::DeclStmt: {
            DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            DeclAST* decl = declStmt->decl;
            
            // Check declarations inside the body
            if (decl->isa<VarDeclAST>()) {
                VarDeclAST* varDecl = decl->as<VarDeclAST>();
                if (varDecl->init) {
                    return containsForbiddenConstructs(varDecl->init, genericParamNames, ctx);
                }
            } else if (decl->isa<FuncDeclAST>()) {
                FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                // Check if the function has generic parameters that shadow
                // This is a nested function with its own generics
                // We need to check its body with its own generic params
                std::unordered_set<InternedString> nestedParamNames = getGenericParamNames(funcDecl, ctx);
                if (!nestedParamNames.empty()) {
                    // Use nested params for the body
                    if (funcDecl->body) {
                        return containsForbiddenConstructs(funcDecl->body, nestedParamNames, ctx);
                    }
                } else {
                    // Non-generic nested function - use outer params
                    if (funcDecl->body) {
                        return containsForbiddenConstructs(funcDecl->body, genericParamNames, ctx);
                    }
                }
            }
            return false;
        }
        
        default:
            return false;
    }
}

bool typeContainsGenericParam(TypeAST* type, const std::unordered_set<InternedString>& genericParamNames, SemaContext& ctx) {
    if (!type) return false;
    
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (genericParamNames.find(named->name) != genericParamNames.end()) {
            return true;
        }
        for (TypeAST* arg : named->genericArgs) {
            if (typeContainsGenericParam(arg, genericParamNames, ctx)) return true;
        }
        return false;
    }
    
    // Recurse into other types
    switch (type->kind) {
        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            return typeContainsGenericParam(arr->element, genericParamNames, ctx);
        }
        
        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return typeContainsGenericParam(nullable->inner, genericParamNames, ctx);
        }
        
        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return typeContainsGenericParam(fallible->inner, genericParamNames, ctx);
        }
        
        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return typeContainsGenericParam(combined->inner, genericParamNames, ctx);
        }
        
        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return typeContainsGenericParam(ref->inner, genericParamNames, ctx);
        }
        
        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return typeContainsGenericParam(ptr->inner, genericParamNames, ctx);
        }
        
        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                if (typeContainsGenericParam(param->type, genericParamNames, ctx)) return true;
            }
            if (func->returnType && typeContainsGenericParam(func->returnType, genericParamNames, ctx)) {
                return true;
            }
            return false;
        }
        
        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return typeContainsGenericParam(future->inner, genericParamNames, ctx);
        }
        
        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return typeContainsGenericParam(thread->inner, genericParamNames, ctx);
        }
        
        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            return typeContainsGenericParam(simd->elementType, genericParamNames, ctx);
        }
        
        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Nested Generic Validation
// ─────────────────────────────────────────────────────────────────────────────

bool validateNestedGenericCompatibility(DeclAST* outerDecl, const ArenaSpan<TypeAST*>& typeArgs, SemaContext& ctx) {
    (void)outerDecl;
    (void)typeArgs;
    (void)ctx;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic Helpers
// ─────────────────────────────────────────────────────────────────────────────

void emitTypeErasedError(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& featureName,
    DiagCode code
) {
    std::string typeName = typeToString(type, ctx.pool);
    
    ctx.diagnostics.error(code, node,
                          "#", featureName, " cannot be used with type-erased generic '",
                          typeName, "'");
    
    // Find the generic parameter name for a better suggestion
    std::string paramName;
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (ctx.isGenericParam(named->name)) {
            paramName = ctx.pool.lookup(named->name);
        }
    }
    
    if (!paramName.empty()) {
        ctx.diagnostics.note(node,
                             "The type '", typeName, "' contains the generic parameter '",
                             paramName, "'");
    }
    
    ctx.diagnostics.note(node,
                         "Remove @[erased] from the enclosing function/struct to make the type concrete");
    
    // If inside a function, suggest the function signature
    if (ctx.stack.insideFunction()) {
        FuncDeclAST* func = ctx.getInnermostFunction();
        if (func) {
            ctx.diagnostics.note(node,
                                 "Remove @[erased] from the function '", ctx.pool.lookup(func->name),
                                 "' to enable compile-time type information");
        }
    }
}

std::unordered_set<InternedString> getGenericParamNames(DeclAST* decl, SemaContext& ctx) {
    std::unordered_set<InternedString> names;
    
    if (!decl) return names;
    
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        for (GenericParamDeclAST* param : funcDecl->genericParams) {
            names.insert(param->name);
        }
    } else if (decl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = decl->as<StructDeclAST>();
        for (GenericParamDeclAST* param : structDecl->genericParams) {
            names.insert(param->name);
        }
    }
    
    return names;
}

} // namespace sema