/**
 * @file IRLowering.hpp
 * @brief AST to LLVM IR lowering for the Lucid compiler.
 * 
 * @responsibility Walks the validated AST and emits LLVM IR instructions.
 *                 This is the shared backend for both interpreter and AOT.
 * 
 * @related_files
 *   - src/interpreter/Interpreter.hpp - uses IRLowering for JIT execution
 *   - src/compiler/aot/AOT.hpp - uses IRLowering for AOT compilation
 *   - src/compiler/TypeMapping.hpp - Lucid → LLVM type conversion
 *   - src/ast/BaseAST.hpp - AST root node
 *   - src/core/memory/StringPool.hpp - String interning
 */

#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"

#include "../core/ast/BaseAST.hpp"
#include "../core/ast/DeclAST.hpp"
#include "../core/ast/ExprAST.hpp"
#include "../core/ast/StmtAST.hpp"
#include "../core/ast/TypeAST.hpp"
#include "../core/memory/InternedString.hpp"
#include "../core/memory/StringPool.hpp"
#include "TypeMapping.hpp"

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>

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

/**
 * @brief Lowers Lucid AST to LLVM IR.
 * 
 * Walks the AST and emits LLVM IR instructions. Uses TypeMapping to convert
 * Lucid types to LLVM types.
 * 
 * @par Usage Example
 * @code
 *   llvm::LLVMContext context;
 *   StringPool stringPool;
 *   TypeMapping typeMapper(context, stringPool);
 *   IRLowering lowerer(context, typeMapper, stringPool);
 *   
 *   auto module = lowerer.lower(moduleAST, "main_module");
 *   if (module) {
 *       // Use module with JIT or AOT backend
 *   }
 * @endcode
 * 
 * @par Scope Management
 * The lowerer maintains its own scope stack for LLVM-level variables:
 * - `enterScope()` - Called when entering a block
 * - `exitScope()` - Called when exiting a block
 * - `lookupLocal()` - Finds a variable in the current scope chain
 * - `allocateLocal()` - Creates an alloca for a local variable
 */
class IRLowering {
public:
    /**
     * @brief Construct an IRLowering instance.
     * 
     * @param context The LLVM context to use
     * @param typeMapper The type mapper for type conversion
     * @param stringPool The string pool for name resolution
     */
    IRLowering(llvm::LLVMContext& context, TypeMapping& typeMapper, StringPool& stringPool);
    ~IRLowering() = default;

    /**
     * @brief Lower a ModuleAST to LLVM IR.
     * 
     * @param module The ModuleAST to lower
     * @param moduleName The name for the LLVM module
     * @return std::unique_ptr<llvm::Module> or nullptr on failure
     * @throws IRLoweringError if lowering fails
     */
    std::unique_ptr<llvm::Module> lower(ModuleAST* module,
                                        const std::string& moduleName);

    /**
     * @brief Get the LLVM context.
     * 
     * @return Reference to the LLVM context
     */
    llvm::LLVMContext& getContext() { return m_context; }

    /**
     * @brief Get the IR builder.
     * 
     * @return Reference to the IR builder
     */
    llvm::IRBuilder<>& getBuilder() { return m_builder; }

    /**
     * @brief Convert an InternedString to a std::string for lookup.
     * 
     * @param name The interned string
     * @return std::string The string representation
     */
    std::string internedToString(InternedString name) const;

private:
    // ─── Scope Management ────────────────────────────────────────────────────

    /**
     * @brief Represents a single scope in the LLVM lowering.
     */
    struct Scope {
        std::unordered_map<std::string, llvm::AllocaInst*> locals;  // Variable allocations
        std::unordered_map<std::string, llvm::Value*> values;       // Temporary values
    };

    /**
     * @brief Enter a new scope.
     */
    void enterScope();

    /**
     * @brief Exit the current scope.
     */
    void exitScope();

    /**
     * @brief Get the current scope.
     */
    Scope& currentScope();

    /**
     * @brief Allocate a local variable in the current scope.
     * 
     * @param name The variable name
     * @param type The LLVM type
     * @return llvm::AllocaInst* The allocation instruction
     */
    llvm::AllocaInst* allocateLocal(const std::string& name, llvm::Type* type);

    /**
     * @brief Look up a local variable in the scope chain.
     * 
     * @param name The variable name
     * @return llvm::Value* The variable value, or nullptr if not found
     */
    llvm::Value* lookupLocal(const std::string& name);

    /**
     * @brief Store a value in a local variable.
     * 
     * @param name The variable name
     * @param value The value to store
     */
    void storeLocal(const std::string& name, llvm::Value* value);

    // ─── Lowering Methods ────────────────────────────────────────────────────

    /**
     * @brief Lower a declaration node.
     * 
     * @param decl The declaration to lower
     */
    void lowerDecl(DeclAST* decl);

    /**
     * @brief Lower a function declaration.
     * 
     * @param funcDecl The function declaration to lower
     */
    void lowerFuncDecl(FuncDeclAST* funcDecl);

    /**
     * @brief Lower a variable declaration.
     * 
     * @param varDecl The variable declaration to lower
     */
    void lowerVarDecl(VarDeclAST* varDecl);

    /**
     * @brief Lower a struct declaration.
     * 
     * @param structDecl The struct declaration to lower
     */
    void lowerStructDecl(StructDeclAST* structDecl);

    /**
     * @brief Lower an enum declaration.
     * 
     * @param enumDecl The enum declaration to lower
     */
    void lowerEnumDecl(EnumDeclAST* enumDecl);

    /**
     * @brief Lower a foreign declaration.
     * 
     * @param funcDecl The foreign function declaration
     */
    void lowerForeignDecl(FuncDeclAST* funcDecl);

    /**
     * @brief Lower a statement node.
     * 
     * @param stmt The statement to lower
     */
    void lowerStmt(StmtAST* stmt);

    /**
     * @brief Lower a block statement.
     * 
     * @param block The block statement to lower
     */
    void lowerBlockStmt(BlockStmtAST* block);

    /**
     * @brief Lower an if statement.
     * 
     * @param ifStmt The if statement to lower
     */
    void lowerIfStmt(IfStmtAST* ifStmt);

    /**
     * @brief Lower a switch statement.
     * 
     * @param switchStmt The switch statement to lower
     */
    void lowerSwitchStmt(SwitchStmtAST* switchStmt);

    /**
     * @brief Lower a for statement.
     * 
     * @param forStmt The for statement to lower
     */
    void lowerForStmt(ForStmtAST* forStmt);

    /**
     * @brief Lower a while statement.
     * 
     * @param whileStmt The while statement to lower
     */
    void lowerWhileStmt(WhileStmtAST* whileStmt);

    /**
     * @brief Lower a return statement.
     * 
     * @param returnStmt The return statement to lower
     */
    void lowerReturnStmt(ReturnStmtAST* returnStmt);

    /**
     * @brief Lower an expression statement.
     * 
     * @param exprStmt The expression statement to lower
     */
    void lowerExprStmt(ExprStmtAST* exprStmt);

    /**
     * @brief Lower an assignment statement.
     * 
     * @param assign The assignment expression to lower
     */
    void lowerAssignStmt(AssignExprAST* assign);

    /**
     * @brief Lower an expression node.
     * 
     * @param expr The expression to lower
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerExpr(ExprAST* expr);

    /**
     * @brief Lower a literal expression.
     * 
     * @param literal The literal expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerLiteral(LiteralExprAST* literal);

    /**
     * @brief Lower an identifier expression.
     * 
     * @param identifier The identifier expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerIdentifier(IdentifierExprAST* identifier);

    /**
     * @brief Lower a binary expression.
     * 
     * @param binary The binary expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerBinary(BinaryExprAST* binary);

    /**
     * @brief Lower a unary expression.
     * 
     * @param unary The unary expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerUnary(UnaryExprAST* unary);

    /**
     * @brief Lower a call expression.
     * 
     * @param call The call expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerCall(CallExprAST* call);

    /**
     * @brief Lower a field access expression.
     * 
     * @param field The field access expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerFieldAccess(FieldAccessExprAST* field);

    /**
     * @brief Lower a module access expression.
     * 
     * @param moduleAccess The module access expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerModuleAccess(ModuleAccessExprAST* moduleAccess);

    /**
     * @brief Lower an index expression.
     * 
     * @param index The index expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerIndex(IndexExprAST* index);

    /**
     * @brief Lower a slice expression.
     * 
     * @param slice The slice expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerSlice(SliceExprAST* slice);

    /**
     * @brief Lower an intrinsic call expression.
     * 
     * @param intrinsic The intrinsic call expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerIntrinsic(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower a null coalesce expression.
     * 
     * @param coalesce The null coalesce expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerNullCoalesce(NullCoalesceExprAST* coalesce);

    /**
     * @brief Lower a struct literal expression.
     * 
     * @param structLit The struct literal expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerStructLiteral(StructLiteralExprAST* structLit);

    /**
     * @brief Lower an array literal expression.
     * 
     * @param arrayLit The array literal expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerArrayLiteral(ArrayLiteralExprAST* arrayLit);

    /**
     * @brief Lower a pipeline expression.
     * 
     * @param pipeline The pipeline expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerPipeline(PipelineExprAST* pipeline);

    /**
     * @brief Lower a compose expression.
     * 
     * @param compose The compose expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerCompose(ComposeExprAST* compose);

    /**
     * @brief Lower an anonymous function expression.
     * 
     * @param anonFunc The anonymous function expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerAnonFunc(AnonFuncExprAST* anonFunc);

    /**
     * @brief Lower a range expression.
     * 
     * @param range The range expression
     * @return llvm::Value* The lowered value
     */
    llvm::Value* lowerRange(RangeExprAST* range);

    // ─── Intrinsic Helpers ───────────────────────────────────────────────────

    /**
     * @brief Lower the #sizeof intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The size as a constant
     */
    llvm::Value* lowerIntrinsicSizeof(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #typeof intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The type name as a string
     */
    llvm::Value* lowerIntrinsicTypeof(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #tostr intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The string representation
     */
    llvm::Value* lowerIntrinsicTostr(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #sqrt intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The sqrt result
     */
    llvm::Value* lowerIntrinsicSqrt(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #memcpy intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The memcpy result (void)
     */
    llvm::Value* lowerIntrinsicMemcpy(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #addrof intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The address
     */
    llvm::Value* lowerIntrinsicAddrof(IntrinsicCallExprAST* intrinsic);

    /**
     * @brief Lower the #ptrOffset intrinsic.
     * 
     * @param intrinsic The intrinsic call
     * @return llvm::Value* The offset pointer
     */
    llvm::Value* lowerIntrinsicPtrOffset(IntrinsicCallExprAST* intrinsic);

    // ─── Type Helpers ────────────────────────────────────────────────────────

    /**
     * @brief Convert a Lucid type to an LLVM type.
     * 
     * @param type The Lucid type
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* toLLVMType(TypeAST* type);

    /**
     * @brief Convert a Lucid function type to an LLVM function type.
     * 
     * @param funcType The Lucid function type
     * @return llvm::FunctionType* The LLVM function type
     */
    llvm::FunctionType* toLLVMFunctionType(FuncTypeAST* funcType);

    /**
     * @brief Get the LLVM type for a primitive kind.
     * 
     * @param kind The primitive kind
     * @return llvm::Type* The LLVM type
     */
    llvm::Type* getPrimitiveType(PrimitiveKind kind);

    // ─── Function Helpers ────────────────────────────────────────────────────

    /**
     * @brief Get or create a function declaration.
     * 
     * @param name The function name
     * @param funcType The function type
     * @param module The LLVM module
     * @return llvm::Function* The function
     */
    llvm::Function* getOrCreateFunction(const std::string& name,
                                        FuncTypeAST* funcType,
                                        llvm::Module* module);

    /**
     * @brief Get or create a foreign function declaration.
     * 
     * @param name The function name
     * @param funcType The function type
     * @param module The LLVM module
     * @return llvm::Function* The function
     */
    llvm::Function* getOrCreateForeignFunction(const std::string& name,
                                               FuncTypeAST* funcType,
                                               llvm::Module* module);

    /**
     * @brief Create an intrinsic function call.
     * 
     * @param intrinsicName The intrinsic name
     * @param args The arguments
     * @param returnType The return type
     * @return llvm::CallInst* The call instruction
     */
    llvm::CallInst* createIntrinsicCall(const std::string& intrinsicName,
                                        const std::vector<llvm::Value*>& args,
                                        llvm::Type* returnType);

    // ─── Members ─────────────────────────────────────────────────────────────

    llvm::LLVMContext& m_context;
    TypeMapping& m_typeMapper;
    StringPool& m_stringPool;
    llvm::IRBuilder<> m_builder;
    std::unique_ptr<llvm::Module> m_module;

    // Scope stack
    std::vector<Scope> m_scopeStack;

    // Current function context
    struct FunctionContext {
        llvm::Function* function = nullptr;
        llvm::BasicBlock* entryBlock = nullptr;
        FuncTypeAST* funcType = nullptr;
        std::unordered_map<std::string, llvm::Value*> parameters;
        bool hasReturn = false;
    };
    std::vector<FunctionContext> m_functionStack;

    FunctionContext& currentFunction() { return m_functionStack.back(); }

    // Loop context for break/continue
    struct LoopContext {
        llvm::BasicBlock* conditionBlock = nullptr;
        llvm::BasicBlock* bodyBlock = nullptr;
        llvm::BasicBlock* incrementBlock = nullptr;
        llvm::BasicBlock* exitBlock = nullptr;
    };
    std::vector<LoopContext> m_loopStack;

    // Built-in runtime function declarations
    llvm::Function* m_panicFunction = nullptr;
    llvm::Function* m_checkBoundsFunction = nullptr;

    // Module name
    std::string m_moduleName;

    // ─── Runtime Function Creation ──────────────────────────────────────────

    /**
     * @brief Create the panic runtime function declaration.
     * 
     * @param module The LLVM module
     */
    void createPanicFunction(llvm::Module* module);

    /**
     * @brief Create the bounds check runtime function declaration.
     * 
     * @param module The LLVM module
     */
    void createCheckBoundsFunction(llvm::Module* module);
};
