/// @file CodeGenContext.hpp
/// @brief Code generation context - LLVM state only.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
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

    // ─── TaggedSlot Helpers ──────────────────────────────────────────────

    /// @brief Cached TaggedSlot type.
    /// 
    /// TaggedSlot is used for two purposes:
    ///   1. **Nil/Err State** (T?, T!, T?!): sentinel = 0 (nil), 1 (valid), 2 (err)
    ///   2. **Type-Erased Generics** (@[erased]): All values are boxed as TaggedSlot* 
    ///      to allow uniform handling of different types.
    llvm::StructType* taggedSlotType_ = nullptr;

    /// @brief Box a value into a TaggedSlot for type-erased generic dispatch.
    /// 
    /// Creates a TaggedSlot struct with:
    ///   - sentinel: The state (0 = nil, 1 = valid, 2 = err) — DEFAULT IS 1 (valid)
    ///   - value: The opaque pointer to the actual value
    /// 
    /// ─── Usage ──────────────────────────────────────────────────────────────
    /// This is used only in the type-erased path (@[erased] generics):
    ///   - Function arguments: Boxing before calling an erased generic function
    ///   - Struct fields: Boxing before storing in an erased generic struct
    /// 
    /// ─── NOT Used for ──────────────────────────────────────────────────────
    ///   - Specialized path (default): All types are concrete, no boxing needed
    ///   - Non-generic code: No boxing needed
    /// @param value The value to box (will be bitcast to i8*).
    /// @param sentinel The state (0 = nil, 1 = valid, 2 = err). 
    ///        Default is 1 (valid).
    /// @param valueType The LLVM type of the value (for debugging).
    /// @return A pointer to the allocated TaggedSlot.
    llvm::Value* boxIntoTaggedSlot(
        llvm::Value* value,
        llvm::Value* sentinel,
        llvm::Type* valueType
    );

    /// @brief Box a value with a known sentinel (compile-time constant).
    /// 
    /// Convenience overload for when the sentinel is known at compile time.
    /// @param value The value to box.
    /// @param sentinel The state (0 = nil, 1 = valid, 2 = err).
    /// @param valueType The LLVM type of the value.
    /// @return A pointer to the allocated TaggedSlot.
    llvm::Value* boxIntoTaggedSlot(
        llvm::Value* value,
        uint32_t sentinel,
        llvm::Type* valueType
    );

    /// @brief Unbox a value from a TaggedSlot.
    /// 
    /// @param slotPtr Pointer to the TaggedSlot.
    /// @param targetType The expected LLVM type of the unboxed value.
    /// @return The unboxed value, or nullptr on error.
    llvm::Value* unboxFromTaggedSlot(
        llvm::Value* slotPtr,
        llvm::Type* targetType
    );

    /// @brief Get or create the TaggedSlot type.
    /// @return The TaggedSlot struct type.
    llvm::StructType* getTaggedSlotType();
    
    // ─── Current Function ───────────────────────────────────────────────
    llvm::Function* currentFunction = nullptr;

    // ─── Unified Exit Block (for ABI transforms like @[erased] boxing) ───
    // When returnBlock is non-null, lowerReturnStmt stores the return value
    // into returnValueAlloca (typed as returnValueType, NOT necessarily
    // func->getReturnType()) and branches to returnBlock instead of
    // emitting `ret` directly. This lets a single caller-installed exit
    // block do ABI-specific work (e.g. boxing into a TaggedSlot for
    // @[erased] functions) exactly once, regardless of how many return
    // sites exist in the body. See lowerErasedFunctionBody.
    llvm::BasicBlock* returnBlock = nullptr;
    llvm::Value* returnValueAlloca = nullptr;
    llvm::Type*  returnValueType   = nullptr;

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
    
    /// @brief Push a new live scope.
    void pushLiveScope(BlockStmtAST* block = nullptr) {
        liveTrackers.emplace_back();
        liveTrackers.back().block = block;
    }

    /// @brief Emit cleanup for exactly one tracker, in two ordered phases:
    ///   1. User #scope_exit callbacks (BEFORE implicit cleanup)
    ///   2. Implicit cleanup (closure releases, array frees, string frees)
    /// @note Read-only w.r.t. the tracker: does NOT mark anything consumed
    ///       and does NOT remove the tracker from ctx.liveTrackers. Callers
    ///       decide separately whether the tracker's scope is actually done
    ///       (popLiveScope) or whether this is just one of possibly several
    ///       divergent exit edges through it (emitUnwindTo) — see the note
    ///       on emitUnwindTo below for why that distinction matters.
    void emitCleanupForTracker(const LiveVariableTracker& tracker);

    /// @brief Pop the current live scope and emit cleanup.
    /// @note If the current block already ends in a terminator, this
    ///       scope's cleanup was already emitted by whatever produced that
    ///       terminator: return/break/continue all call emitUnwindTo
    ///       (non-destructively) through this depth before creating their
    ///       own terminator. Emitting again here would insert instructions
    ///       after a terminator (invalid IR) and double-release resources.
    ///       We still pop — this scope truly is done on this path — we
    ///       just skip the redundant emission.
    void popLiveScope() {
        if (!liveTrackers.empty()) {
            llvm::BasicBlock* block = builder.GetInsertBlock();
            if (!block || !block->getTerminator()) {
                emitCleanupForTracker(liveTrackers.back());
            }
            liveTrackers.pop_back();
        }
    }

    /// @brief Mark a variable as alive in the current scope.
    void markAlive(ValueDeclAST* decl) {
        if (!liveTrackers.empty()) liveTrackers.back().markAlive(decl);
    }

    /// @brief Mark a variable as consumed (handle transferred to runtime).
    /// @note This removes the variable from the alive list.
    void markConsumed(ValueDeclAST* decl) {
        if (!liveTrackers.empty()) liveTrackers.back().markConsumed(decl);
    }

    /// @brief Check if a variable is alive in any scope.
    bool isAlive(ValueDeclAST* decl) const {
        for (auto it = liveTrackers.rbegin(); it != liveTrackers.rend(); ++it) {
            if (it->isAlive(decl)) return true;
        }
        return false;
    }

    /// @brief Check if a variable is consumed in any scope.
    bool isConsumed(ValueDeclAST* decl) const {
        for (auto it = liveTrackers.rbegin(); it != liveTrackers.rend(); ++it) {
            if (it->isConsumed(decl)) return true;
        }
        return false;
    }

    // ─── Scope Unwind Helper ──────────────────────────────────────────────

    /// @brief Emit cleanup for scopes from current depth down to target depth.
    /// @param targetDepth The scope depth to unwind to (0 = function scope).
    /// 
    /// This is used when:
    ///   - `break` exits a loop (unwind to the loop's scope depth)
    ///   - `continue` jumps to next iteration (unwind to loop body's scope depth)
    ///   - `return` exits the function (unwind to scope 0)
    ///
    /// @note DELIBERATELY NON-DESTRUCTIVE. This does NOT pop from
    ///       liveTrackers and does NOT mutate the trackers it cleans up
    ///       (see emitCleanupForTracker). A return/break/continue is only
    ///       ONE of possibly several divergent exit edges out of the scopes
    ///       it's unwinding through — e.g. `if (cond) { return x; }` followed
    ///       by more code in the same enclosing block. That later code is
    ///       reached via a *different* basic block, but still needs those
    ///       same enclosing scopes' trackers intact: to keep registering
    ///       new declarations (markAlive) correctly, and so their OWN
    ///       eventual natural close (popLiveScope, reached only via that
    ///       other edge) still emits cleanup for whatever's alive on ITS
    ///       path. Popping or mutating a tracker here previously caused
    ///       cleanup to be silently dropped for sibling code (a leak), and
    ///       for `break`/`continue` (which unwind to a non-zero depth,
    ///       leaving the stack non-empty) could cause a LATER structurally
    ///       -paired popLiveScope() to pop the wrong (ancestor) tracker
    ///       instead — running that ancestor's cleanup early, inside a loop,
    ///       on the taken-break path, which can free a resource the
    ///       function is still using afterward (use-after-free). Only the
    ///       tracker's own structurally-paired popLiveScope() may ever
    ///       remove it from liveTrackers.
    void emitUnwindTo(size_t targetDepth);
    
    // ─── Resource Reassignment Helper ─────────────────────────────────────

    /// @brief Reassign a variable (clean up old resource, keep alive with new value).
    /// 
    /// ─── When to Use ──────────────────────────────────────────────────────────
    /// Call this in `lowerAssignExpr` BEFORE storing the new value:
    /// ```cpp
    /// llvm::Value* oldValue = loadOldValue(lhs);
    /// llvm::Value* newValue = lowerExpression(rhs);
    /// ctx.reassign(decl, oldValue, newValue);  // Clean up old resource
    /// ctx.builder.CreateStore(newValue, lhsPtr);
    /// ```
    /// 
    /// ─── What It Does ──────────────────────────────────────────────────────────
    /// 1. Checks if the variable is alive (owns a resource)
    /// 2. If alive, determines the resource type (closure/array/string)
    /// 3. Generates LLVM IR to release the old resource
    /// 4. Keeps the variable alive (unlike markConsumed)
    /// 
    /// ─── Why Not markConsumed? ─────────────────────────────────────────────────
    /// - `markConsumed` makes the variable DEAD (removes from alive list)
    /// - Reassignment keeps the variable ALIVE (just with a new value)
    /// - We need to clean up the old resource but keep the variable alive
    /// 
    /// ─── Resource Types Handled ─────────────────────────────────────────────────
    /// - Closures (FuncTypeAST with environment) → __lucid_release_env
    /// - Dynamic arrays ([*]T) → __lucid_free
    /// - Strings (string) → __lucid_free
    /// 
    /// ─── Error Cases ──────────────────────────────────────────────────────────
    /// - Future<T>: Cannot be reassigned while pending (linear type)
    /// - Thread<T>: Cannot be reassigned while running (linear type)
    void reassign(ValueDeclAST* decl, llvm::Value* oldValue, llvm::Value* newValue);
    
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