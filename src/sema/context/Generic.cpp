/// @file sema/context/Generic.cpp
/// @brief Implementation of generic instantiation and substitution.

#include "Generic.hpp"
#include "sema/support/MangledName.hpp"
#include "core/trace/Trace.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Type Substitution Helpers (implementations)
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* substituteType(TypeAST* type, const GenericSubstitution& subst, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return type;

        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            
            if (subst.isParam(named->name)) {
                TypeAST* result = subst.lookup(named->name);
                if (result) {
                    return substituteType(result, subst, ctx);
                }
                return type;
            }
            
            if (!named->genericArgs.empty()) {
                bool changed = false;
                ArenaSpan<TypeAST*> subArgs(ctx.arena);
                for (TypeAST* arg : named->genericArgs) {
                    TypeAST* subArg = substituteType(arg, subst, ctx);
                    subArgs.push_back(subArg);
                    if (subArg != arg) changed = true;
                }
                
                if (changed) {
                    NamedTypeAST* newNamed = ctx.arena.make<NamedTypeAST>(named->name);
                    newNamed->genericArgs = subArgs.toSpan();
                    newNamed->resolvedDecl = named->resolvedDecl;
                    newNamed->loc = named->loc;
                    return newNamed;
                }
            }
            
            return type;
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            TypeAST* subElement = substituteType(arr->element, subst, ctx);
            if (subElement != arr->element) {
                return ctx.getArrayType(arr->arrayKind, arr->size, subElement);
            }
            return type;
        }

        // ... all other cases from the inline version ...
        // (I'm showing the full implementation pattern, but you'd copy all cases)

        default:
            return type;
    }
}

// ... substituteStmt, substituteExpr, containsGenericParams ...

// ─────────────────────────────────────────────────────────────────────────────
// Specialized Struct Creation
// ─────────────────────────────────────────────────────────────────────────────

StructDeclAST* createSpecializedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx) 
{
    if (!templateDecl) return nullptr;

    if (typeArgs.size() != templateDecl->genericParams.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, templateDecl,
            "struct '", ctx.pool.lookup(templateDecl->name),
            "' expected ", templateDecl->genericParams.size(),
            " generic arguments, got ", typeArgs.size());
        return nullptr;
    }

    InternedString mangledName = generateMangledNameForGeneric(
        templateDecl, typeArgs, ctx);
    
    if (!mangledName.isValid()) {
        ctx.diagnostics.error(DiagCode::Backend_InvalidIR, templateDecl,
            "failed to generate mangled name for generic struct '",
            ctx.pool.lookup(templateDecl->name), "'");
        return nullptr;
    }

    GenericSubstitution subst{templateDecl->genericParams, typeArgs};

    ArenaSpan<FieldDeclAST*> substitutedFields(ctx.arena);
    
    for (FieldDeclAST* field : templateDecl->fields) {
        TypeAST* substitutedType = substituteType(field->type, subst, ctx);
        if (!substitutedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, field,
                "field '", ctx.pool.lookup(field->name),
                "' has invalid type in specialization");
            return nullptr;
        }

        ExprAST* substitutedDefault = field->defaultVal 
            ? substituteExpr(field->defaultVal, subst, ctx) 
            : nullptr;

        StmtAST* substitutedBody = field->defaultBody 
            ? substituteStmt(field->defaultBody, subst, ctx) 
            : nullptr;

        FieldDeclAST* newField = ctx.arena.make<FieldDeclAST>(
            field->name,
            substitutedType,
            substitutedDefault,
            substitutedBody,
            field->isConstField
        );
        newField->loc = field->loc;
        substitutedFields.push_back(newField);
    }

    StructDeclAST* specialized = ctx.arena.make<StructDeclAST>(
        mangledName,
        ArenaSpan<GenericParamDeclAST*>(),
        substitutedFields.toSpan(),
        templateDecl->traitRefs,
        templateDecl->isPacked
    );
    specialized->shouldSpecialize = true;
    specialized->mangledName = mangledName;
    specialized->loc = templateDecl->loc;

    Trace::detail("Created specialized struct: ", ctx.pool.lookup(mangledName),
                " (", substitutedFields.size(), " fields)");

    return specialized;
}

FuncDeclAST* createSpecializedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx) 
{
    // ... implementation ...
}

} // namespace sema