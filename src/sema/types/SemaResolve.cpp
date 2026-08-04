/// @file SemaResolve.cpp
/// @brief Implementation of type resolution functions.

#include "SemaResolve.hpp"
#include "SemaCompare.hpp"
#include "SemaValidate.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace sema {

// ─── Main Resolution Entry Point ─────────────────────────────────────────

TypeAST* resolveType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType: 
            return resolvePrimitiveType(type->as<PrimitiveTypeAST>(), ctx);
        case ASTKind::NamedType:     
            return resolveNamedType(type->as<NamedTypeAST>(), ctx);
        case ASTKind::ArrayType:     
            return resolveArrayType(type->as<ArrayTypeAST>(), ctx);
        case ASTKind::NullableType:  
            return resolveNullableType(type->as<NullableTypeAST>(), ctx);
        case ASTKind::FallibleType:  
            return resolveFallibleType(type->as<FallibleTypeAST>(), ctx);
        case ASTKind::CombinedType:  
            return resolveCombinedType(type->as<CombinedTypeAST>(), ctx);
        case ASTKind::RefType:       
            return resolveRefType(type->as<RefTypeAST>(), ctx);
        case ASTKind::PtrType:       
            return resolvePtrType(type->as<PtrTypeAST>(), ctx);
        case ASTKind::FuncType:      
            return resolveFuncType(type->as<FuncTypeAST>(), ctx);
        default:
            ctx.diagnostics.error(DiagCode::Sem_UnknownType, type,
                                  "unknown type");
            return nullptr;
    }
}

// ─── Primitive Type ──────────────────────────────────────────────────────

TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx) {
    // Primitive types are always valid - they're built-in
    return const_cast<PrimitiveTypeAST*>(type);
}

// ─── Named Type ──────────────────────────────────────────────────────────

TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── 1. Check: Is this a generic parameter? ──────────────────────────
    if (ctx.isGenericParam(type->name)) {
        return const_cast<NamedTypeAST*>(type);
    }

    // ─── 2. Look up as concrete type ──────────────────────────────────────
    const TypeDeclAST* decl = ctx.lookupType(type->name);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, type,
                              "undefined type '", ctx.pool.lookup(type->name), "'");
        return nullptr;
    }

    // ─── 3. Check for self-reference ──────────────────────────────────────
    // Self-reference detection - the wrapper resolvers will validate

    // ─── 4. Resolve generic arguments if present ─────────────────────────
    if (!type->genericArgs.empty()) {
        if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (type->genericArgs.size() != traitDecl->genericParams.size()) {
                ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                      "trait '", ctx.pool.lookup(type->name),
                                      "' expected ", traitDecl->genericParams.size(),
                                      " generic arguments, got ", 
                                      type->genericArgs.size());
                return nullptr;
            }
        } else if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (type->genericArgs.size() != structDecl->genericParams.size()) {
                ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                      "struct '", ctx.pool.lookup(type->name),
                                      "' expected ", structDecl->genericParams.size(),
                                      " generic arguments, got ", 
                                      type->genericArgs.size());
                return nullptr;
            }
        }

        // Resolve each generic argument type
        for (const TypePtr arg : type->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }
    }

    return const_cast<NamedTypeAST*>(type);
}

// ─── Array Type ──────────────────────────────────────────────────────────

TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* element = resolveType(type->element, ctx);
    if (!element) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, type,
                              "invalid array element type");
        return nullptr;
    }

    if (element->isa<RefTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                              "reference type (&T) cannot be stored in an array");
        return nullptr;
    }

    return const_cast<ArrayTypeAST*>(type);
}

// ─── Nullable Type ──────────────────────────────────────────────────────

TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid nullable inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be nullable");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be nullable (use empty array instead)");
        return nullptr;
    }

    return const_cast<NullableTypeAST*>(type);
}

// ─── Fallible Type ──────────────────────────────────────────────────────

TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid fallible inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be fallible");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be fallible");
        return nullptr;
    }

    return const_cast<FallibleTypeAST*>(type);
}

// ─── Combined Type ──────────────────────────────────────────────────────

TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid combined inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be combined");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be combined");
        return nullptr;
    }

    return const_cast<CombinedTypeAST*>(type);
}

// ─── Reference Type ─────────────────────────────────────────────────────

TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidPointerTarget, type,
                              "invalid reference target type");
        return nullptr;
    }

    // ─── Validate Downward Flow Rule ──────────────────────────────────────
    // References (&T) are strictly scoped. They are allowed to flow downward
    // (into nested calls), but never upward or sideways.

    // 1. Cannot store &T in struct fields
    const TypeDeclAST* currentStruct = ctx.currentDefiningType();
    if (currentStruct && currentStruct->isa<StructDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_RefInStruct, type,
                              "reference type (&T) cannot be stored in struct fields");
        return nullptr;
    }

    if (isTraitType(inner, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_RefToTrait, type,
                              "cannot take reference to trait type (&Trait)");
        return nullptr;
    }

    return const_cast<RefTypeAST*>(type);
}

// ─── Pointer Type ───────────────────────────────────────────────────────

TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidPointerTarget, type,
                              "invalid pointer target type");
        return nullptr;
    }

    return const_cast<PtrTypeAST*>(type);
}

// ─── Function Type ──────────────────────────────────────────────────────

TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    for (ParamAST* param : type->params) {
        if (!resolveType(param->type, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "invalid parameter type");
            return nullptr;
        }
    }

    if (type->returnType) {
        TypeAST* returnType = resolveType(type->returnType, ctx);
        if (!returnType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, type,
                                  "invalid return type");
            return nullptr;
        }

        if (returnType->isa<RefTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_ReturnRef, type,
                                  "function cannot return reference type (&T)");
            return nullptr;
        }

        if (isTraitType(returnType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_ReturnTrait, type,
                                  "function cannot return trait type (use a concrete struct instead)");
            return nullptr;
        }

        if (returnType->isa<FuncTypeAST>()) {
            if (!resolveFuncType(returnType->as<FuncTypeAST>(), ctx)) {
                return nullptr;
            }
        }
    }

    return const_cast<FuncTypeAST*>(type);
}

// ─── Trait Resolution ────────────────────────────────────────────────────

const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    const TypeDeclAST* typeDecl = ctx.lookupType(ref->name);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, ref,
                              "undefined trait '", ctx.pool.lookup(ref->name), "'");
        return nullptr;
    }

    if (!typeDecl->isa<TraitDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotATrait, ref,
                              "'", ctx.pool.lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    const TraitDeclAST* traitDecl = typeDecl->as<TraitDeclAST>();

    if (!ref->genericArgs.empty()) {
        if (ref->genericArgs.size() != traitDecl->genericParams.size()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, ref,
                                  "trait '", ctx.pool.lookup(ref->name),
                                  "' expected ", traitDecl->genericParams.size(),
                                  " generic arguments, got ", 
                                  ref->genericArgs.size());
            return nullptr;
        }

        for (const TypePtr arg : ref->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }
    }

    return traitDecl;
}

// ─── Callee Resolution ──────────────────────────────────────────────────

/// @brief Resolve a call expression's callee to the FuncDeclAST it names.
/// 
/// Handles two callee shapes:
///   - IdentifierExprAST: Look up in value namespace (uses ctx.lookupValue)
///   - ModuleAccessExprAST: Look up module alias, then member (uses ctx.lookupImport + ctx.findModuleTable)
/// 
/// Any other callee shape (curried call, function literal, field access) 
/// returns nullptr silently - the caller must check the callee's resolved type.
const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx) {
    if (!callee) return nullptr;

    // ─── Case 1: Plain identifier call: `foo(...)` ──────────────────────
    if (callee->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        
        if (ctx.isGenericParam(id->name)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamNotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is a generic type parameter, not a function");
            return nullptr;
        }

        const ValueDeclAST* value = ctx.lookupValue(id->name);
        if (!value) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, callee,
                                  "undefined value '", ctx.pool.lookup(id->name), "'");
            return nullptr;
        }

        if (!value->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is not callable");
            return nullptr;
        }

        return value->as<FuncDeclAST>();
    }

    // ─── Case 2: Cross-module call: `module:member(...)` ────────────────
    if (callee->isa<ModuleAccessExprAST>()) {
        const ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        
        ModuleAST* module = ctx.lookupImport(access->moduleName);
        if (!module) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, callee,
                                  "undefined module alias '", ctx.pool.lookup(access->moduleName), "'");
            return nullptr;
        }

        ModuleTable* table = ctx.findModuleTable(module);
        if (!table) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, callee,
                                  "module '", ctx.pool.lookup(access->moduleName), "' has not been analyzed");
            return nullptr;
        }

        auto it = table->values.find(access->memberName);
        if (it == table->values.end()) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedMember, callee,
                                  "module '", ctx.pool.lookup(access->moduleName),
                                  "' has no exported member '", ctx.pool.lookup(access->memberName), "'");
            return nullptr;
        }

        const ValueDeclAST* decl = it->second;
        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                                  "'", ctx.pool.lookup(access->moduleName), ":",
                                  ctx.pool.lookup(access->memberName), "' is not callable");
            return nullptr;
        }

        return decl->as<FuncDeclAST>();
    }

    // ─── Case 3: Field access call: `obj.method(...)` ────────────────────
    if (callee->isa<FieldAccessExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                              "field access is not callable (Lucid has no methods)");
        return nullptr;
    }

    // ─── Case 4: Any other callee shape ──────────────────────────────────
    return nullptr;
}

// ─── Self-Reference Detection ───────────────────────────────────────────

void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.diagnostics.error(DiagCode::Sem_SelfReferentialInit, expr,
                                      "let variable '", ctx.pool.lookup(varName),
                                      "' cannot be used in its own initializer");
            }
            return;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            checkLetSelfReference(bin->left, varName, ctx);
            checkLetSelfReference(bin->right, varName, ctx);
            return;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            checkLetSelfReference(unary->operand, varName, ctx);
            return;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            checkLetSelfReference(call->callee, varName, ctx);
            for (const ExprAST* arg : call->args) {
                checkLetSelfReference(arg, varName, ctx);
            }
            return;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            checkLetSelfReference(field->object, varName, ctx);
            return;
        }
        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            checkLetSelfReference(index->target, varName, ctx);
            checkLetSelfReference(index->index, varName, ctx);
            return;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : arr->elements) {
                checkLetSelfReference(elem, varName, ctx);
            }
            return;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : st->inits) {
                checkLetSelfReference(init->value, varName, ctx);
            }
            return;
        }
        default:
            return;
    }
}

} // namespace sema