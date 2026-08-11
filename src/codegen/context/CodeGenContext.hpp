/// @file CodeGenContext.hpp
/// @brief Code generation context - LLVM state only.
///
/// This context holds LLVM IR state and mapping from AST nodes
/// to LLVM values. It does NOT duplicate semantic analysis results.
///
/// ─── Responsibilities ──────────────────────────────────────────────────────
///   1. LLVM module, builder, and current insertion point
///   2. AST declaration → LLVM value mapping (allocas, globals, functions)
///   3. Type cache (Lucid type → LLVM type) for performance
///   4. Loop info for break/continue lowering
///   5. Current function for return lowering

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
///
/// This context is passed to all code generation functions. It holds
/// only what's needed for IR generation, not semantic analysis results.
struct CodeGenContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    DiagnosticEngine& diagnostics;
    llvm::LLVMContext& llvmCtx;
    
    // ─── LLVM Module and Builder ────────────────────────────────────────
    
    llvm::Module* module = nullptr;     // The current LLVM module
    llvm::IRBuilder<> builder;          // IR builder for emitting instructions
    
    // ─── Type Cache (performance optimization) ─────────────────────────
    
    std::unordered_map<const TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<const StructDeclAST*, llvm::StructType*> structCache;
    
    // ─── Symbol Mapping: AST → LLVM Value ──────────────────────────────
    //
    // This is the ONLY mapping needed. Variables are looked up by their
    // declaration node, not by name. This is simpler and safer.
    //
    // For l-values (variables, parameters), this stores the alloca pointer.
    // For r-values (constants, temporaries), this stores the computed value.
    std::unordered_map<const ValueDeclAST*, llvm::Value*> values;
    
    // ─── Function Mapping: AST → LLVM Function ─────────────────────────
    //
    // Each function declaration maps to its LLVM function.
    // This is populated in the declaration phase and used in the body phase.
    std::unordered_map<const FuncDeclAST*, llvm::Function*> functions;
    
    // ─── Loop Info (for break/continue) ─────────────────────────────────
    //
    // Stack of loop contexts. Each context stores the header, exit, and
    // continue targets for the current loop.
    struct LoopInfo {
        llvm::BasicBlock* header = nullptr;           // Loop header (for continue)
        llvm::BasicBlock* exit = nullptr;             // Loop exit (for break)
        llvm::BasicBlock* continueTarget = nullptr;   // Where continue jumps to
    };
    std::vector<LoopInfo> loops;
    
    // ─── Current Function ───────────────────────────────────────────────
    //
    // Used for return statements to know which function to return from.
    // Also stores the return block for generating phi nodes if needed.
    llvm::Function* currentFunction = nullptr;
    llvm::BasicBlock* returnBlock = nullptr;          // Optional: block for phi nodes
    
    // ─── Current Insertion Point ────────────────────────────────────────
    //
    // These are convenience accessors for the builder's current state.
    // The builder itself tracks the current block and insertion point.
    
    llvm::BasicBlock* currentBlock() const {
        return builder.GetInsertBlock();
    }
    
    void setInsertPoint(llvm::BasicBlock* block) {
        builder.SetInsertPoint(block);
    }
    
    void setInsertPoint(llvm::BasicBlock* block, llvm::BasicBlock::iterator it) {
        builder.SetInsertPoint(block, it);
    }
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    CodeGenContext(StringPool& p, DiagnosticEngine& d, llvm::LLVMContext& ctx)
        : pool(p)
        , diagnostics(d)
        , llvmCtx(ctx)
        , builder(ctx) {}
    
    CodeGenContext(const CodeGenContext&) = delete;
    CodeGenContext& operator=(const CodeGenContext&) = delete;
    
    // ─── Value Helpers ──────────────────────────────────────────────────
    
    /// @brief Store a value for a declaration.
    void storeValue(const ValueDeclAST* decl, llvm::Value* value) {
        values[decl] = value;
    }
    
    /// @brief Look up a value for a declaration.
    llvm::Value* lookupValue(const ValueDeclAST* decl) const {
        auto it = values.find(decl);
        return it != values.end() ? it->second : nullptr;
    }
    
    /// @brief Check if a declaration has a value.
    bool hasValue(const ValueDeclAST* decl) const {
        return values.find(decl) != values.end();
    }
    
    // ─── Function Helpers ──────────────────────────────────────────────────
    
    /// @brief Store a function for a declaration.
    void storeFunction(const FuncDeclAST* decl, llvm::Function* func) {
        functions[decl] = func;
    }
    
    /// @brief Look up a function for a declaration.
    llvm::Function* lookupFunction(const FuncDeclAST* decl) const {
        auto it = functions.find(decl);
        return it != functions.end() ? it->second : nullptr;
    }
    
    // ─── Loop Helpers ──────────────────────────────────────────────────
    
    /// @brief Push a loop context.
    void pushLoop(llvm::BasicBlock* header, llvm::BasicBlock* exit,
                  llvm::BasicBlock* continueTarget = nullptr) {
        loops.push_back({header, exit, continueTarget});
    }
    
    /// @brief Pop the current loop context.
    void popLoop() {
        if (!loops.empty()) loops.pop_back();
    }
    
    /// @brief Get the current loop context (or nullptr if not in a loop).
    LoopInfo* currentLoop() {
        return loops.empty() ? nullptr : &loops.back();
    }
    
    /// @brief Check if we're inside a loop.
    bool insideLoop() const {
        return !loops.empty();
    }
    
    // ─── Type Helpers ──────────────────────────────────────────────────
    
    /// @brief Cache a type mapping.
    void cacheType(const TypeAST* lucidType, llvm::Type* llvmType) {
        typeCache[lucidType] = llvmType;
    }
    
    /// @brief Look up a cached type.
    llvm::Type* lookupType(const TypeAST* lucidType) const {
        auto it = typeCache.find(lucidType);
        return it != typeCache.end() ? it->second : nullptr;
    }
    
    /// @brief Cache a struct type.
    void cacheStruct(const StructDeclAST* decl, llvm::StructType* structType) {
        structCache[decl] = structType;
    }
    
    /// @brief Look up a cached struct type.
    llvm::StructType* lookupStruct(const StructDeclAST* decl) const {
        auto it = structCache.find(decl);
        return it != structCache.end() ? it->second : nullptr;
    }
};

} // namespace codegen