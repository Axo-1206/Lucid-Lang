/// @file CodeGenContext.hpp
/// @brief Code generation context - LLVM state only.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <unordered_map>
#include <vector>

namespace codegen {

/// @brief Code generation context - LLVM state only.
struct CodeGenContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    DiagnosticEngine& diagnostics;
    llvm::LLVMContext& llvmCtx;
    
    // ─── LLVM Module and Builder ────────────────────────────────────────
    
    llvm::Module* module = nullptr;
    llvm::IRBuilder<> builder;
    
    // ─── Type Cache ─────────────────────────────────────────────────────
    
    std::unordered_map<const TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<const StructDeclAST*, llvm::StructType*> structCache;
    
    // ─── Symbol Mapping: AST → LLVM Value ──────────────────────────────
    
    std::unordered_map<const ValueDeclAST*, llvm::Value*> values;
    
    // ─── Function Mapping: AST → LLVM Function ─────────────────────────
    
    std::unordered_map<const FuncDeclAST*, llvm::Function*> functions;
    
    // ─── Runtime Function Mapping ──────────────────────────────────────
    
    std::unordered_map<std::string, llvm::Function*> runtimeFunctions;
    
    // ─── Loop Info (for break/continue) ─────────────────────────────────
    
    struct LoopInfo {
        llvm::BasicBlock* header = nullptr;
        llvm::BasicBlock* exit = nullptr;
        llvm::BasicBlock* continueTarget = nullptr;
    };
    std::vector<LoopInfo> loops;
    
    // ─── Current Function ───────────────────────────────────────────────
    
    llvm::Function* currentFunction = nullptr;
    llvm::BasicBlock* returnBlock = nullptr;
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    CodeGenContext(StringPool& p, DiagnosticEngine& d, llvm::LLVMContext& ctx)
        : pool(p)
        , diagnostics(d)
        , llvmCtx(ctx)
        , builder(ctx) {}
    
    CodeGenContext(const CodeGenContext&) = delete;
    CodeGenContext& operator=(const CodeGenContext&) = delete;
    
    // ─── Value Helpers ──────────────────────────────────────────────────
    
    void storeValue(const ValueDeclAST* decl, llvm::Value* value) {
        values[decl] = value;
    }
    
    llvm::Value* lookupValue(const ValueDeclAST* decl) const {
        auto it = values.find(decl);
        return it != values.end() ? it->second : nullptr;
    }
    
    bool hasValue(const ValueDeclAST* decl) const {
        return values.find(decl) != values.end();
    }
    
    // ─── Function Helpers ──────────────────────────────────────────────────
    
    void storeFunction(const FuncDeclAST* decl, llvm::Function* func) {
        functions[decl] = func;
    }
    
    llvm::Function* lookupFunction(const FuncDeclAST* decl) const {
        auto it = functions.find(decl);
        return it != functions.end() ? it->second : nullptr;
    }
    
    // ─── Current Function Helpers ──────────────────────────────────────
    
    void setCurrentFunction(llvm::Function* func) {
        currentFunction = func;
    }
    
    llvm::Function* getCurrentFunction() const {
        return currentFunction;
    }
    
    // ─── Runtime Function Helpers ──────────────────────────────────────
    
    llvm::Function* getRuntimeFunction(const std::string& name) const {
        auto it = runtimeFunctions.find(name);
        return it != runtimeFunctions.end() ? it->second : nullptr;
    }
    
    void setRuntimeFunction(const std::string& name, llvm::Function* func) {
        runtimeFunctions[name] = func;
    }
    
    // ─── Loop Helpers ──────────────────────────────────────────────────
    
    void pushLoop(llvm::BasicBlock* header, llvm::BasicBlock* exit,
                  llvm::BasicBlock* continueTarget = nullptr) {
        loops.push_back({header, exit, continueTarget});
    }
    
    void popLoop() {
        if (!loops.empty()) loops.pop_back();
    }
    
    LoopInfo* currentLoop() {
        return loops.empty() ? nullptr : &loops.back();
    }
    
    bool insideLoop() const {
        return !loops.empty();
    }
    
    // ─── Type Helpers ──────────────────────────────────────────────────
    
    void cacheType(const TypeAST* lucidType, llvm::Type* llvmType) {
        typeCache[lucidType] = llvmType;
    }
    
    llvm::Type* lookupType(const TypeAST* lucidType) const {
        auto it = typeCache.find(lucidType);
        return it != typeCache.end() ? it->second : nullptr;
    }
    
    void cacheStruct(const StructDeclAST* decl, llvm::StructType* structType) {
        structCache[decl] = structType;
    }
    
    llvm::StructType* lookupStruct(const StructDeclAST* decl) const {
        auto it = structCache.find(decl);
        return it != structCache.end() ? it->second : nullptr;
    }
};

} // namespace codegen