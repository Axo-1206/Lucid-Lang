/// @file execution/SymbolResolver.cpp
/// @brief Implementation of symbol resolution functions.

#include "SymbolResolver.hpp"
#include "../support/InterpreterError.hpp"

#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace interpreter {

// ─── Entry Point Resolution ─────────────────────────────────────────────

InternedString findEntryPoint(InterpreterContext& ctx, InternedString entryPoint) {
    // 1. If a specific entry point was requested, look for it first
    if (entryPoint.isValid()) {
        std::string epName = ctx.pool.lookup(entryPoint);
        
        // Scan all loaded modules for the function
        for (const auto& [id, module] : ctx.loadedModules) {
            for (DeclPtr decl : module->decls) {
                if (FuncDeclAST* func = decl->as<FuncDeclAST>()) {
                    // Check if this function matches the entry point name
                    if (func->name == entryPoint) {
                        // If found, check if it's exported (or it's the main module)
                        if (isExported(func, ctx)) {
                            return entryPoint;
                        }
                    }
                }
            }
        }
        return InternedString(); // Not found
    }

    // 2. No specific entry point - look for default names
    std::vector<std::string> defaultNames = {"main", "start", "run"};
    
    for (const std::string& name : defaultNames) {
        InternedString nameInterned = ctx.pool.intern(name);
        InternedString found = findEntryPoint(ctx, nameInterned);
        if (found.isValid()) {
            return found;
        }
    }

    return InternedString(); // No entry point found
}

InternedString findEntryPoint(InterpreterContext& ctx, const std::string& entryPoint) {
    InternedString ep = ctx.pool.intern(entryPoint);
    return findEntryPoint(ctx, ep);
}

bool isExported(const FuncDeclAST* func, InterpreterContext& ctx) {
    if (!func) return false;
    
    InternedString exportName = ctx.pool.intern("export");
    for (AttributePtr attr : func->attributes) {
        if (attr->name == exportName) {
            return true;
        }
    }
    return false;
}

bool isEntryPointCandidate(const FuncDeclAST* func, InterpreterContext& ctx) {
    if (!func) return false;
    
    // Entry point must be exported
    if (!isExported(func, ctx)) {
        return false;
    }
    
    // Entry point must have a compatible signature
    const FuncTypeAST* funcType = func->funcType;
    if (!funcType) return false;
    
    // Must have no generic parameters
    if (!func->genericParams.empty()) {
        return false;
    }
    
    // Must have no parameters (entry point takes no arguments)
    if (!funcType->params.empty()) {
        return false;
    }
    
    // Must return int or void
    if (funcType->returnType) {
        if (funcType->returnType->isa<PrimitiveTypeAST>()) {
            PrimitiveTypeAST* prim = funcType->returnType->as<PrimitiveTypeAST>();
            PrimitiveKind kind = prim->primitiveKind;
            if (kind == PrimitiveKind::Int || kind == PrimitiveKind::Int32 ||
                kind == PrimitiveKind::Int64) {
                return true;
            }
        }
        return false;
    }
    
    // No return type means void
    return true;
}

std::vector<const FuncDeclAST*> getEntryPointCandidates(InterpreterContext& ctx) {
    std::vector<const FuncDeclAST*> candidates;
    
    for (const auto& [id, module] : ctx.loadedModules) {
        for (DeclPtr decl : module->decls) {
            if (FuncDeclAST* func = decl->as<FuncDeclAST>()) {
                if (isEntryPointCandidate(func, ctx)) {
                    candidates.push_back(func);
                }
            }
        }
    }
    
    return candidates;
}

// ─── Name Mangling ──────────────────────────────────────────────────────

InternedString getMangledName(const FuncDeclAST* func, InterpreterContext& ctx) {
    if (!func) return InternedString();
    
    // If the function already has a mangled name, use it
    if (func->mangledName.isValid()) {
        return func->mangledName;
    }
    
    // Otherwise, use the original name
    return func->name;
}

} // namespace interpreter