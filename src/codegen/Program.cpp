/// @file codegen/Program.cpp
/// @brief Implementation of per-program state.

#include "Program.hpp"
#include "codegen/LLVMTypeHelpers.hpp"
#include "codegen/emit/Emitter.hpp"
#include "emit/Emitter.hpp"
#include "ownership/Ownership.hpp"

#include "core/ast/DeclAST.hpp"

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
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
    , abi_(*this)
{
    assert(llvmCtx_ && "ProgramState constructed with null LLVMContext");
    assert(module_ && "ProgramState constructed with null module");

    // Components that need a fully-constructed ProgramState are built in
    // the constructor body, after all members are initialized. They take
    // `*this` by reference and store it; the reference is valid from this
    // point on because the object is fully constructed.
    ownership_ = std::make_unique<Ownership>(*this);
    emitter_ = std::make_unique<Emitter>(*this);
}

ProgramState::~ProgramState() = default;

Emitter& ProgramState::emitter() {
    return *emitter_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Current Function State
// ─────────────────────────────────────────────────────────────────────────────

void ProgramState::setCurrentFunctionState(llvm::Function* fn,
                                            TypeAST* returnType) {
    assert(!currentFunctionState
           && "setCurrentFunctionState() called with a function already "
              "active — clearCurrentFunctionState() first");

    currentFunctionStateOwned_ =
        std::make_unique<FunctionState>(*this, fn, returnType);
    currentFunctionState = currentFunctionStateOwned_.get();
}

void ProgramState::clearCurrentFunctionState() {
    // Reset the pointer BEFORE destroying the FunctionState, so
    // FunctionState's destructor doesn't see a stale `currentFunctionState`
    // pointing at itself.
    currentFunctionState = nullptr;
    currentFunctionStateOwned_.reset();
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
// Closure Caches
// ─────────────────────────────────────────────────────────────────────────────

void ProgramState::storeClosureEnvType(AnonFuncExprAST* expr,
                                       llvm::StructType* ty) {
    assert(expr && "storeClosureEnvType() with null expression");
    assert(ty && "storeClosureEnvType() with null type");
    closureEnvTypes_[expr] = ty;
}

llvm::StructType* ProgramState::lookupClosureEnvType(
    AnonFuncExprAST* expr) const {
    if (!expr) return nullptr;
    auto it = closureEnvTypes_.find(expr);
    return it != closureEnvTypes_.end() ? it->second : nullptr;
}

void ProgramState::storeClosureFunction(AnonFuncExprAST* expr,
                                        llvm::Function* fn) {
    assert(expr && "storeClosureFunction() with null expression");
    assert(fn && "storeClosureFunction() with null function");
    closureFunctions_[expr] = fn;
}

llvm::Function* ProgramState::lookupClosureFunction(
    AnonFuncExprAST* expr) const {
    if (!expr) return nullptr;
    auto it = closureFunctions_.find(expr);
    return it != closureFunctions_.end() ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Module Symbol Naming
// ─────────────────────────────────────────────────────────────────────────────

std::string ProgramState::moduleStateSymbol(ModuleAST* module) const {
    if (!module) return {};
    return "__module_state_" + sanitizeForLLVMSymbol(
        pool.lookup(module->filePath));
}

std::string ProgramState::moduleInitSymbol(ModuleAST* module) const {
    if (!module) return {};
    return "__init_module_" + sanitizeForLLVMSymbol(
        pool.lookup(module->filePath));
}

std::string ProgramState::moduleFreeSymbol(ModuleAST* module) const {
    if (!module) return {};
    return "__free_module_" + sanitizeForLLVMSymbol(
        pool.lookup(module->filePath));
}

std::string ProgramState::moduleSizeSymbol(ModuleAST* module) const {
    if (!module) return {};
    return "__module_size_" + sanitizeForLLVMSymbol(
        pool.lookup(module->filePath));
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