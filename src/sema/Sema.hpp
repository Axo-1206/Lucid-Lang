#pragma once
#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "ScopeManager.hpp"
#include <unordered_map>

class ModuleAST;

struct SemaContext {
    ModuleAST* currentModule;
    ScopeManager scopeManager;
    Diagnostic& diag;
    
    // Import aliases: alias → module
    std::unordered_map<InternedString, ModuleAST*> importAliases;
    
    // Current function/loop context for validation
    FuncDeclAST* currentFunction = nullptr;
    StmtAST* currentLoop = nullptr;
    SwitchStmtAST* currentSwitch = nullptr;
    
    SemaContext(ModuleAST* mod, Diagnostic& d)
        : currentModule(mod), diag(d) {}
};