/// @file codegen/Program.cpp
/// @brief Implementation of per-program state.

#include "Program.hpp"
#include "ownership/Ownership.hpp"

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

ProgramState::ProgramState(StringPool& pool_,
                           DiagnosticEngine& diagnostics_,
                           std::unique_ptr<llvm::LLVMContext> llvmCtx,
                           std::string moduleName)
    : pool(pool_)
    , diagnostics(diagnostics_)
    , llvmCtx_(std::move(llvmCtx))
    , module_(std::make_unique<llvm::Module>(moduleName, *llvmCtx_))
    , builder_(*llvmCtx_)
    , types_(*llvmCtx_, *module_, pool_)
    , abi_(*module_, types_)
{
    assert(llvmCtx_ && "ProgramState constructed with null LLVMContext");
    assert(module_ && "ProgramState constructed with null module");

    // Ownership is constructed in the body, not the initializer list,
    // because it needs a reference to *this and must be constructed after
    // the other members are initialized.
    ownership_ = std::make_unique<Ownership>(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// Function Table
// ─────────────────────────────────────────────────────────────────────────────

void ProgramState::storeFunction(FuncDeclAST* decl, llvm::Function* fn) {
    assert(decl && "storeFunction() with null decl");
    assert(fn && "storeFunction() with null fn");
    functions_[decl] = fn;
}

llvm::Function* ProgramState::lookupFunction(FuncDeclAST* decl) const {
    if (!decl) return nullptr;
    auto it = functions_.find(decl);
    return it != functions_.end() ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Drop-Glue Cache
// ─────────────────────────────────────────────────────────────────────────────

void ProgramState::storeDropGlue(TypeAST* type, llvm::Function* fn) {
    assert(type && "storeDropGlue() with null type");
    assert(fn && "storeDropGlue() with null fn");
    dropGlue_[type] = fn;
}

llvm::Function* ProgramState::lookupDropGlue(TypeAST* type) const {
    if (!type) return nullptr;
    auto it = dropGlue_.find(type);
    return it != dropGlue_.end() ? it->second : nullptr;
}

void ProgramState::storeCopyGlue(TypeAST* type, llvm::Function* fn) {
    assert(type && "storeCopyGlue() with null type");
    assert(fn && "storeCopyGlue() with null fn");
    copyGlue_[type] = fn;
}

llvm::Function* ProgramState::lookupCopyGlue(TypeAST* type) const {
    if (!type) return nullptr;
    auto it = copyGlue_.find(type);
    return it != copyGlue_.end() ? it->second : nullptr;
}

} // namespace codegen