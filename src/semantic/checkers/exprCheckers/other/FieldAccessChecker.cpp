/**
 * @file FieldAccessChecker.cpp
 * @brief Semantic checking for struct field access expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkFieldAccessExpr: field=" << ctx.pool.lookup(node.field));
    
    TypeAST* objectType = checkExpr(node.object.get(), ctx);
    if (!objectType) return nullptr;
    
    // Check if object is nullable
    if (TypeChecker::isNullable(objectType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002,
                  "cannot access field on nullable value; use '?.' chain or guard first");
        return nullptr;
    }
    
    // Must be a named type (struct or enum)
    if (!objectType->isa<NamedTypeAST>()) {
        ctx.error(node.loc, DiagCode::E2002, "field access only allowed on structs and enums");
        return nullptr;
    }
    
    InternedString typeName = objectType->as<NamedTypeAST>()->name;
    Symbol* sym = ctx.symbols->lookup(typeName);
    if (!sym) {
        ctx.error(node.loc, DiagCode::E2001,
                  "type '", ctx.pool.lookup(typeName), "' not found");
        return nullptr;
    }
    
    // Enum variant access: EnumName.variantName
    if (sym->kind == SymbolKind::Enum) {
        std::string mangled = NameMangler::mangleEnumVariant(
            ctx.pool.lookup(typeName),
            ctx.pool.lookup(node.field));
        
        Symbol* variantSym = ctx.symbols->lookup(ctx.pool.intern(mangled));
        if (!variantSym) {
            ctx.error(node.loc, DiagCode::E2001,
                      "enum '", ctx.pool.lookup(typeName),
                      "' has no variant '", ctx.pool.lookup(node.field), "'");
            return nullptr;
        }
        
        TypeAST* variantType = variantSym->type;
        if (!variantType) {
            variantType = objectType;
            variantSym->type = variantType;
        }
        
        node.resolvedType = variantType;
        node.isConst = true; // Enum variants are compile-time constants
        return variantType;
    }
    
    // Struct field access
    if (sym->kind == SymbolKind::Struct) {
        auto* structDecl = sym->decl->as<StructDeclAST>();
        
        for (auto& field : structDecl->fields) {
            if (field->name == node.field) {
                if (!field->type) {
                    ctx.error(node.loc, DiagCode::E2002, "field has no type");
                    return nullptr;
                }
                
                TypeAST* fieldType = field->type.get();
                node.resolvedType = fieldType;
                node.isConst = node.object->isConst && fieldType->isa<PrimitiveTypeAST>();
                return fieldType;
            }
        }
        
        ctx.error(node.loc, DiagCode::E2001,
                  "struct '", ctx.pool.lookup(typeName),
                  "' has no field '", ctx.pool.lookup(node.field), "'");
        return nullptr;
    }
    
    ctx.error(node.loc, DiagCode::E2002, "field access only allowed on structs and enums");
    return nullptr;
}