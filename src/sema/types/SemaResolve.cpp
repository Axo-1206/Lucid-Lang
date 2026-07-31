/// @file SemaResolve.cpp
/// @brief Implementation of type resolution functions.

#include "SemaResolve.hpp"
#include "SemaLookup.hpp"
#include "SemaCompare.hpp"
#include "SemaValidate.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

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
            ctx.error(type, DiagCode::E3003, "unknown type");
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
    // Generic parameters have highest priority and shadow type names
    if (isGenericParam(type->name, ctx)) {
        // Valid generic parameter - return as-is
        return const_cast<NamedTypeAST*>(type);
    }

    // ─── 2. Look up as concrete type ──────────────────────────────────────
    const TypeDeclAST* decl = ctx.lookupType(type->name);
    if (!decl) {
        ctx.error(type, DiagCode::E2002,
                  "undefined type '", ctx.pool.lookup(type->name), "'");
        return nullptr;
    }

    // ─── 3. Check for self-reference ──────────────────────────────────────
    // If this type is currently being defined, it's a self-reference
    // This is only allowed through ptr/ref/nullable wrappers
    if (ctx.isDefiningType(decl)) {
        // Self-reference detected - this is allowed when wrapped
        // The actual validation happens in resolveRefType/resolvePtrType
        // etc. via validateRefContext()
        // We just pass through - the wrapper resolvers will check
    }

    // ─── 4. Resolve generic arguments if present ─────────────────────────
    if (!type->genericArgs.empty()) {
        // Check arity against the declaration
        if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (type->genericArgs.size() != traitDecl->genericParams.size()) {
                ctx.error(type, DiagCode::E2207,
                          "trait '", ctx.pool.lookup(type->name),
                          "' expected ", std::to_string(traitDecl->genericParams.size()),
                          " generic arguments, got ", 
                          std::to_string(type->genericArgs.size()));
                return nullptr;
            }
        } else if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (type->genericArgs.size() != structDecl->genericParams.size()) {
                ctx.error(type, DiagCode::E2207,
                          "struct '", ctx.pool.lookup(type->name),
                          "' expected ", std::to_string(structDecl->genericParams.size()),
                          " generic arguments, got ", 
                          std::to_string(type->genericArgs.size()));
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
        ctx.error(type, DiagCode::E3003, "invalid array element type");
        return nullptr;
    }

    if (element->isa<RefTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
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
        ctx.error(type, DiagCode::E3003, "invalid nullable inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004, "function types cannot be nullable");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
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
        ctx.error(type, DiagCode::E3003, "invalid fallible inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004, "function types cannot be fallible");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004, "array types cannot be fallible");
        return nullptr;
    }

    return const_cast<FallibleTypeAST*>(type);
}

// ─── Combined Type ──────────────────────────────────────────────────────

TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid combined inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004, "function types cannot be combined");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004, "array types cannot be combined");
        return nullptr;
    }

    return const_cast<CombinedTypeAST*>(type);
}

// ─── Reference Type ─────────────────────────────────────────────────────

TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid reference target type");
        return nullptr;
    }

    // ─── Validate Downward Flow Rule ──────────────────────────────────────
    // References (&T) are strictly scoped. They are allowed to flow downward
    // (into nested calls), but never upward or sideways.

    // 1. Cannot store &T in struct fields
    const TypeDeclAST* currentStruct = ctx.currentDefiningType();
    if (currentStruct && currentStruct->isa<StructDeclAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "reference type (&T) cannot be stored in struct fields");
        return nullptr;
    }

    // References to traits are NOT allowed
    if (isTraitType(inner, ctx)) {
        ctx.error(type, DiagCode::E3004,
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
        ctx.error(type, DiagCode::E3003, "invalid pointer target type");
        return nullptr;
    }

    return const_cast<PtrTypeAST*>(type);
}

// ─── Function Type ──────────────────────────────────────────────────────

TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    for (ParamAST* param : type->params) {
        if (!resolveType(param->type, ctx)) {
            ctx.error(param, DiagCode::E3003, "invalid parameter type");
            return nullptr;
        }
    }

    if (type->returnType) {
        TypeAST* returnType = resolveType(type->returnType, ctx);
        if (!returnType) {
            ctx.error(type, DiagCode::E3003, "invalid return type");
            return nullptr;
        }

        if (returnType->isa<RefTypeAST>()) {
            ctx.error(type, DiagCode::E3004,
                      "function cannot return reference type (&T)");
            return nullptr;
        }

        if (isTraitType(returnType, ctx)) {
            ctx.error(type, DiagCode::E3004,
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
        ctx.error(ref, DiagCode::E2002,
                  "undefined trait '", ctx.pool.lookup(ref->name), "'");
        return nullptr;
    }

    // Verify it's a trait (not a struct or enum)
    if (!typeDecl->isa<TraitDeclAST>()) {
        ctx.error(ref, DiagCode::E2002,
                  "'", ctx.pool.lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    const TraitDeclAST* traitDecl = typeDecl->as<TraitDeclAST>();

    // Check generic arguments if present
    if (!ref->genericArgs.empty()) {
        if (ref->genericArgs.size() != traitDecl->genericParams.size()) {
            ctx.error(ref, DiagCode::E2207,
                      "trait '", ctx.pool.lookup(ref->name),
                      "' expected ", std::to_string(traitDecl->genericParams.size()),
                      " generic arguments, got ", 
                      std::to_string(ref->genericArgs.size()));
            return nullptr;
        }

        // Resolve each generic argument type
        for (const TypePtr arg : ref->genericArgs) {
            if (!resolveType(arg, ctx)) {
                // Error already reported
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
        
        // Check if it's a generic parameter (not callable)
        if (ctx.isGenericParam(id->name)) {
            ctx.error(callee, DiagCode::E2003,
                      "'", ctx.pool.lookup(id->name), "' is a generic type parameter, not a function");
            return nullptr;
        }

        // Look up the value in the symbol table
        const ValueDeclAST* value = ctx.lookupValue(id->name);
        if (!value) {
            ctx.error(callee, DiagCode::E2001,
                      "undefined value '", ctx.pool.lookup(id->name), "'");
            return nullptr;
        }

        // Must be a function
        if (!value->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003,
                      "'", ctx.pool.lookup(id->name), "' is not callable");
            return nullptr;
        }

        return value->as<FuncDeclAST>();
    }

    // ─── Case 2: Cross-module call: `module:member(...)` ────────────────
    if (callee->isa<ModuleAccessExprAST>()) {
        const ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        
        // Look up the module alias
        ModuleAST* module = ctx.lookupImport(access->moduleName);
        if (!module) {
            ctx.error(callee, DiagCode::E2001,
                      "undefined module alias '", ctx.pool.lookup(access->moduleName), "'");
            return nullptr;
        }

        // Get the module's table
        ModuleTable* table = ctx.findModuleTable(module);
        if (!table) {
            ctx.error(callee, DiagCode::E2001,
                      "module '", ctx.pool.lookup(access->moduleName), "' has not been analyzed");
            return nullptr;
        }

        // Look up the member
        auto it = table->values.find(access->memberName);
        if (it == table->values.end()) {
            ctx.error(callee, DiagCode::E2001,
                      "module '", ctx.pool.lookup(access->moduleName),
                      "' has no exported member '", ctx.pool.lookup(access->memberName), "'");
            return nullptr;
        }

        const ValueDeclAST* decl = it->second;
        if (!decl->isa<FuncDeclAST>()) {
            ctx.error(callee, DiagCode::E2003,
                      "'", ctx.pool.lookup(access->moduleName), ":",
                      ctx.pool.lookup(access->memberName), "' is not callable");
            return nullptr;
        }

        return decl->as<FuncDeclAST>();
    }

    // ─── Case 3: Field access call: `obj.method(...)` ────────────────────
    // Lucid has no methods - field access is for struct fields only
    if (callee->isa<FieldAccessExprAST>()) {
        // Field access expressions in Lucid are for struct fields, not methods
        ctx.error(callee, DiagCode::E2003,
                  "field access is not callable (Lucid has no methods)");
        return nullptr;
    }

    // ─── Case 4: Any other callee shape ──────────────────────────────────
    // Examples: function literal, curried call, pipeline step, etc.
    // The caller must check callee->resolvedType instead
    return nullptr;
}

// ─── Self-Reference Detection ───────────────────────────────────────────

void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.error(expr, DiagCode::E3003,
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