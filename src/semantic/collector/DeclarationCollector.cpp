/**
 * @file DeclarationCollector.cpp
 * @brief Implementation of Phase 1 declaration collector.
 */

#include "DeclarationCollector.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Constructor
// ============================================================================

DeclarationCollector::DeclarationCollector(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("DeclarationCollector constructed");
}

// ============================================================================
// Main Entry Point
// ============================================================================

void DeclarationCollector::collectProgram(ProgramAST* program) {
    if (!program) return;
    
    // Get absolute file path for tracking
    std::string filePath = std::string(ctx_.pool.lookup(program->filePath));
    
    // Skip already processed files
    if (processedFiles_.find(filePath) != processedFiles_.end()) {
        LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: skipping already processed file: " << filePath);
        return;
    }
    processedFiles_.insert(filePath);
    
    // Initialize per-file import tracking
    fileImports_[program->filePath] = std::unordered_set<std::string>();
    
    LUC_LOG_SEMANTIC_VERBOSE("DeclarationCollector: collecting declarations from: " << filePath);
    
    // Process each top-level declaration
    for (auto& decl : program->decls) {
        switch (decl->kind) {
            case ASTKind::UseDecl:
                collectUseDecl(decl->as<UseDeclAST>());
                break;
            case ASTKind::VarDecl:
                collectVarDecl(decl->as<VarDeclAST>());
                break;
            case ASTKind::FuncDecl:
                collectFuncDecl(decl->as<FuncDeclAST>());
                break;
            case ASTKind::StructDecl:
                collectStructDecl(decl->as<StructDeclAST>());
                break;
            case ASTKind::EnumDecl:
                collectEnumDecl(decl->as<EnumDeclAST>());
                break;
            case ASTKind::TraitDecl:
                collectTraitDecl(decl->as<TraitDeclAST>());
                break;
            case ASTKind::ImplDecl:
                collectImplDecl(decl->as<ImplDeclAST>());
                break;
            case ASTKind::FromDecl:
                collectFromDecl(decl->as<FromDeclAST>());
                break;
            case ASTKind::TypeAliasDecl:
                collectTypeAliasDecl(decl->as<TypeAliasDeclAST>());
                break;
            default:
                // PackageDecl, etc. – not registered
                LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: skipping declaration kind: "
                                         << LucDebug::kindToString(decl->kind));
                break;
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("DeclarationCollector: finished collecting from: " << filePath);
}

// ============================================================================
// Use Declaration (import detection only, not registered)
// ============================================================================

void DeclarationCollector::collectUseDecl(UseDeclAST* use) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting UseDecl");
    
    // Build path string
    std::string path;
    for (size_t i = 0; i < use->path.size(); ++i) {
        if (i > 0) path += '.';
        path += ctx_.pool.lookup(use->path[i]);
    }
    
    // Check for duplicate import within the same file
    checkDuplicateUse(ctx_.currentFile, path, use->loc);
}

void DeclarationCollector::checkDuplicateUse(InternedString fileName, 
                                              const std::string& path, 
                                              const SourceLocation& loc) {
    auto& imports = fileImports_[fileName];
    if (!imports.insert(path).second) {
        ctx_.error(loc, DiagCode::E2005, "duplicate import of '", path, "'");
    }
}

// ============================================================================
// Value Declarations (registered in value namespace)
// ============================================================================

void DeclarationCollector::collectVarDecl(VarDeclAST* var) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting VarDecl: " 
                             << ctx_.pool.lookup(var->name));
    
    // Set file path
    var->file = ctx_.currentFile;
    
    // Register in value namespace
    if (!ctx_.scope.declareValue(var)) {
        ctx_.error(var->loc, DiagCode::E2005, 
                   "variable '", ctx_.pool.lookup(var->name), "' already declared in this scope");
    }
}

void DeclarationCollector::collectFuncDecl(FuncDeclAST* func) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting FuncDecl: " 
                             << ctx_.pool.lookup(func->name));
    
    // Set file path
    func->file = ctx_.currentFile;
    
    // Register as overload (even if only one, this handles the first registration)
    if (!ctx_.scope.declareOverload(func)) {
        // If declareOverload fails, try normal value declaration (non-overload)
        if (!ctx_.scope.declareValue(func)) {
            ctx_.error(func->loc, DiagCode::E2005,
                       "function '", ctx_.pool.lookup(func->name), 
                       "' already declared in this scope");
        }
    }
    
    // Register parameters in function's own scope (for body resolution)
    collectFunctionParams(func);
}

void DeclarationCollector::collectFunctionParams(FuncDeclAST* func) {
    if (!func->funcType) return;
    
    // Push scope for function parameters
    ctx_.scope.push();
    
    // Register each parameter
    for (auto* param : func->funcType->params) {
        if (!param) continue;
        
        param->file = ctx_.currentFile;
        
        if (!ctx_.scope.declareValue(param)) {
            ctx_.error(param->loc, DiagCode::E2005,
                       "parameter '", ctx_.pool.lookup(param->name), 
                       "' already declared in this function");
        }
    }
    
    // Note: We don't pop the scope here because the function body will
    // be checked in Phase 3 with this scope active. The scope will be
    // popped after checking the function body.
    // For now, we leave it on the stack – it will be popped later.
}

// ============================================================================
// Type Declarations (registered in type namespace)
// ============================================================================

void DeclarationCollector::collectStructDecl(StructDeclAST* structDecl) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting StructDecl: " 
                             << ctx_.pool.lookup(structDecl->name));
    
    // Set file path
    structDecl->file = ctx_.currentFile;
    
    // Create self-type reference
    ensureSelfType(structDecl);
    
    // Register in type namespace
    if (!ctx_.scope.declareType(structDecl)) {
        ctx_.error(structDecl->loc, DiagCode::E2005,
                   "type '", ctx_.pool.lookup(structDecl->name), 
                   "' already declared in this scope");
    }
    
    // Process fields in struct's own scope
    collectStructFields(structDecl);
}

void DeclarationCollector::collectStructFields(StructDeclAST* structDecl) {
    // Push scope for struct fields
    ctx_.scope.push();
    
    // Register each field in the struct's scope
    for (auto* field : structDecl->fields) {
        if (!field) continue;
        
        field->file = ctx_.currentFile;
        
        if (!ctx_.scope.declareValue(field)) {
            ctx_.error(field->loc, DiagCode::E2005,
                       "field '", ctx_.pool.lookup(field->name), 
                       "' already declared in struct '", 
                       ctx_.pool.lookup(structDecl->name), "'");
        }
    }
    
    // Pop struct field scope – fields are only accessible via dot notation,
    // not by direct name lookup outside the struct.
    ctx_.scope.pop();
}

void DeclarationCollector::collectEnumDecl(EnumDeclAST* enumDecl) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting EnumDecl: " 
                             << ctx_.pool.lookup(enumDecl->name));
    
    // Set file path
    enumDecl->file = ctx_.currentFile;
    
    // Create self-type reference
    ensureSelfType(enumDecl);
    
    // Register in type namespace
    if (!ctx_.scope.declareType(enumDecl)) {
        ctx_.error(enumDecl->loc, DiagCode::E2005,
                   "type '", ctx_.pool.lookup(enumDecl->name), 
                   "' already declared in this scope");
    }
    
    // Process variants in enum's own scope
    collectEnumVariants(enumDecl);
}

void DeclarationCollector::collectEnumVariants(EnumDeclAST* enumDecl) {
    // Push scope for enum variants
    ctx_.scope.push();
    
    // Register each variant in the enum's scope
    for (auto* variant : enumDecl->variants) {
        if (!variant) continue;
        
        variant->file = ctx_.currentFile;
        
        if (!ctx_.scope.declareValue(variant)) {
            ctx_.error(variant->loc, DiagCode::E2005,
                       "variant '", ctx_.pool.lookup(variant->name), 
                       "' already declared in enum '", 
                       ctx_.pool.lookup(enumDecl->name), "'");
        }
    }
    
    // Pop enum variant scope – variants are accessed via EnumName.Variant,
    // not by direct name lookup outside the enum.
    ctx_.scope.pop();
}

void DeclarationCollector::collectTraitDecl(TraitDeclAST* trait) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting TraitDecl: " 
                             << ctx_.pool.lookup(trait->name));
    
    // Set file path
    trait->file = ctx_.currentFile;
    
    // Create self-type reference
    ensureSelfType(trait);
    
    // Register in type namespace
    if (!ctx_.scope.declareType(trait)) {
        ctx_.error(trait->loc, DiagCode::E2005,
                   "type '", ctx_.pool.lookup(trait->name), 
                   "' already declared in this scope");
    }
}

void DeclarationCollector::collectTypeAliasDecl(TypeAliasDeclAST* alias) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting TypeAliasDecl: " 
                             << ctx_.pool.lookup(alias->name));
    
    // Set file path
    alias->file = ctx_.currentFile;
    
    // Create self-type reference
    ensureSelfType(alias);
    
    // Register in type namespace
    if (!ctx_.scope.declareType(alias)) {
        ctx_.error(alias->loc, DiagCode::E2005,
                   "type '", ctx_.pool.lookup(alias->name), 
                   "' already declared in this scope");
    }
}

// ============================================================================
// Behaviour Declarations (not registered, just collected)
// ============================================================================

void DeclarationCollector::collectImplDecl(ImplDeclAST* impl) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting ImplDecl");
    
    impl->file = ctx_.currentFile;
    
    // Store for later processing (trait conformance, method resolution)
    impls_.push_back(impl);
}

void DeclarationCollector::collectFromDecl(FromDeclAST* from) {
    LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: collecting FromDecl");
    
    from->file = ctx_.currentFile;
    
    // Store for later processing (implicit conversion resolution)
    froms_.push_back(from);
}

// ============================================================================
// Helpers
// ============================================================================

void DeclarationCollector::ensureSelfType(TypeDeclAST* typeDecl) {
    if (!typeDecl->selfType) {
        typeDecl->selfType = ctx_.arena.make<NamedTypeAST>(typeDecl->name);
        typeDecl->selfType->loc = typeDecl->loc;
        LUC_LOG_SEMANTIC_EXTREME("DeclarationCollector: created self-type for: "
                                 << ctx_.pool.lookup(typeDecl->name));
    }
}
