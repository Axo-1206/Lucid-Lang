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
#include "types/CodeGenType.hpp"

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
std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx);

/// @brief Lower all top-level declarations in a module.
///
/// This creates LLVM types and function declarations but does NOT
/// lower function bodies. Bodies are lowered in a second pass.
void lowerModuleDeclarations(ModuleAST* module, CodeGenContext& ctx);

/// @brief Lower all function bodies in a module.
///
/// This is the second pass of function lowering. It visits each function
/// declaration and generates IR for its body.
void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx);

void generateGlobalInitializer(CodeGenContext& ctx);
void registerGlobalConstructor(llvm::Function* func, CodeGenContext& ctx);

// =============================================================================
// Declaration Lowering
// =============================================================================

/// @brief Lower a declaration to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// declaration kind.
void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a function declaration (prototype only).
///
/// Creates the llvm::Function for the declaration. Does NOT lower the body.
void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Internal function to lower a function body.
///
/// This is called for non-generic functions and for type-erased generic
/// functions. It creates the entry block, lowers parameters, and generates
/// the function body.
void lowerFunctionBodyInternal(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx);

/// @brief Lower a function body (second pass).
///
/// Generates IR for the function's body and verifies it.
void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a variable declaration.
void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a struct declaration.
///
/// Creates the LLVM struct type for the struct.
void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower an enum declaration.
///
/// Enums are lowered to integer constants.
/// @param ctx The code generation context.
void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a parameter.
///
/// Registers the parameter in the symbol table and sets up its alloca.
void lowerParam(ParamAST* param, CodeGenContext& ctx);

// =============================================================================
// Statement Lowering
// =============================================================================

/// @brief Lower a statement to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// statement kind.
void lowerStatement(StmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a block statement.
void lowerBlockStmt(BlockStmtAST* block, CodeGenContext& ctx);

/// @brief Lower an if statement.
void lowerIfStmt(IfStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a switch statement.
void lowerSwitchStmt(SwitchStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a for loop.
void lowerForStmt(ForStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a while loop.
void lowerWhileStmt(WhileStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a do-while loop.
void lowerDoWhileStmt(DoWhileStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a return statement.
void lowerReturnStmt(ReturnStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a break statement.
void lowerBreakStmt(BreakStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a continue statement.
void lowerContinueStmt(ContinueStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower an expression statement.
void lowerExprStmt(ExprStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a declaration statement.
void lowerDeclStmt(DeclStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a function reference statement.
void lowerFuncRefStmt(FuncRefStmtAST* stmt, CodeGenContext& ctx);

// ─── Concurrency Statements ─────────────────────────────────────────────

/// @brief Lower an async statement.
void lowerAsyncStmt(AsyncStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower an await statement.
void lowerAwaitStmt(AwaitStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a spawn statement.
void lowerSpawnStmt(SpawnStmtAST* stmt, CodeGenContext& ctx);

/// @brief Lower a join statement.
void lowerJoinStmt(JoinStmtAST* stmt, CodeGenContext& ctx);

// =============================================================================
// Expression Lowering
// =============================================================================

/// @brief Lower an expression to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// expression kind. Stores the result in expr->llvmValue.or.
llvm::Value* lowerExpression(ExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a literal expression.
llvm::Value* lowerLiteralExpr(LiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an identifier expression.
llvm::Value* lowerIdentifierExpr(IdentifierExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an array literal expression.
llvm::Value* lowerArrayLiteralExpr(ArrayLiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a struct literal expression.
llvm::Value* lowerStructLiteralExpr(StructLiteralExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a binary expression.
llvm::Value* lowerBinaryExpr(BinaryExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a unary expression.
llvm::Value* lowerUnaryExpr(UnaryExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a call expression.
llvm::Value* lowerCallExpr(CallExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an intrinsic call expression.
llvm::Value* lowerIntrinsicCallExpr(IntrinsicCallExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an index expression.
llvm::Value* lowerIndexExpr(IndexExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a slice expression.
///
/// @param expr The slice expression.
llvm::Value* lowerSliceExpr(SliceExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a field access expression.
llvm::Value* lowerFieldAccessExpr(FieldAccessExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a module access expression.
llvm::Value* lowerModuleAccessExpr(ModuleAccessExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a arena access expression.
llvm::Value* lowerArenaAccessExpr(ArenaAccessExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a null coalesce expression.
llvm::Value* lowerNullCoalesceExpr(NullCoalesceExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an assignment expression.
llvm::Value* lowerAssignExpr(AssignExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a pipeline expression.
llvm::Value* lowerPipelineExpr(PipelineExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a pipeline step.
llvm::Value* lowerPipelineStep(PipelineStepAST* step, llvm::Value* upstreamValue, CodeGenContext& ctx);

/// @brief Lower a composition expression.
llvm::Value* lowerComposeExpr(ComposeExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a composition operand.
llvm::Value* lowerComposeOperand(ComposeOperandAST* operand, CodeGenContext& ctx);

/// @brief Lower an anonymous function expression.
llvm::Value* lowerAnonFuncExpr(AnonFuncExprAST* expr, CodeGenContext& ctx);

/// @brief Lower an if expression.
llvm::Value* lowerIfExpr(IfExprAST* expr, CodeGenContext& ctx);

/// @brief Lower a range expression.
llvm::Value* lowerRangeExpr(RangeExprAST* expr, CodeGenContext& ctx);

} // namespace codegen