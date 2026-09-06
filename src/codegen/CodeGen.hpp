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
//
// ─── Overview ──────────────────────────────────────────────────────────────
// Declarations are lowered in two phases:
//
//   Phase 1 (lowerModuleDeclarations): Creates prototypes and types.
//   Phase 2 (lowerModuleBodies): Generates function bodies.
//
// ─── Function Dispatch ─────────────────────────────────────────────────────
// lowerDeclaration() dispatches to the appropriate handler based on kind.
// Each handler follows a consistent pattern:
//   1. Check if already lowered
//   2. Handle generic vs non-generic
//   3. Create the LLVM object
//   4. Store in context
//
// ─── Generic Strategy ─────────────────────────────────────────────────────
// Generic declarations use a hybrid strategy:
//   - DEFAULT: Type-erased (one version with tagged slots)
//   - OPT-IN (@[specialize]): Monomorphized (one per instantiation)
// See CodeGenGeneric.hpp for the full generic pipeline.

// ─── 1. Main Dispatch ──────────────────────────────────────────────────────

/// @brief Lower a declaration to LLVM IR.
///
/// Dispatches to the appropriate specific lower function based on the
/// declaration kind. This is the main entry point for declaration lowering.
///
/// @param decl The declaration to lower.
/// @param ctx The code generation context.
///
/// @note Called during Phase 1 (lowerModuleDeclarations).
void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx);

// ─── 2. Function Declarations ─────────────────────────────────────────────

/// @brief Lower a function declaration.
///
/// Creates the LLVM function prototype for a function declaration.
/// Does NOT generate the function body - that happens in lowerFunctionBody().
///
/// ─── Dispatch ────────────────────────────────────────────────────────────
///   - Foreign functions:  ExternalLinkage with raw name
///   - Generic + specialize: Nothing (lazy generation)
///   - Generic + default:   Erased function with tagged slots
///   - Normal functions:    Regular function with mangled name
///
/// @param decl The function declaration.
/// @param ctx The code generation context.
///
/// @note Called during Phase 1 (lowerModuleDeclarations).
void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a foreign function declaration.
///
/// Foreign functions are declared with @[foreign] and resolved by the
/// linker/JIT at runtime. They use the raw name (not mangled).
///
/// @param decl The function declaration (must be foreign).
/// @param ctx The code generation context.
void lowerForeignFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a generic function declaration.
///
/// Generic functions are handled differently based on @[specialize]:
///   - With @[specialize]: Registered for lazy instantiation (Phase 2)
///   - Without @[specialize]: Type-erased function with tagged slots
///
/// @param decl The generic function declaration.
/// @param ctx The code generation context.
void lowerGenericFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a normal (non-generic) function declaration.
///
/// Creates a regular LLVM function with mangled name and parameter names.
/// Also sets up closure tracking for `let` functions with captures.
///
/// @param decl The function declaration (must not be generic).
/// @param ctx The code generation context.
void lowerNormalFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Track a mutable closure function.
///
/// For `let` functions that hold closures, creates an alloca to store
/// the closure value { func_ptr, env_ptr } and marks it alive for cleanup.
///
/// @param decl The function declaration (must be `let` with closure).
/// @param func The LLVM function.
/// @param ctx The code generation context.
void trackClosureFunction(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx);

// ─── 3. Function Bodies ───────────────────────────────────────────────────

/// @brief Lower a function body (Phase 2).
///
/// Generates LLVM IR for the function's body. This is called during
/// Phase 2 after all declarations have been lowered.
///
/// ─── Dispatch ────────────────────────────────────────────────────────────
///   - Foreign functions:  Skipped (no body)
///   - Generic + specialize: Deferred (lazy instantiation)
///   - Generic + default:   Erased body with tagged slots
///   - Normal functions:    Regular body with typed parameters
///
/// @param decl The function declaration.
/// @param ctx The code generation context.
///
/// @note Called during Phase 2 (lowerModuleBodies).
void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a generic function body.
///
/// Handles body lowering for generic functions:
///   - With @[specialize]: Deferred to instantiation time
///   - Without @[specialize]: Erased body with tagged slot unpacking
///
/// @param decl The generic function declaration.
/// @param ctx The code generation context.
void lowerGenericFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a normal (non-generic) function body.
///
/// Lowers a regular function body with typed parameters.
///
/// @param decl The function declaration (must not be generic).
/// @param ctx The code generation context.
void lowerNormalFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Internal function to lower a function body.
///
/// This is the core body lowering function called for non-generic functions.
/// It creates the entry block, lowers parameters, and generates the body.
///
/// @param decl The function declaration.
/// @param func The LLVM function.
/// @param ctx The code generation context.
void lowerFunctionBodyInternal(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx);

/// @brief Lower the body of a type-erased generic function.
///
/// Type-erased generic functions use opaque pointers for all parameters.
/// Each parameter is a tagged slot { i8 tag, ptr value }.
/// This function unpacks the tagged slots and lowers the body.
///
/// @param decl The generic function declaration.
/// @param func The type-erased LLVM function.
/// @param ctx The code generation context.
void lowerErasedFunctionBody(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx);

/// @brief Lower a specialized function body for a specific instantiation.
///
/// This is called when @[specialize] is used and a concrete instantiation
/// is needed. It clones the generic function body with type substitutions.
///
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param specializedFunc The specialized LLVM function.
/// @param ctx The code generation context.
void lowerSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
);

/// @brief Instantiate a specialized function body for a generic function.
///
/// This is called from getOrCreateSpecializedFunction() when a new
/// instantiation is needed. It creates the function body with substituted types.
///
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param specializedFunc The specialized LLVM function to generate the body for.
/// @param ctx The code generation context.
void instantiateSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
);

// ─── 4. Parameter Lowering ─────────────────────────────────────────────────

/// @brief Lower a function parameter.
///
/// Each parameter gets an alloca in the function's entry block. The argument
/// value is stored into this alloca. This is necessary because:
///   1. Parameters can be referenced as l-values (e.g., &param)
///   2. Parameters can be mutable (if declared with `let`)
///   3. It provides a consistent way to access parameters
///
/// @param param The parameter declaration.
/// @param ctx The code generation context.
///
/// @note Parameters do NOT own their environment. The caller owns the
///       closure environment, not the callee. So we should NOT mark
///       parameters as alive.
void lowerParam(ParamAST* param, CodeGenContext& ctx);

// ─── 5. Variable Declarations ─────────────────────────────────────────────

/// @brief Lower a variable declaration.
///
/// Variables can be either module-level (globals) or local (allocas).
/// Module-level variables use mangled names for AOT compilation.
///
/// @param decl The variable declaration.
/// @param ctx The code generation context.
void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a global variable.
///
/// Creates an LLVM GlobalVariable with the mangled name.
/// Handles constant initialization and deferred non-constant init.
///
/// @param decl The variable declaration (must be module-level).
/// @param varType The LLVM type of the variable.
/// @param ctx The code generation context.
void lowerGlobalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx);

/// @brief Lower a local variable.
///
/// Creates an LLVM AllocaInst for the variable.
/// Handles initialization and marks alive if it owns heap memory.
///
/// @param decl The variable declaration (must be local).
/// @param varType The LLVM type of the variable.
/// @param ctx The code generation context.
void lowerLocalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx);

// ─── 6. Struct Declarations ───────────────────────────────────────────────

/// @brief Lower a struct declaration to an LLVM struct type.
///
/// ─── Generic Struct Handling ────────────────────────────────────────────
/// Generic structs follow the hybrid strategy:
///   - With @[specialize]: Lazy generation per instantiation
///   - Without @[specialize]: Type-erased with tagged slots
///
/// ─── Self-Reference Handling ────────────────────────────────────────────
/// Self-referential structs are handled by:
///   1. Creating an opaque (incomplete) struct type first
///   2. Caching the opaque type
///   3. Building field types (self-reference returns a pointer)
///   4. Setting the body with all field types
///
/// @param decl The struct declaration.
/// @param ctx The code generation context.
void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a generic struct declaration.
///
/// Handles generic structs based on @[specialize]:
///   - With @[specialize]: Registered for lazy instantiation
///   - Without @[specialize]: Type-erased with tagged slots
///
/// @param decl The generic struct declaration.
/// @param ctx The code generation context.
void lowerGenericStructDecl(StructDeclAST* decl, CodeGenContext& ctx);

/// @brief Lower a normal (non-generic) struct declaration.
///
/// Creates an LLVM struct type with the mangled name.
/// Handles self-referential structs via opaque type creation.
///
/// @param decl The struct declaration (must not be generic).
/// @param ctx The code generation context.
void lowerNormalStructDecl(StructDeclAST* decl, CodeGenContext& ctx);

// ─── 7. Enum Declarations ─────────────────────────────────────────────────

/// @brief Lower an enum declaration to integer constants.
///
/// Enums in Lucid are simple integer enumerations. Each variant has an
/// explicit integer value. Lowered to LLVM integer constants.
///
/// @param decl The enum declaration.
/// @param ctx The code generation context.
void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx);

// ─── 8. Helper Functions ──────────────────────────────────────────────────

/// @brief Verify an LLVM function.
///
/// Checks the function for validity and reports errors.
///
/// @param func The LLVM function to verify.
/// @param loc The source location for error reporting.
/// @param ctx The code generation context.
void verifyFunction(llvm::Function* func, const SourceLocation& loc, CodeGenContext& ctx);

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