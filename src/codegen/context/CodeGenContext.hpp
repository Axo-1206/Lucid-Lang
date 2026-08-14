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
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <unordered_map>
#include <vector>
#include <string>

namespace codegen {

// ─── Forward declarations ──────────────────────────────────────────────────

/// @brief A key for identifying a generic instantiation.
struct GenericInstantiationKey {
    DeclAST* decl;                    // The generic declaration
    std::vector<TypeAST*> typeArgs;   // Concrete type arguments
    
    bool operator==(const GenericInstantiationKey& other) const;
};

/// @brief Hash for GenericInstantiationKey.
struct GenericInstantiationKeyHash {
    size_t operator()(const GenericInstantiationKey& key) const;
};

/// @brief Registry of all generic instantiations in a module.
/// 
/// This is a CACHE, not a global registry. It tracks which specialized
/// versions we've already generated so we don't generate them twice.
struct GenericRegistry {
    // ─── Function Instantiations ──────────────────────────────────────────
    // Generic function → (type args → specialized function)
    std::unordered_map<
        FuncDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Function*, GenericInstantiationKeyHash>
    > functionInstantiations;
    
    // ─── Struct Instantiations ─────────────────────────────────────────────
    // Generic struct → (type args → specialized struct type)
    std::unordered_map<
        StructDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Type*, GenericInstantiationKeyHash>
    > structInstantiations;
};

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

    // ─── String Type Helpers ──────────────────────────────────────────────
    
    /// @brief Get the string type (struct { ptr, len, cap }).
    /// @return The string struct type.
    llvm::StructType* getStringType() const {
        llvm::Type* i8Ptr = llvm::PointerType::get(llvmCtx, 0);
        llvm::Type* i64 = llvm::Type::getInt64Ty(llvmCtx);
        return llvm::StructType::get(llvmCtx, {i8Ptr, i64, i64});
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