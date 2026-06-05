/**
 * @file CallableExtractor.cpp
 * @brief Implementation of callable extraction.
 */

#include "CallableExtractor.hpp"
#include "../core/TypeCloner.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugMacros.hpp"

CallableExtractor::CallableExtractor(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("CallableExtractor constructed");
}

FuncTypeAST* CallableExtractor::extract(ExprPtr& callable,
                                        ArenaSpan<TypePtr>& /*explicitTypeArgs*/,
                                        const SourceLocation& loc) {
    if (!callable) return nullptr;

    switch (callable->kind) {
        case ASTKind::IdentifierExpr:
            return extractFromIdentifier(callable->as<IdentifierExprAST>(), loc);
        case ASTKind::FieldAccessExpr:
            return extractFromFieldAccess(callable->as<FieldAccessExprAST>(), loc);
        case ASTKind::CallableRefExpr:
            return extractFromCallableRef(callable->as<CallableRefExprAST>(), loc);
        case ASTKind::BehaviorAccessExpr:
            return extractFromBehaviorAccess(callable->as<BehaviorAccessExprAST>(), loc);
        default:
            ctx_.error(loc, DiagCode::E2002, "not a callable expression");
            return nullptr;
    }
}

FuncTypeAST* CallableExtractor::extractFromIdentifier(IdentifierExprAST* ident, const SourceLocation& loc) {
    Symbol* sym = ctx_.symbols->lookup(ident->name);
    if (!sym) {
        ctx_.error(loc, DiagCode::E2001, "function '" + std::string(ctx_.pool.lookup(ident->name)) + "' not found");
        return nullptr;
    }
    if (sym->kind != SymbolKind::Func && sym->kind != SymbolKind::ExternFunc) {
        ctx_.error(loc, DiagCode::E2002, "'" + std::string(ctx_.pool.lookup(ident->name)) + "' is not a function");
        return nullptr;
    }
    if (!sym->type || !sym->type->isa<FuncTypeAST>()) {
        ctx_.error(loc, DiagCode::E2002, "function has no resolved type");
        return nullptr;
    }
    return TypeCloner::cloneFunc(ctx_.arena, sym->type->as<FuncTypeAST>());
}

FuncTypeAST* CallableExtractor::extractFromFieldAccess(FieldAccessExprAST* field, const SourceLocation& loc) {
    // Simplified: assume field resolves to a function in global scope.
    Symbol* sym = ctx_.symbols->lookup(field->field);
    if (!sym) {
        ctx_.error(loc, DiagCode::E2001, "symbol not found");
        return nullptr;
    }
    if (!sym->type || !sym->type->isa<FuncTypeAST>()) {
        ctx_.error(loc, DiagCode::E2002, "field does not resolve to a function type");
        return nullptr;
    }
    return TypeCloner::cloneFunc(ctx_.arena, sym->type->as<FuncTypeAST>());
}

FuncTypeAST* CallableExtractor::extractFromCallableRef(CallableRefExprAST* callableRef, const SourceLocation& loc) {
    // Extract base entity; type arguments are handled elsewhere.
    FuncTypeAST* base = extract(callableRef->entity, callableRef->typeArgs, loc);
    if (!base) return nullptr;
    // For now, ignore explicit type args (they will be substituted later).
    return base;
}

FuncTypeAST* CallableExtractor::extractFromBehaviorAccess(BehaviorAccessExprAST* behavior, const SourceLocation& loc) {
    std::string mangled = NameMangler::mangleMethod(
        ctx_.pool.lookup(behavior->typeName),
        ctx_.pool.lookup(behavior->method)
    );
    InternedString mangledName = ctx_.pool.intern(mangled);
    Symbol* sym = ctx_.symbols->lookup(mangledName);
    if (!sym) {
        ctx_.error(loc, DiagCode::E2001,
                   "method '" + std::string(ctx_.pool.lookup(behavior->typeName)) + ":" +
                   std::string(ctx_.pool.lookup(behavior->method)) + "' not found");
        return nullptr;
    }
    if (!sym->type || !sym->type->isa<FuncTypeAST>()) {
        ctx_.error(loc, DiagCode::E2002, "method has no resolved type");
        return nullptr;
    }
    return TypeCloner::cloneFunc(ctx_.arena, sym->type->as<FuncTypeAST>());
}

TypeAST* CallableExtractor::resolveReference(ExprPtr& ref,
                                             ArenaSpan<TypePtr>& typeArgs,
                                             const SourceLocation& loc) {
    FuncTypeAST* funcType = extract(ref, typeArgs, loc);
    return funcType; // as TypeAST*
}