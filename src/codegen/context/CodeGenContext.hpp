// CodeGenContext.hpp
#pragma once

#include "core/ast/BaseAST.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <unordered_map>
#include <vector>

namespace codegen {

/// @brief Simple stack frame for variable tracking.
struct CodeGenFrame {
    std::unordered_map<const ValueDeclAST*, llvm::Value*> values;
    CodeGenFrame* parent = nullptr;
    
    void insert(const ValueDeclAST* decl, llvm::Value* value) {
        values[decl] = value;
    }
    
    llvm::Value* lookup(const ValueDeclAST* decl) const {
        auto it = values.find(decl);
        if (it != values.end()) return it->second;
        if (parent) return parent->lookup(decl);
        return nullptr;
    }
};

/// @brief Simple stack with push/pop for scopes.
struct CodeGenStack {
    std::vector<CodeGenFrame*> frames;
    
    void push() {
        auto* frame = new CodeGenFrame();
        if (!frames.empty()) {
            frame->parent = frames.back();
        }
        frames.push_back(frame);
    }
    
    void pop() {
        if (!frames.empty()) {
            delete frames.back();
            frames.pop_back();
        }
    }
    
    void insert(const ValueDeclAST* decl, llvm::Value* value) {
        if (!frames.empty()) {
            frames.back()->insert(decl, value);
        }
    }
    
    llvm::Value* lookup(const ValueDeclAST* decl) const {
        if (frames.empty()) return nullptr;
        return frames.back()->lookup(decl);
    }
    
    CodeGenFrame* current() const {
        return frames.empty() ? nullptr : frames.back();
    }
};

/// @brief Simple context for code generation.
struct CodeGenContext {
    llvm::LLVMContext& llvmCtx;
    llvm::Module* module = nullptr;
    llvm::IRBuilder<> builder;
    
    // Type cache
    std::unordered_map<const TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<const StructDeclAST*, llvm::StructType*> structCache;
    
    // Value tracking
    CodeGenStack scope;
    
    // Function tracking
    std::unordered_map<const FuncDeclAST*, llvm::Function*> functions;
    std::unordered_map<InternedString, llvm::Function*> foreignFunctions;
    
    // Loop tracking (for break/continue)
    struct LoopInfo {
        llvm::BasicBlock* header = nullptr;
        llvm::BasicBlock* exit = nullptr;
        llvm::BasicBlock* continueTarget = nullptr;
    };
    std::vector<LoopInfo> loops;
    
    // Current function (for return)
    llvm::Function* currentFunction = nullptr;
    llvm::BasicBlock* currentReturnBlock = nullptr;
    
    CodeGenContext(llvm::LLVMContext& ctx) 
        : llvmCtx(ctx), builder(ctx) {}
    
    // ─── Type Helpers ──────────────────────────────────────────────────
    llvm::Type* getType(const TypeAST* type);
    llvm::StructType* getStructType(const StructDeclAST* decl);
    llvm::FunctionType* getFunctionType(const FuncTypeAST* funcType);
    
    // ─── Scope Helpers ─────────────────────────────────────────────────
    void pushScope() { scope.push(); }
    void popScope() { scope.pop(); }
    void insertValue(const ValueDeclAST* decl, llvm::Value* value) {
        scope.insert(decl, value);
    }
    llvm::Value* lookupValue(const ValueDeclAST* decl) const {
        return scope.lookup(decl);
    }
    
    // ─── Loop Helpers ─────────────────────────────────────────────────
    void pushLoop(llvm::BasicBlock* header, llvm::BasicBlock* exit, 
                  llvm::BasicBlock* continueTarget = nullptr) {
        loops.push_back({header, exit, continueTarget});
    }
    void popLoop() { if (!loops.empty()) loops.pop_back(); }
    LoopInfo* currentLoop() { return loops.empty() ? nullptr : &loops.back(); }
    
    // ─── Function Helpers ─────────────────────────────────────────────
    void setCurrentFunction(llvm::Function* func) { currentFunction = func; }
    llvm::Function* getCurrentFunction() const { return currentFunction; }
    
    // ─── Builder Helpers ──────────────────────────────────────────────
    llvm::IRBuilder<>& builder() { return builder; }
};

} // namespace codegen