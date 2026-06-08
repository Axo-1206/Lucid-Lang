#include "CallableExtractor.hpp"
#include "../core/TypeCloner.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugMacros.hpp"

CallableExtractor::CallableExtractor(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("CallableExtractor constructed");
}

FuncTypeAST* CallableExtractor::extract(ExprPtr& callable,
                                        ArenaSpan<TypePtr>& explicitTypeArgs,
                                        const SourceLocation& loc) {
    if (!callable) return nullptr;

    switch (callable->kind) {
        case ASTKind::IdentifierExpr:
            return extractFromIdentifier(callable->as<IdentifierExprAST>(), loc);
        case ASTKind::FieldAccessExpr:
            return extractFromFieldAccess(callable->as<FieldAccessExprAST>(), loc);
        case ASTKind::BehaviorAccessExpr:
            return extractFromBehaviorAccess(callable->as<BehaviorAccessExprAST>(), loc);
        default:
            ctx_.error(loc, DiagCode::E2002, "not a callable expression");
            return nullptr;
    }
}

FuncTypeAST* CallableExtractor::extractFromIdentifier(IdentifierExprAST* ident, const SourceLocation& loc) {
    std::string funcName = std::string(ctx_.pool.lookup(ident->name));
    
    // Check if the identifier has generic arguments attached
    if (ident->genericArgs.size() > 0) {
        LUC_LOG_SEMANTIC("extractFromIdentifier: generic function '" << funcName 
                         << "' with " << ident->genericArgs.size() << " type args");
    }
    
    Symbol* sym = ctx_.symbols->lookup(ident->name);
    if (!sym) {
        ctx_.error(loc, DiagCode::E2001, "function '" + funcName + "' not found");
        return nullptr;
    }
    if (sym->kind != SymbolKind::Func && sym->kind != SymbolKind::ExternFunc) {
        ctx_.error(loc, DiagCode::E2002, "'" + funcName + "' is not a function");
        return nullptr;
    }
    if (!sym->type || !sym->type->isa<FuncTypeAST>()) {
        ctx_.error(loc, DiagCode::E2002, "function has no resolved type");
        return nullptr;
    }
    FuncTypeAST* funcType = TypeCloner::cloneFunc(ctx_.arena, sym->type->as<FuncTypeAST>());
    
    // Apply generic arguments if present (they will be used during instantiation)
    // The actual instantiation happens in the call checker, not here
    return funcType;
}

FuncTypeAST* CallableExtractor::extractFromFieldAccess(FieldAccessExprAST* field, const SourceLocation& loc) {
    std::string fieldName = std::string(ctx_.pool.lookup(field->field));
    
    // Check if the field access has generic arguments attached
    if (field->genericArgs.size() > 0) {
        LUC_LOG_SEMANTIC("extractFromFieldAccess: generic function '" << fieldName 
                         << "' with " << field->genericArgs.size() << " type args");
    }
    
    // For qualified names like math.toString, we need to resolve the module path
    // First try to resolve from the object (module path)
    Symbol* sym = nullptr;
    
    // If the object is an identifier, treat it as a module path prefix
    if (field->object && field->object->isa<IdentifierExprAST>()) {
        IdentifierExprAST* module = field->object->as<IdentifierExprAST>();
        std::string moduleName = std::string(ctx_.pool.lookup(module->name));
        
        // Try to lookup the fully qualified name
        std::string qualifiedName = moduleName + "." + fieldName;
        InternedString qualified = ctx_.pool.intern(qualifiedName);
        sym = ctx_.symbols->lookup(qualified);
        
        if (!sym) {
            // Try just the field name
            sym = ctx_.symbols->lookup(field->field);
        }
    } else {
        // Just look up the field name
        sym = ctx_.symbols->lookup(field->field);
    }
    
    if (!sym) {
        ctx_.error(loc, DiagCode::E2001, "symbol '" + fieldName + "' not found");
        return nullptr;
    }
    if (!sym->type || !sym->type->isa<FuncTypeAST>()) {
        ctx_.error(loc, DiagCode::E2002, "field does not resolve to a function type");
        return nullptr;
    }
    FuncTypeAST* funcType = TypeCloner::cloneFunc(ctx_.arena, sym->type->as<FuncTypeAST>());
    
    // Apply generic arguments if present
    return funcType;
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
    // The typeArgs parameter may contain generic arguments from the call syntax
    // (e.g., identity<int>(42) where <int> is parsed as part of the call)
    // However, with the new design, generic arguments are stored directly on
    // IdentifierExprAST or FieldAccessExprAST, so typeArgs is usually empty.
    FuncTypeAST* funcType = extract(ref, typeArgs, loc);
    return funcType;
}