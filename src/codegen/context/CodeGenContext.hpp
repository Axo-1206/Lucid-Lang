/// @file CodeGenContext.hpp
/// @brief Code generation context - LLVM state only.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "../runtime/RuntimeFunctionRegistry.hpp"
#include "../generic/GenericRegistry.hpp"
#include "../support/LiveVariableTracker.hpp"
#include "../types/LLVMTypeHelpers.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Instructions.h>

#include <unordered_map>
#include <vector>
#include <string>

namespace codegen {

/// @brief Code generation context - LLVM state only.
struct CodeGenContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    DiagnosticEngine& diagnostics;
    llvm::LLVMContext& llvmCtx;

    // ─── Current Source File ───────────────────────────────────────────
    InternedString currentFile;
    
    // ─── LLVM Module and Builder ────────────────────────────────────────
    
    llvm::Module* module = nullptr;
    llvm::IRBuilder<> builder;
    
    // ─── Module Tracking ──────────────────────────────────────────────────
    
    /// @brief All modules being generated.
    std::vector<ModuleAST*> modules;
    
    /// @brief AST → LLVM module mapping.
    std::unordered_map<ModuleAST*, llvm::Module*> llvmModules;
    
    /// @brief Current module being generated.
    ModuleAST* currentModule = nullptr;
    
    // ─── Global Initialization ──────────────────────────────────────────
    
    /// @brief Information about a global variable that needs runtime initialization.
    struct GlobalInitInfo {
        VarDeclAST* decl;                    // The variable declaration
        ExprAST* init;                       // The initializer expression
        llvm::GlobalVariable* global;        // The LLVM global variable
        ModuleAST* module;                   // Which module this global belongs to
        int orderInModule;                   // Declaration order within module
    };
    
    /// @brief Pending globals that need runtime initialization.
    std::vector<GlobalInitInfo> pendingGlobals;
    
    // ─── Type Cache ─────────────────────────────────────────────────────
    
    std::unordered_map<TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<StructDeclAST*, llvm::StructType*> structCache;
    
    // ─── Current Environment Pointer (for closures) ────────────────────
    llvm::Value* currentEnvPtr = nullptr;
    
    // ─── Symbol Mapping: AST → LLVM Value ──────────────────────────────
    std::unordered_map<ValueDeclAST*, llvm::Value*> values;
    
    // ─── Function Mapping: AST → LLVM Function ─────────────────────────
    std::unordered_map<FuncDeclAST*, llvm::Function*> functions;

    // ─── Live Variable Tracking ──────────────────────────────────────────
    std::vector<LiveVariableTracker> liveTrackers;
    
    // ─── Runtime Function Mapping ──────────────────────────────────────
    std::unordered_map<std::string, llvm::Function*> runtimeFunctions;
    
    // ─── Generic Registry ──────────────────────────────────────────────
    GenericRegistry genericRegistry;
    
    // ─── Loop Info (for break/continue) ─────────────────────────────────
    struct LoopInfo {
        llvm::BasicBlock* header         = nullptr;
        llvm::BasicBlock* exit           = nullptr;
        llvm::BasicBlock* continueTarget = nullptr;
        size_t scopeDepth = 0;
    };
    std::vector<LoopInfo> loops;
    
    // ─── Current Function ───────────────────────────────────────────────
    llvm::Function* currentFunction = nullptr;
    llvm::BasicBlock* returnBlock = nullptr;

    // ─── Null Coalesce Context Stack ──────────────────────────────────
    struct NullCoalesceContext {
        llvm::BasicBlock* fallbackBlock = nullptr;
        bool isActive = false;
    };
    std::vector<NullCoalesceContext> nullCoalesceStack;

    // ─── Constructor ────────────────────────────────────────────────────
    
    CodeGenContext(StringPool& p, DiagnosticEngine& d, llvm::LLVMContext& ctx)
        : pool(p)
        , diagnostics(d)
        , llvmCtx(ctx)
        , builder(ctx) {}
    
    CodeGenContext(const CodeGenContext&) = delete;
    CodeGenContext& operator=(const CodeGenContext&) = delete;
    
    // ─── Module Helpers ──────────────────────────────────────────────────
    
    llvm::Module* getLLVMModule(ModuleAST* module) const {
        auto it = llvmModules.find(module);
        return it != llvmModules.end() ? it->second : nullptr;
    }
    
    // ─── Symbol Helpers ──────────────────────────────────────────────────
    
    void storeValue(ValueDeclAST* decl, llvm::Value* value) {
        values[decl] = value;
    }
    
    llvm::Value* lookupValue(ValueDeclAST* decl) const {
        auto it = values.find(decl);
        return it != values.end() ? it->second : nullptr;
    }
    
    bool hasValue(ValueDeclAST* decl) const {
        return values.find(decl) != values.end();
    }

    void storeFunction(FuncDeclAST* decl, llvm::Function* func) {
        functions[decl] = func;
    }
    
    llvm::Function* lookupFunction(FuncDeclAST* decl) const {
        auto it = functions.find(decl);
        return it != functions.end() ? it->second : nullptr;
    }
    
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
    
    llvm::Function* getOrCreateRuntimeFunction(
        const std::string& name,
        llvm::FunctionType* type
    );
    
    llvm::Function* getRuntimeFn(RuntimeFn fn);
    
    llvm::Function* getOrInsertFunction(
        const std::string& name,
        llvm::FunctionType* type
    );
    
    // ─── Loop Helpers ──────────────────────────────────────────────────
    
    void pushLoop(llvm::BasicBlock* header, llvm::BasicBlock* exit,
                  llvm::BasicBlock* continueTarget = nullptr) {
        loops.push_back({header, exit, continueTarget, liveTrackers.size()});
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

    // ─── Null Coalesce Helpers ──────────────────────────────────────────

    void pushNullCoalesce(llvm::BasicBlock* fallbackBlock) {
        nullCoalesceStack.push_back({fallbackBlock, true});
    }

    void popNullCoalesce() {
        if (!nullCoalesceStack.empty()) {
            nullCoalesceStack.pop_back();
        }
    }

    NullCoalesceContext* currentNullCoalesce() {
        return nullCoalesceStack.empty() ? nullptr : &nullCoalesceStack.back();
    }

    bool isInsideNullCoalesce() const {
        return !nullCoalesceStack.empty() && nullCoalesceStack.back().isActive;
    }

    llvm::BasicBlock* getNullCoalesceFallbackBlock() const {
        if (nullCoalesceStack.empty()) return nullptr;
        return nullCoalesceStack.back().fallbackBlock;
    }

    // ─── Live Variable Helpers ──────────────────────────────────────────
    
    void pushLiveScope(BlockStmtAST* block = nullptr) {
        liveTrackers.emplace_back();
        liveTrackers.back().block = block;
    }

    /// Emit cleanup for exactly one tracker, in two ordered phases:
    ///   1. User #scope_exit callbacks (BEFORE implicit cleanup)
    ///   2. Implicit cleanup (closure releases, array frees, string frees)
    void emitCleanupForTracker(LiveVariableTracker& tracker);

    void popLiveScope() {
        if (!liveTrackers.empty()) {
            emitCleanupForTracker(liveTrackers.back());
            liveTrackers.pop_back();
        }
    }

    void markAlive(ValueDeclAST* decl) {
        if (!liveTrackers.empty()) liveTrackers.back().markAlive(decl);
    }

    void markConsumed(ValueDeclAST* decl) {
        if (!liveTrackers.empty()) liveTrackers.back().markConsumed(decl);
    }

    bool isAlive(ValueDeclAST* decl) const {
        for (auto it = liveTrackers.rbegin(); it != liveTrackers.rend(); ++it) {
            if (it->isAlive(decl)) return true;
        }
        return false;
    }

    bool isConsumed(ValueDeclAST* decl) const {
        for (auto it = liveTrackers.rbegin(); it != liveTrackers.rend(); ++it) {
            if (it->isConsumed(decl)) return true;
        }
        return false;
    }
    
    // ─── Type Cache Helpers ──────────────────────────────────────────────
    
    void cacheType(TypeAST* lucidType, llvm::Type* llvmType) {
        typeCache[lucidType] = llvmType;
    }
    
    llvm::Type* lookupType(TypeAST* lucidType) const {
        auto it = typeCache.find(lucidType);
        return it != typeCache.end() ? it->second : nullptr;
    }
    
    void cacheStruct(StructDeclAST* decl, llvm::StructType* structType) {
        structCache[decl] = structType;
    }
    
    llvm::StructType* lookupStruct(StructDeclAST* decl) const {
        auto it = structCache.find(decl);
        return it != structCache.end() ? it->second : nullptr;
    }

    // ─── Fat Pointer Type Helpers ──────────────────────────────────────
    
    llvm::StructType* getSliceType() const {
        return codegen::getSliceType(module);
    }
    
    llvm::StructType* getClosureType() const {
        return codegen::getClosureType(module);
    }
    
    llvm::StructType* getStringType() const {
        return codegen::getStringType(module);
    }
    
    llvm::StructType* getArenaType() const {
        return codegen::getArenaType(module);
    }
    
    llvm::StructType* getArenaDescriptorType() const {
        return codegen::getArenaDescriptorType(module);
    }
    
    llvm::Value* createStringLiteral(const std::string& str);
    
    // ─── Intrinsic Helpers ─────────────────────────────────────────────
    
    llvm::Function* getLLVMIntrinsicDecl(
        llvm::Intrinsic::ID id,
        llvm::ArrayRef<llvm::Type*> argTypes
    );
    
    // ─── Pointee Type Helpers ──────────────────────────────────────────
    
    llvm::Type* getPointeeType(llvm::Value* ptr) const;
    llvm::Type* getPointeeType(llvm::Type* type) const;
};

} // namespace codegen