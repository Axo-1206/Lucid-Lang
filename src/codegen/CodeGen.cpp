/// @file CodeGen.cpp
/// @brief Implementation of the main code generation orchestrator.

#include "CodeGen.hpp"
#include "debug/DebugUtils.hpp"
#include "core/memory/StringPool.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace codegen {

// =============================================================================
// Public API
// =============================================================================

std::vector<std::unique_ptr<llvm::Module>> generate(
    const std::vector<ModuleAST*>& modules,
    StringPool& p, DiagnosticEngine& d,
    llvm::LLVMContext& context
) {
    std::vector<std::unique_ptr<llvm::Module>> result;
    result.reserve(modules.size());

    for (ModuleAST* module : modules) {
        if (!module) continue;

        CodeGenContext ctx(p, d, context);
        
        // ─── Create LLVM module ──────────────────────────────────────────
        std::string name = StringPool::instance().lookup(module->filePath);
        ctx.module = new llvm::Module(name, context);
        
        // ─── Generate IR for the module ────────────────────────────────
        auto irModule = generateModule(module, ctx);
        if (irModule) {
            result.push_back(std::move(irModule));
        }
    }

    return result;
}

std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) {
        return nullptr;
    }

    LOG_CODEGEN("Generating IR for module: ", 
                StringPool::instance().lookup(module->filePath));

    // ─── Phase 1: Lower all declarations ──────────────────────────────────
    // This creates LLVM types, function prototypes, globals, and structs.
    // Function bodies are NOT lowered yet.
    lowerModuleDeclarations(module, ctx);

    // ─── Phase 2: Lower all function bodies ──────────────────────────────
    // Now that all declarations exist, we can lower function bodies
    // with full symbol resolution (forward references work).
    lowerModuleBodies(module, ctx);

    // ─── Phase 3: Verify the module ──────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyModule(*ctx.module, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, module->loc,
                                "LLVM IR verification failed: ", error);
        return nullptr;
    }

    LOG_CODEGEN("Generated IR successfully for module");
    return std::unique_ptr<llvm::Module>(ctx.module);
}

// =============================================================================
// Module-Level Emission
// =============================================================================

void lowerModuleDeclarations(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // ─── Lower all declarations except function bodies ──────────────
        // For functions, this creates the prototype but not the body.
        switch (decl->kind) {
            case ASTKind::ImportDecl:
                // Imports are handled by the module resolver, not CodeGen.
                break;

            case ASTKind::FuncDecl:
                lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
                break;

            case ASTKind::StructDecl:
                lowerStructDecl(decl->as<StructDeclAST>(), ctx);
                break;

            case ASTKind::EnumDecl:
                lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
                break;

            case ASTKind::VarDecl:
                lowerVarDecl(decl->as<VarDeclAST>(), ctx);
                break;

            default:
                // Other declaration kinds don't need special handling
                // in the declaration phase.
                break;
        }
    }
}

void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // ─── Lower function bodies ───────────────────────────────────────
        if (decl->isa<FuncDeclAST>()) {
            lowerFunctionBody(decl->as<FuncDeclAST>(), ctx);
        }
        // Note: Struct and enum bodies are already handled in the
        // declaration phase. Variables with initializers are handled
        // in lowerVarDecl.
    }
}

} // namespace codegen