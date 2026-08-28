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
    
    // ─── LLVM Module and Builder ────────────────────────────────────────
    
    llvm::Module* module = nullptr;
    llvm::IRBuilder<> builder;
    
    // ─── Type Cache ─────────────────────────────────────────────────────
    
    std::unordered_map<TypeAST*, llvm::Type*> typeCache;
    std::unordered_map<StructDeclAST*, llvm::StructType*> structCache;
    
    // ─── Current Environment Pointer (for closures) ──────────────────────
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
    
    // ─── Function Helpers ──────────────────────────────────────────────────
    
    void storeFunction(FuncDeclAST* decl, llvm::Function* func) {
        functions[decl] = func;
    }
    
    llvm::Function* lookupFunction(FuncDeclAST* decl) const {
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
    
    /// @brief Get or create a runtime function in the module.
    /// @param name The function name.
    /// @param type The function type.
    /// @return The LLVM function.
    llvm::Function* getOrCreateRuntimeFunction(
        const std::string& name,
        llvm::FunctionType* type
    ) {
        llvm::Function* func = getRuntimeFunction(name);
        if (func) return func;

        func = llvm::Function::Create(
            type,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
        setRuntimeFunction(name, func);
        return func;
    }

    /// @brief Get or create a runtime function by its RuntimeFn entry.
    ///
    /// Preferred over getOrCreateRuntimeFunction(name, type) for anything
    /// with a RuntimeFunctionRegistry entry - the name is never spelled as
    /// a string at the call site, and the FunctionType comes from the
    /// registry's single definition instead of being rebuilt ad hoc at
    /// every call site. See runtime/RuntimeFunctionRegistry.hpp.
    /// @param fn The runtime function to get or declare.
    /// @return The LLVM function.
    llvm::Function* getRuntimeFn(RuntimeFn fn) {
        // 1. Look up the function info from the registry
        const RuntimeFunctionInfo& info = getRuntimeFunctionInfo(fn);
        std::string name(info.name);  // "__lucid_shutdown"
        
        // 2. Check if we already declared this function in the module
        llvm::Function* func = getRuntimeFunction(name);
        if (func) return func;
        
        // 3. Build the LLVM function type from the registry
        llvm::FunctionType* type = info.buildType(*this);
        
        // 4. Create an LLVM function declaration in the module
        func = llvm::Function::Create(
            type,
            llvm::Function::ExternalLinkage,  // External = defined elsewhere
            name,
            module
        );
        
        // 5. Cache it for future use
        setRuntimeFunction(name, func);
        return func;
    }
    
    /// @brief Get or insert a function in the module.
    /// @param name The function name.
    /// @param type The function type.
    /// @return The LLVM function.
    llvm::Function* getOrInsertFunction(
        const std::string& name,
        llvm::FunctionType* type
    ) {
        llvm::FunctionCallee callee = module->getOrInsertFunction(name, type);
        return llvm::dyn_cast<llvm::Function>(callee.getCallee());
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

    // ─── Live Variable Helpers ──────────────────────────────────────────
    
    void pushLiveScope() {
        liveTrackers.emplace_back();
    }

    void popLiveScope() {
        if (!liveTrackers.empty()) {
            emitScopeExitCleanup();
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

    /// @brief Release closure environments owned by the current live scope.
    ///
    /// This is called when a scope exits (block, function, loop body).
    /// It cleans up:
    ///   1. Closure environments (__lucid_release_env)
    ///   2. Heap-backed locals ([*]T, string)
    ///   3. Scope exit callbacks (#scope_exit)
    ///
    /// @note This function is called automatically by popLiveScope().
    void emitScopeExitCleanup() {
        if (liveTrackers.empty() || !getCurrentFunction()) return;

        LiveVariableTracker& tracker = liveTrackers.back();
        llvm::Function* releaseFn = getRuntimeFn(RuntimeFn::ReleaseEnv);
        llvm::Function* freeFn = getRuntimeFn(RuntimeFn::Free);
        std::vector<ValueDeclAST*> declarations = tracker.getAliveVariables();

        // ─── Helper: Load value if it's in an alloca ──────────────────────────
        auto loadValue = [&](llvm::Value* val) -> llvm::Value* {
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
                return builder.CreateLoad(alloca->getAllocatedType(), alloca, "cleanup_load");
            }
            return val;
        };

        for (ValueDeclAST* decl : declarations) {
            llvm::Value* binding = lookupValue(decl);
            if (!binding || !decl->type) continue;

            llvm::Value* value = loadValue(binding);

            // ─── 1. CLOSURE ENVIRONMENTS (FuncTypeAST) ──────────────────────────
            if (decl->type->isa<FuncTypeAST>()) {
                if (value->getType()->isStructTy() &&
                    value->getType()->getStructNumElements() == 2) {
                    llvm::Value* envPtr = builder.CreateExtractValue(value, 1, "closure_env");
                    llvm::Value* isNull = builder.CreateIsNull(envPtr, "env_is_null");

                    llvm::Function* func = getCurrentFunction();
                    llvm::BasicBlock* releaseBlock = llvm::BasicBlock::Create(
                        llvmCtx, "release_env", func);
                    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                        llvmCtx, "release_continue", func);

                    builder.CreateCondBr(isNull, continueBlock, releaseBlock);
                    builder.SetInsertPoint(releaseBlock);
                    builder.CreateCall(releaseFn, {envPtr});
                    builder.CreateBr(continueBlock);
                    builder.SetInsertPoint(continueBlock);

                    tracker.markConsumed(decl);
                }
                continue;
            }

            // ─── 2. DYNAMIC ARRAYS [*]T ──────────────────────────────────────────
            if (decl->type->isa<ArrayTypeAST>()) {
                ArrayTypeAST* arrayType = decl->type->as<ArrayTypeAST>();
                if (arrayType->isDynamic()) {
                    // Dynamic array value is { ptr, len, cap }
                    if (value->getType()->isStructTy() &&
                        value->getType()->getStructNumElements() == 3) {
                        llvm::Value* dataPtr = builder.CreateExtractValue(value, 0, "array_data");
                        llvm::Value* isNull = builder.CreateIsNull(dataPtr, "array_is_null");

                        llvm::Function* func = getCurrentFunction();
                        llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(
                            llvmCtx, "free_array", func);
                        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                            llvmCtx, "free_array_continue", func);

                        builder.CreateCondBr(isNull, continueBlock, freeBlock);
                        builder.SetInsertPoint(freeBlock);
                        builder.CreateCall(freeFn, {dataPtr});
                        builder.CreateBr(continueBlock);
                        builder.SetInsertPoint(continueBlock);

                        tracker.markConsumed(decl);
                    }
                }
                // Slices [_]T - do NOT free (borrowed view)
                // Fixed arrays [N]T - stack allocated, no free needed
                continue;
            }

            // ─── 3. STRINGS ──────────────────────────────────────────────────────
            if (decl->type->isa<PrimitiveTypeAST>()) {
                PrimitiveTypeAST* primType = decl->type->as<PrimitiveTypeAST>();
                if (primType->primitiveKind == PrimitiveKind::String) {
                    if (value->getType()->isStructTy() &&
                        value->getType()->getStructNumElements() == 3) {
                        llvm::Value* dataPtr = builder.CreateExtractValue(value, 0, "string_data");

                        // Check if this is a static string literal (global constant)
                        bool isStaticString = false;
                        if (llvm::Constant* constPtr = llvm::dyn_cast<llvm::Constant>(dataPtr)) {
                            if (llvm::isa<llvm::GlobalVariable>(constPtr)) {
                                isStaticString = true;
                            }
                        }

                        // Skip freeing static strings
                        if (isStaticString) {
                            tracker.markConsumed(decl);
                            continue;
                        }

                        llvm::Value* isNull = builder.CreateIsNull(dataPtr, "string_is_null");
                        llvm::Function* func = getCurrentFunction();
                        llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(
                            llvmCtx, "free_string", func);
                        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                            llvmCtx, "free_string_continue", func);

                        builder.CreateCondBr(isNull, continueBlock, freeBlock);
                        builder.SetInsertPoint(freeBlock);
                        builder.CreateCall(freeFn, {dataPtr});
                        builder.CreateBr(continueBlock);
                        builder.SetInsertPoint(continueBlock);

                        tracker.markConsumed(decl);
                    }
                }
                continue;
            }
        }
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

    // ─── Fat Pointer Type Helpers ────────────────────────────────────────

    /// @brief Get the canonical slice type (struct { ptr, len, cap }).
    llvm::StructType* getSliceType() const {
        llvm::StructType* type = llvm::StructType::getTypeByName(llvmCtx, "lucid.Slice");
        if (!type) {
            type = llvm::StructType::create(llvmCtx, "lucid.Slice");
            type->setBody({
                llvm::PointerType::get(llvmCtx, 0),
                llvm::Type::getInt64Ty(llvmCtx),
                llvm::Type::getInt64Ty(llvmCtx)
            });
        }
        return type;
    }

    /// @brief Get the canonical closure value type (struct { ptr, ptr }).
    llvm::StructType* getClosureType() const {
        llvm::StructType* type = llvm::StructType::getTypeByName(llvmCtx, "lucid.Closure");
        if (!type) {
            type = llvm::StructType::create(llvmCtx, "lucid.Closure");
            type->setBody({
                llvm::PointerType::get(llvmCtx, 0),
                llvm::PointerType::get(llvmCtx, 0)
            });
        }
        return type;
    }

    /// @brief Get the canonical string type (struct { ptr, len, cap }).
    llvm::StructType* getStringType() const {
        llvm::StructType* type = llvm::StructType::getTypeByName(llvmCtx, "lucid.String");
        if (!type) {
            type = llvm::StructType::create(llvmCtx, "lucid.String");
            type->setBody({
                llvm::PointerType::get(llvmCtx, 0),
                llvm::Type::getInt64Ty(llvmCtx),
                llvm::Type::getInt64Ty(llvmCtx)
            });
        }
        return type;
    }

    /// @brief Create a string literal as an LLVM value.
    /// @param str The string content.
    /// @return An LLVM value representing the string literal.
    llvm::Value* createStringLiteral(const std::string& str) {
        llvm::Constant* strConst = llvm::ConstantDataArray::getString(llvmCtx, str);
        llvm::GlobalVariable* global = new llvm::GlobalVariable(
            *module,
            strConst->getType(),
            true,
            llvm::GlobalValue::PrivateLinkage,
            strConst
        );

        llvm::Type* strType = getStringType();
        llvm::Type* i64 = llvm::Type::getInt64Ty(llvmCtx);
        llvm::Type* i8Ptr = llvm::PointerType::get(llvmCtx, 0);

        llvm::Value* ptr = builder.CreateBitCast(global, i8Ptr);
        llvm::Value* len = llvm::ConstantInt::get(i64, str.length());

        llvm::Value* result = llvm::UndefValue::get(strType);
        result = builder.CreateInsertValue(result, ptr, 0);
        result = builder.CreateInsertValue(result, len, 1);
        result = builder.CreateInsertValue(result, len, 2);
        return result;
    }
    
    // ─── Intrinsic Helpers ─────────────────────────────────────────────
    
    /// @brief Get an LLVM intrinsic function declaration.
    /// @param id The LLVM intrinsic ID.
    /// @param argTypes The argument types.
    /// @return The LLVM function.
    llvm::Function* getLLVMIntrinsicDecl(
        llvm::Intrinsic::ID id,
        llvm::ArrayRef<llvm::Type*> argTypes
    ) {
        return llvm::Intrinsic::getDeclaration(module, id, argTypes);
    }
    
    /// @brief Parse a memory ordering string to LLVM AtomicOrdering.
    /// @param order The ordering string ("relaxed", "acquire", etc.).
    /// @return The corresponding LLVM AtomicOrdering.
    static llvm::AtomicOrdering parseOrdering(const std::string& order) {
        if (order == "relaxed") return llvm::AtomicOrdering::Monotonic;
        if (order == "acquire") return llvm::AtomicOrdering::Acquire;
        if (order == "release") return llvm::AtomicOrdering::Release;
        if (order == "acq_rel") return llvm::AtomicOrdering::AcquireRelease;
        if (order == "seq_cst") return llvm::AtomicOrdering::SequentiallyConsistent;
        return llvm::AtomicOrdering::SequentiallyConsistent;
    }
    
    // ─── DataLayout Helpers ─────────────────────────────────────────────
    
    /// @brief Get the size of a type in bytes.
    /// @param type The LLVM type.
    /// @return The size in bytes.
    uint64_t getTypeSize(llvm::Type* type) const {
        return module->getDataLayout().getTypeAllocSize(type);
    }
    
    /// @brief Get the alignment of a type in bytes.
    /// @param type The LLVM type.
    /// @return The alignment in bytes.
    uint64_t getTypeAlign(llvm::Type* type) const {
        return module->getDataLayout().getABITypeAlign(type).value();
    }
    
    // ─── Pointee Type Helpers (opaque pointer safe) ─────────────────────
    
    /// @brief Get the pointee type for a pointer value.
    /// @param ptr The pointer value.
    /// @return The LLVM type of the pointee, or i8* if unknown.
    llvm::Type* getPointeeType(llvm::Value* ptr) const {
        // With opaque pointers (LLVM 17+), we can't get the element type from the pointer.
        // Default to i8
        (void)ptr;
        return llvm::Type::getInt8Ty(llvmCtx);
    }
    
    /// @brief Get the pointee type from a pointer type.
    /// @param type The pointer type.
    /// @return The LLVM type of the pointee, or i8* if unknown.
    llvm::Type* getPointeeType(llvm::Type* type) const {
        // With opaque pointers (LLVM 17+), we can't get the element type from the pointer.
        // Default to i8
        (void)type;
        return llvm::Type::getInt8Ty(llvmCtx);
    }
};

} // namespace codegen