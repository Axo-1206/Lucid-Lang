/**
 * @file IRLowering.hpp
 * @brief Unified header for AST to LLVM IR lowering.
 * 
 * This file contains all declarations for the IR lowering module.
 * Implementation is split across multiple .cpp files:
 *   - IRLowering.cpp       : Main entry point + orchestration
 *   - IRLoweringDecl.cpp   : Declaration lowering
 *   - IRLoweringStmt.cpp   : Statement lowering
 *   - IRLoweringExpr.cpp   : Expression lowering
 *   - IRLoweringIntrinsic.cpp : Intrinsic lowering
 *   - IRLoweringBuilder.cpp   : Helper builders
 */

#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Intrinsics.h"

#include "../core/ast/BaseAST.hpp"
#include "../core/ast/DeclAST.hpp"
#include "../core/ast/ExprAST.hpp"
#include "../core/ast/StmtAST.hpp"
#include "../core/ast/TypeAST.hpp"
#include "../core/memory/InternedString.hpp"
#include "../core/memory/StringPool.hpp"
#include "TypeMapping.hpp"
#include "IntrinsicRegistry.hpp"

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Exception thrown when IR lowering fails.
 */
class IRLoweringError : public std::runtime_error {
public:
    enum class Kind {
        TypeConversionFailed,
        FunctionNotFound,
        VariableNotFound,
        UnsupportedNode,
        IntrinsicNotFound,
    };

    IRLoweringError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

// ─────────────────────────────────────────────────────────────────────────────
// IRLowering - Main Class
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Lowers Lucid AST to LLVM IR.
 * 
 * Walks the AST and emits LLVM IR instructions. Uses TypeMapping to convert
 * Lucid types to LLVM types.
 * 
 * Implementation is split across multiple .cpp files for maintainability.
 */
class IRLowering {
public:
    IRLowering(llvm::LLVMContext& context, TypeMapping& typeMapper, StringPool& stringPool);
    ~IRLowering() = default;

    /**
     * @brief Lower a ModuleAST to LLVM IR.
     */
    std::unique_ptr<llvm::Module> lower(ModuleAST* module, const std::string& moduleName);

    // ─── Accessors ──────────────────────────────────────────────────────────

    llvm::LLVMContext& getContext() { return m_context; }
    llvm::IRBuilder<>& getBuilder() { return m_builder; }
    TypeMapping& getTypeMapper() { return m_typeMapper; }
    StringPool& getStringPool() { return m_stringPool; }
    llvm::Module* getModule() { return m_module.get(); }

    std::string internedToString(InternedString name) const;

private:
    // ─── Friends ────────────────────────────────────────────────────────────
    // The implementation files need access to private members
    
    friend struct IRDeclLowering;
    friend struct IRStmtLowering;
    friend struct IRExprLowering;
    friend struct IRIntrinsicLowering;
    friend struct IRBuilderHelper;

    // ─── Scope Management ────────────────────────────────────────────────────

    struct Scope {
        std::unordered_map<std::string, llvm::AllocaInst*> locals;
        std::unordered_map<std::string, llvm::Value*> values;
    };

    void enterScope();
    void exitScope();
    Scope& currentScope();
    
    llvm::AllocaInst* allocateLocal(const std::string& name, llvm::Type* type);
    llvm::Value* lookupLocal(const std::string& name);
    void storeLocal(const std::string& name, llvm::Value* value);

    // ─── Function Context ───────────────────────────────────────────────────

    struct FunctionContext {
        llvm::Function* function = nullptr;
        llvm::BasicBlock* entryBlock = nullptr;
        FuncTypeAST* funcType = nullptr;
        std::unordered_map<std::string, llvm::Value*> parameters;
        bool hasReturn = false;
    };

    FunctionContext& currentFunction();

    // ─── Loop Context ──────────────────────────────────────────────────────

    struct LoopContext {
        llvm::BasicBlock* conditionBlock = nullptr;
        llvm::BasicBlock* bodyBlock = nullptr;
        llvm::BasicBlock* incrementBlock = nullptr;
        llvm::BasicBlock* exitBlock = nullptr;
    };

    void pushLoop(const LoopContext& ctx);
    void popLoop();
    bool hasLoop() const;
    const LoopContext& currentLoop() const;

    // ─── Type Helpers ────────────────────────────────────────────────────────

    llvm::Type* toLLVMType(TypeAST* type);
    llvm::FunctionType* toLLVMFunctionType(FuncTypeAST* funcType);
    llvm::Type* getPrimitiveType(PrimitiveKind kind);

    // ─── Runtime Functions ──────────────────────────────────────────────────

    void createPanicFunction(llvm::Module* module);
    void createCheckBoundsFunction(llvm::Module* module);

    // ─── Members ─────────────────────────────────────────────────────────────

    llvm::LLVMContext& m_context;
    TypeMapping& m_typeMapper;
    StringPool& m_stringPool;
    llvm::IRBuilder<> m_builder;
    std::unique_ptr<llvm::Module> m_module;

    std::vector<Scope> m_scopeStack;
    std::vector<FunctionContext> m_functionStack;
    std::vector<LoopContext> m_loopStack;

    llvm::Function* m_panicFunction = nullptr;
    llvm::Function* m_checkBoundsFunction = nullptr;

    std::string m_moduleName;
};

// ─────────────────────────────────────────────────────────────────────────────
// IRBuilderHelper - Common IR Construction Helpers
// ─────────────────────────────────────────────────────────────────────────────

struct IRBuilderHelper {
    // Allocation
    static llvm::AllocaInst* createAlloca(IRLowering& lowerer, llvm::Type* type, 
                                          const std::string& name = "");
    
    // Load/Store
    static llvm::LoadInst* createLoad(IRLowering& lowerer, llvm::Value* ptr, 
                                      const std::string& name = "");
    static llvm::StoreInst* createStore(IRLowering& lowerer, llvm::Value* value, 
                                        llvm::Value* ptr);
    
    // GetElementPointer
    static llvm::Value* createGEP(IRLowering& lowerer, llvm::Type* elemType,
                                  llvm::Value* ptr, llvm::Value* index,
                                  const std::string& name = "");
    static llvm::Value* createStructGEP(IRLowering& lowerer, 
                                        llvm::StructType* structType,
                                        llvm::Value* ptr, unsigned index,
                                        const std::string& name = "");
    
    // Pointer Conversion
    static llvm::Value* createPtrToInt(IRLowering& lowerer, llvm::Value* ptr,
                                       const std::string& name = "");
    static llvm::Value* createIntToPtr(IRLowering& lowerer, llvm::Value* intVal,
                                       llvm::Type* ptrType, const std::string& name = "");
    
    // Branching
    static void createConditionalBranch(IRLowering& lowerer, llvm::Value* cond,
                                        llvm::BasicBlock* trueBlock, 
                                        llvm::BasicBlock* falseBlock);
    
    // Block Management
    static llvm::BasicBlock* createBlock(IRLowering& lowerer, llvm::Function* fn,
                                         const std::string& name = "");
    static void setInsertPoint(IRLowering& lowerer, llvm::BasicBlock* block);
    
    // Comparisons
    static llvm::Value* createICmp(IRLowering& lowerer, llvm::CmpInst::Predicate pred,
                                   llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    static llvm::Value* createFCmp(IRLowering& lowerer, llvm::CmpInst::Predicate pred,
                                   llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    
    // Arithmetic
    static llvm::Value* createAdd(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createSub(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createMul(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createDiv(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createRem(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    
    // Bitwise
    static llvm::Value* createAnd(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createOr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                 const std::string& name = "");
    static llvm::Value* createXor(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createShl(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                  const std::string& name = "");
    static llvm::Value* createLShr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    static llvm::Value* createAShr(IRLowering& lowerer, llvm::Value* lhs, llvm::Value* rhs,
                                   const std::string& name = "");
    
    // Null Checks
    static llvm::Value* createIsNull(IRLowering& lowerer, llvm::Value* value,
                                     const std::string& name = "");
    static llvm::Value* createIsNotNull(IRLowering& lowerer, llvm::Value* value,
                                        const std::string& name = "");
    
    // Select
    static llvm::Value* createSelect(IRLowering& lowerer, llvm::Value* cond,
                                     llvm::Value* trueVal, llvm::Value* falseVal,
                                     const std::string& name = "");
    
    // Type Utilities
    static bool isIntegerType(llvm::Type* type);
    static bool isFloatingPointType(llvm::Type* type);
    static bool isPointerType(llvm::Type* type);
    static bool isStructType(llvm::Type* type);
    static bool isArrayType(llvm::Type* type);
    static bool isVoidType(llvm::Type* type);
    
    // Type Creation
    static llvm::IntegerType* getIntType(IRLowering& lowerer, unsigned bits);
    static llvm::PointerType* getPtrType(IRLowering& lowerer, llvm::Type* elemType = nullptr);
    
    // Constants
    static llvm::ConstantInt* getIntConstant(IRLowering& lowerer, uint64_t value, unsigned bits);
    static llvm::ConstantInt* getInt32Constant(IRLowering& lowerer, uint32_t value);
    static llvm::ConstantInt* getInt64Constant(IRLowering& lowerer, uint64_t value);
    static llvm::ConstantFP* getFloatConstant(IRLowering& lowerer, float value);
    static llvm::ConstantFP* getDoubleConstant(IRLowering& lowerer, double value);
    static llvm::Constant* getBoolConstant(IRLowering& lowerer, bool value);
};

// ─────────────────────────────────────────────────────────────────────────────
// IRDeclLowering - Declaration Lowering
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Lowers declarations to LLVM IR.
 * 
 * Implemented in IRLoweringDecl.cpp
 */
struct IRDeclLowering {
    static void lowerDecl(IRLowering& lowerer, DeclAST* decl);
    static void lowerFuncDecl(IRLowering& lowerer, FuncDeclAST* funcDecl);
    static void lowerForeignDecl(IRLowering& lowerer, FuncDeclAST* funcDecl);
    static void lowerVarDecl(IRLowering& lowerer, VarDeclAST* varDecl);
    static void lowerStructDecl(IRLowering& lowerer, StructDeclAST* structDecl);
    static void lowerEnumDecl(IRLowering& lowerer, EnumDeclAST* enumDecl);
};

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Statement Lowering
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Lowers statements to LLVM IR.
 * 
 * Implemented in IRLoweringStmt.cpp
 */
struct IRStmtLowering {
    static void lowerStmt(IRLowering& lowerer, StmtAST* stmt);
    static void lowerBlockStmt(IRLowering& lowerer, BlockStmtAST* block);
    static void lowerIfStmt(IRLowering& lowerer, IfStmtAST* ifStmt);
    static void lowerSwitchStmt(IRLowering& lowerer, SwitchStmtAST* switchStmt);
    static void lowerForStmt(IRLowering& lowerer, ForStmtAST* forStmt);
    static void lowerWhileStmt(IRLowering& lowerer, WhileStmtAST* whileStmt);
    static void lowerReturnStmt(IRLowering& lowerer, ReturnStmtAST* returnStmt);
    static void lowerExprStmt(IRLowering& lowerer, ExprStmtAST* exprStmt);
    static void lowerAssignStmt(IRLowering& lowerer, AssignExprAST* assign);
};

// ─────────────────────────────────────────────────────────────────────────────
// IRExprLowering - Expression Lowering
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Lowers expressions to LLVM IR.
 * 
 * Implemented in IRLoweringExpr.cpp
 */
struct IRExprLowering {
    static llvm::Value* lowerExpr(IRLowering& lowerer, ExprAST* expr);
    static llvm::Value* lowerLiteral(IRLowering& lowerer, LiteralExprAST* literal);
    static llvm::Value* lowerIdentifier(IRLowering& lowerer, IdentifierExprAST* identifier);
    static llvm::Value* lowerBinary(IRLowering& lowerer, BinaryExprAST* binary);
    static llvm::Value* lowerUnary(IRLowering& lowerer, UnaryExprAST* unary);
    static llvm::Value* lowerCall(IRLowering& lowerer, CallExprAST* call);
    static llvm::Value* lowerFieldAccess(IRLowering& lowerer, FieldAccessExprAST* field);
    static llvm::Value* lowerModuleAccess(IRLowering& lowerer, ModuleAccessExprAST* moduleAccess);
    static llvm::Value* lowerIndex(IRLowering& lowerer, IndexExprAST* index);
    static llvm::Value* lowerSlice(IRLowering& lowerer, SliceExprAST* slice);
    static llvm::Value* lowerNullCoalesce(IRLowering& lowerer, NullCoalesceExprAST* coalesce);
    static llvm::Value* lowerStructLiteral(IRLowering& lowerer, StructLiteralExprAST* structLit);
    static llvm::Value* lowerArrayLiteral(IRLowering& lowerer, ArrayLiteralExprAST* arrayLit);
    static llvm::Value* lowerPipeline(IRLowering& lowerer, PipelineExprAST* pipeline);
    static llvm::Value* lowerCompose(IRLowering& lowerer, ComposeExprAST* compose);
    static llvm::Value* lowerAnonFunc(IRLowering& lowerer, AnonFuncExprAST* anonFunc);
    static llvm::Value* lowerRange(IRLowering& lowerer, RangeExprAST* range);
};

// ─────────────────────────────────────────────────────────────────────────────
// IRIntrinsicLowering - Intrinsic Lowering
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Lowers intrinsics to LLVM IR.
 * 
 * Implemented in IRLoweringIntrinsic.cpp
 */
struct IRIntrinsicLowering {
    static llvm::Value* lowerIntrinsic(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerLLVMIntrinsic(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    
    // Compiler-handled intrinsics
    static llvm::Value* lowerIntrinsicSizeof(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicTypeof(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicTostr(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicAddrof(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicPtrOffset(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicPtrDiff(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicToRef(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
    static llvm::Value* lowerIntrinsicToPtr(IRLowering& lowerer, IntrinsicCallExprAST* intrinsic);
};