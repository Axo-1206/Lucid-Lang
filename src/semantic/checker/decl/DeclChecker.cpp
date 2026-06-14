/**
 * @file DeclChecker.cpp
 * @brief Implementation of the top-level declaration dispatcher.
 * 
 * This file contains only the dispatch logic. All declaration-specific
 * validation is implemented in their respective .cpp files.
 */

#include "DeclChecker.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx) {
    if (!decl) return;
    
    LUC_LOG_SEMANTIC_EXTREME("checkTopLevelDecl: kind=" << LucDebug::kindToString(decl->kind));
    
    switch (decl->kind) {
        case ASTKind::VarDecl:
            checkVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
            
        case ASTKind::FuncDecl:
            checkFuncDecl(decl->as<FuncDeclAST>(), ctx);
            break;
            
        case ASTKind::StructDecl:
            checkStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
            
        case ASTKind::EnumDecl:
            checkEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
            
        case ASTKind::TraitDecl:
            checkTraitDecl(decl->as<TraitDeclAST>(), ctx);
            break;
            
        case ASTKind::ImplDecl:
            checkImplDecl(decl->as<ImplDeclAST>(), ctx);
            break;
            
        case ASTKind::FromDecl:
            checkFromDecl(decl->as<FromDeclAST>(), ctx);
            break;
            
        case ASTKind::TypeAliasDecl:
            checkTypeAliasDecl(decl->as<TypeAliasDeclAST>(), ctx);
            break;
            
        default:
            // PackageDecl, UseDecl, and unknown declarations have no semantic checks
            // PackageDecl: only for package name validation (handled elsewhere)
            // UseDecl: handled by module loader, not semantic analysis
            // UnknownDecl: error already reported by parser
            break;
    }
}