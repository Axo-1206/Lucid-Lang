// CodeGen.hpp
#pragma once

#include "core/ast/BaseAST.hpp"
#include "context/CodeGenContext.hpp"
#include <llvm/IR/LLVMContext.h>
#include <memory>
#include <vector>

namespace codegen {

class CodeGen {
public:
    CodeGen(llvm::LLVMContext& context);
    ~CodeGen();
    
    /// @brief Generate IR for all modules.
    std::vector<std::unique_ptr<llvm::Module>> generate(
        const std::vector<ModuleAST*>& modules
    );
    
    /// @brief Generate IR for a single module.
    std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx);

private:
    llvm::LLVMContext& m_context;
};

} // namespace codegen