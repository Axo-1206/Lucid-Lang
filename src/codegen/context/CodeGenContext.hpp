/// @file CodeGenContext.hpp
/// @brief Simple context for code generation - monolithic design.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
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
/// 
/// This context is passed to all code generation functions. It holds:
///   - Resources: StringPool, DiagnosticEngine, LLVM context
///   - Type cache: Maps Lucid types to LLVM types
///   - Value tracking: Maps declarations to LLVM values
///   - Function tracking: Maps declarations to LLVM functions
///   - Loop tracking: For break/continue
///   - Current function: For return statements
struct CodeGenContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    DiagnosticEngine& diagnostics;
    llvm::LLVMContext& llvmCtx;
    
    // ─── LLVM Module ────────────────────────────────────────────────────
    
    llvm::Module* module = nullptr;
    llvm::IRBuilder<> builder;
    
    // ─── Type Cache ────────────────────────────────────────────────────
    
    std::unordered_map<const TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<const StructDeclAST*, llvm::StructType*> structCache;
    
    // ─── Value Tracking ────────────────────────────────────────────────
    
    CodeGenStack scope;
    
    // ─── Function Tracking ─────────────────────────────────────────────
    
    std::unordered_map<const FuncDeclAST*, llvm::Function*> functions;
    std::unordered_map<InternedString, llvm::Function*> foreignFunctions;
    
    // ─── Runtime Function Tracking ────────────────────────────────────
    
    std::unordered_map<std::string, llvm::Function*> runtimeFunctions;
    
    // ─── Loop Tracking (for break/continue) ───────────────────────────
    
    struct LoopInfo {
        llvm::BasicBlock* header = nullptr;
        llvm::BasicBlock* exit = nullptr;
        llvm::BasicBlock* continueTarget = nullptr;
    };
    std::vector<LoopInfo> loops;
    
    // ─── Current Function (for return) ────────────────────────────────
    
    llvm::Function* currentFunction = nullptr;
    llvm::BasicBlock* currentReturnBlock = nullptr;
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    CodeGenContext(StringPool& p, DiagnosticEngine& d, llvm::LLVMContext& ctx)
        : pool(p)
        , diagnostics(d)
        , llvmCtx(ctx)
        , builder(ctx) {}
    
    CodeGenContext(const CodeGenContext&) = delete;
    CodeGenContext& operator=(const CodeGenContext&) = delete;
    
    // ─── Runtime Function Helpers ─────────────────────────────────────
    
    llvm::Function* getRuntimeFunction(const std::string& name) {
        auto it = runtimeFunctions.find(name);
        if (it != runtimeFunctions.end()) {
            return it->second;
        }
        return nullptr;
    }
    
    void setRuntimeFunction(const std::string& name, llvm::Function* func) {
        runtimeFunctions[name] = func;
    }
    
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
};

} // namespace codegen