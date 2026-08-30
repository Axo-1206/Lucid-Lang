/// @file SemaSelfReference.cpp
/// @brief Implementation of self-reference detection and validation.

#include "SemaType.hpp"
#include "core/ASTStrings.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <unordered_set>

namespace sema {

// ─── Variable Self-Reference Detection ──────────────────────────────────

void checkLetSelfReference(ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    // Walk the expression tree looking for references to varName
    // Uses a recursive visitor pattern with early termination on error

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.diagnostics.error(DiagCode::Sem_SelfReferentialInit, expr,
                                      "let variable '", ctx.pool.lookup(varName),
                                      "' cannot be used in its own initializer");
            }
            return;
        }

        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            checkLetSelfReference(bin->left, varName, ctx);
            checkLetSelfReference(bin->right, varName, ctx);
            return;
        }

        case ASTKind::UnaryExpr: {
            UnaryExprAST* unary = expr->as<UnaryExprAST>();
            checkLetSelfReference(unary->operand, varName, ctx);
            return;
        }

        case ASTKind::CallExpr: {
            CallExprAST* call = expr->as<CallExprAST>();
            checkLetSelfReference(call->callee, varName, ctx);
            for (ExprAST* arg : call->args) {
                checkLetSelfReference(arg, varName, ctx);
            }
            return;
        }

        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            checkLetSelfReference(field->object, varName, ctx);
            return;
        }

        case ASTKind::IndexExpr: {
            IndexExprAST* index = expr->as<IndexExprAST>();
            checkLetSelfReference(index->target, varName, ctx);
            checkLetSelfReference(index->index, varName, ctx);
            return;
        }

        case ASTKind::ArrayLiteralExpr: {
            ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (ExprAST* elem : arr->elements) {
                checkLetSelfReference(elem, varName, ctx);
            }
            return;
        }

        case ASTKind::StructLiteralExpr: {
            StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (FieldInitAST* init : st->inits) {
                checkLetSelfReference(init->value, varName, ctx);
            }
            return;
        }

        case ASTKind::NullCoalesceExpr: {
            NullCoalesceExprAST* coalesce = expr->as<NullCoalesceExprAST>();
            checkLetSelfReference(coalesce->value, varName, ctx);
            checkLetSelfReference(coalesce->fallback, varName, ctx);
            return;
        }

        case ASTKind::AssignExpr: {
            AssignExprAST* assign = expr->as<AssignExprAST>();
            checkLetSelfReference(assign->lhs, varName, ctx);
            checkLetSelfReference(assign->rhs, varName, ctx);
            return;
        }

        case ASTKind::PipelineExpr: {
            PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            checkLetSelfReference(pipeline->seed, varName, ctx);
            for (PipelineStepAST* step : pipeline->steps) {
                checkLetSelfReference(step->callable, varName, ctx);
                for (ExprAST* arg : step->packArgs) {
                    checkLetSelfReference(arg, varName, ctx);
                }
            }
            return;
        }

        case ASTKind::ComposeExpr: {
            ComposeExprAST* compose = expr->as<ComposeExprAST>();
            checkLetSelfReference(compose->left, varName, ctx);
            for (ComposeOperandAST* op : compose->operands) {
                checkLetSelfReference(op->callable, varName, ctx);
            }
            return;
        }

        case ASTKind::AnonFuncExpr: {
            // An anonymous function's body may reference the variable
            // But the variable is in scope, so we check the body
            AnonFuncExprAST* anon = expr->as<AnonFuncExprAST>();
            // We need to traverse the body statement
            // For simplicity, we check the body if it's a block
            if (anon->body && anon->body->isa<BlockStmtAST>()) {
                BlockStmtAST* block = anon->body->as<BlockStmtAST>();
                for (StmtAST* stmt : block->stmts) {
                    // Check each statement for references
                    // This is a simplified check - a full implementation would
                    // need to traverse all statement types
                    if (stmt->isa<ExprStmtAST>()) {
                        checkLetSelfReference(stmt->as<ExprStmtAST>()->expr, varName, ctx);
                    } else if (stmt->isa<ReturnStmtAST>()) {
                        checkLetSelfReference(stmt->as<ReturnStmtAST>()->value, varName, ctx);
                    }
                }
            }
            return;
        }

        case ASTKind::IfExpr: {
            IfExprAST* ifExpr = expr->as<IfExprAST>();
            checkLetSelfReference(ifExpr->condition, varName, ctx);
            checkLetSelfReference(ifExpr->thenBranch, varName, ctx);
            checkLetSelfReference(ifExpr->elseBranch, varName, ctx);
            return;
        }

        case ASTKind::RangeExpr: {
            RangeExprAST* range = expr->as<RangeExprAST>();
            checkLetSelfReference(range->lo, varName, ctx);
            checkLetSelfReference(range->hi, varName, ctx);
            return;
        }

        // These expression types cannot contain variable references
        case ASTKind::LiteralExpr:
        case ASTKind::IntrinsicCallExpr:
        case ASTKind::SliceExpr:
        case ASTKind::ModuleAccessExpr:
        default:
            return;
    }
}

// ─── Struct Self-Reference Validation ───────────────────────────────────

bool isValidStructSelfReference(TypeAST* fieldType,
                                 StructDeclAST* currentStruct,
                                 SemaContext& ctx) {
    if (!fieldType || !currentStruct) return false;

    // ─── Step 1: Unwrap nullable and pointer layers ────────────────────────
    bool isNullable = false;
    bool isPointer = false;
    TypeAST* innerType = fieldType;

    if (fieldType->isa<NullableTypeAST>()) {
        isNullable = true;
        innerType = fieldType->as<NullableTypeAST>()->inner;
    }

    if (innerType->isa<PtrTypeAST>()) {
        isPointer = true;
        innerType = innerType->as<PtrTypeAST>()->inner;
    }

    // ─── Step 2: Check if the inner type is a NamedType ────────────────────
    if (!innerType->isa<NamedTypeAST>()) {
        return false;  // Not a self-reference
    }

    NamedTypeAST* named = innerType->as<NamedTypeAST>();

    // ─── Step 3: Check if it references the current struct ─────────────────
    if (named->name != currentStruct->name) {
        return false;  // Not a self-reference
    }

    // ─── Step 4: Check generic arguments match ─────────────────────────────
    if (named->genericArgs.size() != currentStruct->genericParams.size()) {
        return false;  // Different instantiation
    }

    for (size_t i = 0; i < named->genericArgs.size(); ++i) {
        TypeAST* arg = named->genericArgs[i];
        GenericParamDeclAST* param = currentStruct->genericParams[i];
        
        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* argNamed = arg->as<NamedTypeAST>();
            if (argNamed->name != param->name) {
                return false;  // Different generic arguments
            }
        } else {
            return false;  // Not a generic parameter reference
        }
    }

    // ─── Step 5: Validate self-reference rules ─────────────────────────────
    // Self-reference is only valid if it's nullable or a raw pointer.
    // Slices ([_]T) are borrowed views and cannot be stored in structs.
    
    // Check if this is a slice self-reference (invalid)
    if (innerType->isa<ArrayTypeAST>() && innerType->as<ArrayTypeAST>()->isSlice()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, fieldType,
                              "slice self-reference ([_]", ctx.pool.lookup(currentStruct->name),
                              ") is not allowed — slices are borrowed views and cannot be stored in structs");
        return false;
    }

    // Non-nullable self-reference is invalid
    if (!isNullable && !isPointer) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, fieldType,
                              "non-nullable self-reference in struct '",
                              ctx.pool.lookup(currentStruct->name),
                              "' (use '?', '*', or '*?' to allow recursion)");
        return false;
    }

    // Self-reference through raw pointer is always allowed
    // Self-reference through nullable is always allowed
    return true;
}

// ─── Generic Type Field Access ──────────────────────────────────────────

bool isFieldAccessibleOnGenericType(TypeAST* genericType,
                                     InternedString fieldName,
                                     SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = genericType->as<NamedTypeAST>();

    // ─── Step 1: Ensure the type is resolved ──────────────────────────────
    if (!named->resolvedDecl) {
        resolveNamedType(named, ctx);
    }

    // ─── Step 2: Check if it's a generic parameter ─────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
        GenericParamDeclAST* param = static_cast<GenericParamDeclAST*>(named->resolvedDecl);
        
        for (NamedTypeAST* constraint : param->constraints) {
            TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) return true;
            }
        }
        return false;
    }

    // ─── Step 3: Concrete type - check fields ─────────────────────────────
    TypeDeclAST* decl = named->resolvedDecl;
    if (!decl || !decl->isa<StructDeclAST>()) return false;

    StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) return true;
    }

    return false;
}

TypeAST* getFieldTypeOnGenericType(TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return nullptr;
    NamedTypeAST* named = genericType->as<NamedTypeAST>();

    // ─── Step 1: Ensure the type is resolved ──────────────────────────────
    if (!named->resolvedDecl) {
        resolveNamedType(named, ctx);
    }

    // ─── Step 2: Check if it's a generic parameter ─────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
        GenericParamDeclAST* param = static_cast<GenericParamDeclAST*>(named->resolvedDecl);
        
        for (NamedTypeAST* constraint : param->constraints) {
            TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) {
                    return field->type;
                }
            }
        }
        return nullptr;
    }

    // ─── Step 3: Concrete type - check fields ─────────────────────────────
    TypeDeclAST* decl = named->resolvedDecl;
    if (!decl || !decl->isa<StructDeclAST>()) return nullptr;

    StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) {
            return field->type;
        }
    }

    return nullptr;
}

} // namespace sema