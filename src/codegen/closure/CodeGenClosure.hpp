/// @file closure/CodeGenClosure.hpp
/// @brief Closure lowering declarations - separate from main CodeGen interface.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file contains the declarations for closure-related code generation
/// functions. Closures are a complex feature with their own specialized
/// lowering logic, so they're separated into their own module.
///
/// ─── Why Separate from CodeGen.hpp? ───────────────────────────────────────
/// 1. **Complexity**: Closure lowering is complex (environment structs,
///    capture analysis, fat pointers, heap allocation). Keeping it separate
///    keeps CodeGen.hpp focused on the core AST → IR translation.
///
/// 2. **Dependencies**: Closure code depends on runtime functions
///    (__lucid_alloc_env) and has its own helper functions. This separation
///    keeps the main CodeGen.hpp clean.
///
/// 3. **Maintainability**: When fixing closure bugs or adding features
///    (e.g., non-escaping closure optimization), developers know exactly
///    where to look.
///
/// 4. **Compilation Speed**: Changes to closure code don't trigger
///    recompilation of all CodeGen files.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/trace/Trace.hpp"
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>

namespace codegen {

/// @brief Lower a closure (anonymous function with captures).
///
/// Creates the closure environment struct, the closure function, and
/// the closure value (function pointer + environment pointer).
///
/// ─── Closure Lowering Steps ──────────────────────────────────────────────
/// 1. Build the closure environment struct from captured variables
/// 2. Create the closure function (takes env pointer + regular params)
/// 3. Allocate the environment on the heap (via __lucid_alloc_env)
/// 4. Fill the environment with captured variable values
/// 5. Create the closure value (fat pointer: {func, env})
///
/// @param expr The anonymous function expression.
/// @param ctx The code generation context.
/// @return The closure value (fat pointer), or nullptr on error.
llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx);

/// @brief Build the closure environment struct.
///
/// Analyzes the captures and creates an LLVM struct type where each
/// field corresponds to a captured variable.
///
/// @param expr The anonymous function expression.
/// @param ctx The code generation context.
/// @return The LLVM struct type for the environment.
llvm::StructType* buildClosureEnvironment(AnonFuncExprAST* expr, CodeGenContext& ctx);

/// @brief Create the closure function.
///
/// Generates the LLVM function that implements the closure body.
/// The function takes the environment pointer as its first argument,
/// followed by the regular parameters.
///
/// @param expr The anonymous function expression.
/// @param ctx The code generation context.
/// @return The LLVM function.
llvm::Function* createClosureFunction(AnonFuncExprAST* expr, CodeGenContext& ctx);

/// @brief Emit a call to a closure.
///
/// Generates the IR to call a closure value (fat pointer).
/// The environment pointer is passed as the first argument.
///
/// @param funcPtr The closure function pointer.
/// @param envPtr The environment pointer.
/// @param args The arguments.
/// @param returnType The LLVM return type, derived by the caller from the
///        call expression's resolved type (sema sets this in resolveCallExpr
///        as funcType->returnType). Pass void's LLVM type explicitly for
///        void-returning calls (e.g. #scope_exit callbacks) - there is no
///        implicit default anymore.
/// @param ctx The code generation context.
/// @return The LLVM value.
llvm::Value* emitClosureCall(
    llvm::Value* funcPtr,
    llvm::Value* envPtr,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::Type* returnType,
    CodeGenContext& ctx
);

/// @brief Check if a closure is needed.
///
/// A closure is needed if the anonymous function has captures or
/// is explicitly marked as a closure.
///
/// @param expr The anonymous function expression.
/// @return True if a closure is needed.
bool isClosureNeeded(const AnonFuncExprAST* expr);

} // namespace codegen