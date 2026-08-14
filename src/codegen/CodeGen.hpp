/// @file CodeGen.hpp
/// @brief Lucid code generator – lowers validated AST to LLVM IR.
///
/// The CodeGen pass is the final stage of the frontend. It walks the
/// validated AST and emits LLVM IR for each module.
///
/// ─── Architecture ──────────────────────────────────────────────────────────
/// The CodeGen is split into specialized files:
///   - CodeGen.cpp       : Orchestrator, module-level emission
///   - CodeGenDecl.cpp   : Function, variable, struct, enum declarations
///   - CodeGenStmt.cpp   : Statements (if, for, while, return, block)
///   - CodeGenExpr.cpp   : Expressions (literals, binary, calls, intrinsics)
///   - CodeGenGeneric.cpp: Generic instantiation and type-erased generation
///   - CodeGenType.cpp   : Lucid → LLVM type mapping
///   - closure/          : Closure environment and capture handling
///
/// ─── Two-Phase Function Lowering ──────────────────────────────────────────
/// Functions are lowered in two passes:
///   1. Lower declarations: Create llvm::Function prototypes for all functions
///   2. Lower bodies: Generate IR for each function body
///
/// This allows forward references (calls to functions defined later).
///
/// ─── CodeGenContext ──────────────────────────────────────────────────────
/// The context carries all state needed during lowering:
///   - LLVM context, module, builder
///   - Current function and insertion point
///   - Symbol table (AST node → LLVM value)
///   - Type cache
///   - Break/continue target stack
///   - Closure environment tracking

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "context/CodeGenContext.hpp"
#include "CodeGenType.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Verifier.h>

#include <memory>
#include <vector>
#include <unordered_map>

namespace codegen {

// =============================================================================
// Main Code Generation Entry Point
// =============================================================================

/// @brief Generate LLVM IR for a set of modules.
///
/// This is the main entry point for code generation. It lowers each module
/// to an LLVM Module and returns a vector of unique pointers.
///
/// @param modules The modules to generate IR for.
/// @param context The LLVM context to use.
/// @return A vector of LLVM modules, one per input module.
///
/// @example
///   llvm::LLVMContext llvmCtx;
///   auto modules = CodeGen::generate(astModules, llvmCtx);
std::vector<std::unique_ptr<llvm::Module>> generate(
    const std::vector<ModuleAST*>& modules,
    StringPool& p, DiagnosticEngine& d,
    llvm::LLVMContext& context
);

// =============================================================================
// Module-Level Emission
// =============================================================================

/// @brief Generate IR for a single module.
///
/// @param module The module AST to generate IR for.
/// @param ctx The code generation context.
/// @return The LLVM module, or nullptr on error.
std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx);

/// @brief Lower all top-level declarations in a module.
///
/// This creates LLVM types and function declarations but does NOT
/// lower function bodies. Bodies are lowered in a second pass.
///
/// @param module The module AST.
/// @param ctx The code generation context.
void lowerModuleDeclarations(ModuleAST* module, CodeGenContext& ctx);

/// @brief Lower all function bodies in a module.
///
/// This is the second pass of function lowering. It visits each function
/// declaration and generates IR for its body.
///
/// @param module The module AST.
/// @param ctx The code generation context.
void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx);

// =============================================================================
// Declaration Lowering
// =============================================================================

/// @brief Lower a declaration to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// declaration kind.
///
/// @param decl The declaration to lower.
/// @param ctx The code generation context.
void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a function declaration (prototype only).
///
/// Creates the llvm::Function for the declaration. Does NOT lower the body.
///
/// @param decl The function declaration.
/// @param ctx The code generation context.
void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Internal function to lower a function body.
///
/// This is called for non-generic functions and for type-erased generic
/// functions. It creates the entry block, lowers parameters, and generates
/// the function body.
///
/// @param decl The function declaration.
/// @param func The LLVM function to generate the body for.
/// @param ctx The code generation context.
void lowerFunctionBodyInternal(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx);

/// @brief Lower a function body (second pass).
///
/// Generates IR for the function's body and verifies it.
///
/// @param decl The function declaration.
/// @param ctx The code generation context.
void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a variable declaration.
///
/// @param decl The variable declaration.
/// @param ctx The code generation context.
void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a struct declaration.
///
/// Creates the LLVM struct type for the struct.
///
/// @param decl The struct declaration.
/// @param ctx The code generation context.
void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower an enum declaration.
///
/// Enums are lowered to integer constants.
///
/// @param decl The enum declaration.
/// @param ctx The code generation context.
void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a parameter.
///
/// Registers the parameter in the symbol table and sets up its alloca.
///
/// @param param The parameter.
/// @param ctx The code generation context.
void lowerParam(ParamAST* param, CodeGenContext& ctx);

// =============================================================================
// Statement Lowering
// =============================================================================

/// @brief Lower a statement to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// statement kind.
///
/// @param stmt The statement to lower.
/// @param ctx The code generation context.
void lowerStatement(StmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a block statement.
///
/// @param block The block statement.
/// @param ctx The code generation context.
void lowerBlockStmt(BlockStmtAST* block, CodeGenContext& ctx);

/// @brief Lower an if statement.
///
/// @param stmt The if statement.
/// @param ctx The code generation context.
void lowerIfStmt(IfStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a switch statement.
///
/// @param stmt The switch statement.
/// @param ctx The code generation context.
void lowerSwitchStmt(SwitchStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a for loop.
///
/// @param stmt The for statement.
/// @param ctx The code generation context.
void lowerForStmt(ForStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a while loop.
///
/// @param stmt The while statement.
/// @param ctx The code generation context.
void lowerWhileStmt(WhileStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a do-while loop.
///
/// @param stmt The do-while statement.
/// @param ctx The code generation context.
void lowerDoWhileStmt(DoWhileStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a return statement.
///
/// @param stmt The return statement.
/// @param ctx The code generation context.
void lowerReturnStmt(ReturnStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a break statement.
///
/// @param stmt The break statement.
/// @param ctx The code generation context.
void lowerBreakStmt(BreakStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a continue statement.
///
/// @param stmt The continue statement.
/// @param ctx The code generation context.
void lowerContinueStmt(ContinueStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower an expression statement.
///
/// @param stmt The expression statement.
/// @param ctx The code generation context.
void lowerExprStmt(ExprStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a declaration statement.
///
/// @param stmt The declaration statement.
/// @param ctx The code generation context.
void lowerDeclStmt(DeclStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a function reference statement.
///
/// @param stmt The function reference statement.
/// @param ctx The code generation context.
void lowerFuncRefStmt(FuncRefStmtAST* stmt, CodeGenContext& ctx);

// ─── Concurrency Statements ─────────────────────────────────────────────

/// @brief Lower an async statement.
///
/// @param stmt The async statement.
/// @param ctx The code generation context.
void lowerAsyncStmt(AsyncStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower an await statement.
///
/// @param stmt The await statement.
/// @param ctx The code generation context.
void lowerAwaitStmt(AwaitStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a spawn statement.
///
/// @param stmt The spawn statement.
/// @param ctx The code generation context.
void lowerSpawnStmt(SpawnStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a join statement.
///
/// @param stmt The join statement.
/// @param ctx The code generation context.
void lowerJoinStmt(JoinStmtAST* stmt, CodeGenContext& ctx);

// =============================================================================
// Expression Lowering
// =============================================================================

/// @brief Lower an expression to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// expression kind. Stores the result in expr->llvmValue.
///
/// @param expr The expression to lower.
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr on error.
llvm::Value* lowerExpression(ExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a literal expression.
///
/// @param expr The literal expression.
/// @param ctx The code generation context.
/// @return The LLVM constant value.
llvm::Value* lowerLiteralExpr(LiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an identifier expression.
///
/// @param expr The identifier expression.
/// @param ctx The code generation context.
/// @return The LLVM value (loaded if r-value, pointer if l-value).
llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an array literal expression.
///
/// @param expr The array literal expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a struct literal expression.
///
/// @param expr The struct literal expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerStructLiteralExpr(StructLiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a binary expression.
///
/// @param expr The binary expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a unary expression.
///
/// @param expr The unary expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a call expression.
///
/// @param expr The call expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an intrinsic call expression.
///
/// @param expr The intrinsic call expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an index expression.
///
/// @param expr The index expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a slice expression.
///
/// @param expr The slice expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a field access expression.
///
/// @param expr The field access expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a module access expression.
///
/// @param expr The module access expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a null coalesce expression.
///
/// @param expr The null coalesce expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an assignment expression.
///
/// @param expr The assignment expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a pipeline expression.
///
/// @param expr The pipeline expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a pipeline step.
///
/// @param step The pipeline step.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerPipelineStep(PipelineStepAST* step, CodeGenContext& ctx);

/// @brief Lower a composition expression.
///
/// @param expr The composition expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a composition operand.
///
/// @param operand The composition operand.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx);

/// @brief Lower an anonymous function expression.
///
/// @param expr The anonymous function expression.
/// @param ctx The code generation context.
/// @return The LLVM value (function pointer).
llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an if expression.
///
/// @param expr The if expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a range expression.
///
/// @param expr The range expression.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* lowerRangeExpr(RangeExprAST* expr, CodeGenContext& ctx);

} // namespace codegen