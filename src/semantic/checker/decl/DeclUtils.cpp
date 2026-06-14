/**
 * @file DeclUtils.cpp
 * @brief Implementation of shared utility functions for declaration checking.
 * 
 * These functions are used by multiple declaration-specific checkers and
 * provide common operations like name extraction, attribute lookup, and
 * context conversion.
 */

#include "DeclChecker.hpp"
#include "registry/AttributeRegistry.hpp"

namespace decl {

std::string getDeclName(const DeclAST* decl, const StringPool& pool) {
    if (!decl) return "<null>";
    
    if (auto* valueDecl = decl->as<ValueDeclAST>()) {
        return std::string(pool.lookup(valueDecl->name));
    }
    if (auto* typeDecl = decl->as<TypeDeclAST>()) {
        return std::string(pool.lookup(typeDecl->name));
    }
    if (auto* packageDecl = decl->as<PackageDeclAST>()) {
        return std::string(pool.lookup(packageDecl->name));
    }
    if (auto* useDecl = decl->as<UseDeclAST>()) {
        return "<use declaration>";
    }
    if (auto* fromDecl = decl->as<FromDeclAST>()) {
        return "<from block>";
    }
    if (auto* implDecl = decl->as<ImplDeclAST>()) {
        return "<impl block>";
    }
    
    return "<anonymous declaration>";
}

DeclKeyword getDeclKeyword(const DeclAST* decl) {
    if (auto* var = decl->as<VarDeclAST>()) {
        return var->keyword;
    }
    if (auto* func = decl->as<FuncDeclAST>()) {
        return func->keyword;
    }
    // Default for non-value declarations (structs, enums, traits, etc.)
    return DeclKeyword::Let;
}

AttributeContext getAttributeContextForDecl(const DeclAST* decl) {
    if (!decl) return AttributeContext::None;
    
    switch (decl->kind) {
        case ASTKind::FuncDecl:
            return AttributeContext::Func;
            
        case ASTKind::VarDecl:
            return AttributeContext::Var;
            
        case ASTKind::StructDecl:
            return AttributeContext::Struct;
            
        case ASTKind::ImplDecl:
            return AttributeContext::Impl;
            
        case ASTKind::EnumDecl:
            return AttributeContext::Enum;
            
        case ASTKind::TraitDecl:
            return AttributeContext::Trait;
            
        case ASTKind::FromDecl:
            return AttributeContext::From;
            
        case ASTKind::TypeAliasDecl:
            return AttributeContext::TypeAlias;
            
        default:
            return AttributeContext::None;
    }
}

bool hasAttribute(const DeclAST* decl, InternedString attrId) {
    if (!decl || !attrId.isValid()) return false;
    
    for (auto* attr : decl->attributes) {
        if (attr->name == attrId) {
            return true;
        }
    }
    return false;
}

bool hasAttribute(const DeclAST* decl, std::string_view attrName, StringPool& pool) {
    InternedString id = pool.intern(std::string(attrName));
    return hasAttribute(decl, id);
}

} // namespace decl