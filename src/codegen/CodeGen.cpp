// CodeGen.cpp
#include "CodeGen.hpp"
#include "CodeGenContext.hpp"

namespace codegen {

CodeGen::CodeGen(llvm::LLVMContext& context) : m_context(context) {}
CodeGen::~CodeGen() = default;

std::vector<std::unique_ptr<llvm::Module>> CodeGen::generate(
    const std::vector<ModuleAST*>& modules
) {
    std::vector<std::unique_ptr<llvm::Module>> result;
    
    for (ModuleAST* module : modules) {
        CodeGenContext ctx(m_context);
        
        // Create LLVM module
        std::string name = StringPool::instance().lookup(module->filePath);
        ctx.module = new llvm::Module(name, m_context);
        
        auto irModule = generateModule(module, ctx);
        if (irModule) {
            result.push_back(std::move(irModule));
        }
    }
    
    return result;
}

std::unique_ptr<llvm::Module> CodeGen::generateModule(
    ModuleAST* module, 
    CodeGenContext& ctx
) {
    // ─── Phase 1: Lower declarations ──────────────────────────────────
    for (const DeclPtr decl : module->decls) {
        lowerDeclaration(decl, ctx);
    }
    
    // ─── Phase 2: Lower function bodies ──────────────────────────────
    for (const DeclPtr decl : module->decls) {
        if (decl->isa<FuncDeclAST>()) {
            lowerFunctionBody(decl->as<FuncDeclAST>(), ctx);
        }
    }
    
    return std::unique_ptr<llvm::Module>(ctx.module);
}

} // namespace codegen