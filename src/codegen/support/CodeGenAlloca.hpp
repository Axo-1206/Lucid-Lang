/// @file support/CodeGenAlloca.hpp
/// @brief Memory allocation and basic block management helpers for code generation.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides low-level LLVM IR construction helpers for managing
/// memory (allocas) and control flow (basic blocks). These are the building
/// blocks used throughout CodeGen to create function-local variables and
/// control flow structures.
///
/// ─── Why a Separate File? ──────────────────────────────────────────────────
/// These helpers are used by virtually every CodeGen component:
///   - CodeGenDecl.cpp  → creates allocas for parameters and local variables
///   - CodeGenStmt.cpp  → creates basic blocks for if/while/for/switch
///   - CodeGenExpr.cpp  → creates temporary allocas for complex expressions
///   - CodeGenClosure.cpp → creates allocas for closure environments
///
/// By keeping them in a single file, we:
///   1. Avoid duplicating the alloca-creation logic across 5+ files
///   2. Ensure consistent behavior (always in entry block, proper naming)
///   3. Provide a single place to update if LLVM's API changes
///   4. Keep the core CodeGen files focused on AST → IR translation logic
///
/// ─── Memory Management: Who Frees Alloca Memory? ──────────────────────────
/// ════════════════════════════════════════════════════════════════════════════
/// 
/// **SHORT ANSWER: No one needs to free it.**
/// 
/// `alloca` allocates memory on the STACK, not the heap. Stack memory is
/// automatically freed when the function returns. This is a fundamental
/// property of how functions work in all compiled languages.
///
/// ─── Detailed Explanation ──────────────────────────────────────────────────
///
/// 1. Stack Allocation (alloca) - AUTO-FREED
///    ┌───────────────────────────────────────────────────────────┐
///    │  createAlloca() → LLVM `alloca` instruction               │
///    │  Memory lives on the stack frame                          │
///    │  Automatically freed when the function returns            │
///    │  No manual free required                                  │
///    │  Very fast (just moves stack pointer)                     │
///    └───────────────────────────────────────────────────────────┘
///
/// 2. Heap Allocation (malloc/__lucid_alloc) - MUST FREE
///    ┌───────────────────────────────────────────────────────────┐
///    │  __lucid_alloc() → Runtime heap allocation                │
///    │  Memory lives on the heap                                 │
///    │  Must be explicitly freed with __lucid_free()             │
///    │  Managed by runtime (GC or reference counting)            │
///    └───────────────────────────────────────────────────────────┘
///
/// ─── Why Alloca Doesn't Need Free ─────────────────────────────────────────
///
/// The stack is a LIFO (Last-In-First-Out) data structure:
///   - When a function is called: stack grows (push frame)
///   - When a function returns: stack shrinks (pop frame)
///   - All `alloca` memory is in the frame → automatically freed on pop
///
/// LLVM's `alloca` instruction documentation:
///   "The `alloca` instruction allocates memory on the stack frame of the
///    currently executing function. This memory is automatically released
///    when the function returns."
///
/// Additionally, LLVM's `mem2reg` optimization promotes `alloca` to SSA
/// registers when possible, eliminating the memory allocation entirely.
///
/// ─── Summary ──────────────────────────────────────────────────────────────
///
/// | Allocation Method | Memory Type | Who Frees? | When?      | File                 |
/// | ----------------- | ----------- | ---------- | ---------- | -------------------- |
/// | createAlloca()    | Stack       | Automatic  | Return     | CodeGenAlloca.hpp    |
/// | __lucid_alloc()   | Heap        | Runtime    | Explicit   | IntrinsicEmitter.cpp |
/// | arena_alloc()     | Arena       | Runtime    | reset/free | IntrinsicEmitter.cpp |
///
/// ─── Key Helpers ────────────────────────────────────────────────────────────
///
///   createAlloca()  → Creates an alloca in the current function's entry block.
///                     This is the standard way to allocate stack space for
///                     local variables and parameters in LLVM.
///
///   createBlock()   → Creates a new basic block in the current function.
///                     Used for if/else branches, loop bodies, switch cases,
///                     and merge points.
///
///   loadIfNeeded()  → Conditionally loads a value from a pointer.
///                     With LLVM's opaque pointer model (v17+), we must
///                     explicitly provide the element type when loading.
///                     The deprecated overload exists for backward compatibility.
///
/// ─── LLVM Opaque Pointer Context ──────────────────────────────────────────
/// Starting with LLVM 17, pointers no longer carry element type information
/// (opaque pointers). This means:
///   - `ptr` is the universal pointer type
///   - `getPointerElementType()` is removed
///   - `CreateLoad()` now requires the element type as the first argument
///
/// Our helpers handle this complexity so that other CodeGen files don't need
/// to worry about LLVM version differences.
///
/// ─── Usage Example ─────────────────────────────────────────────────────────
/// @code
/// // Create a local variable (stack allocation - auto-freed)
/// llvm::Type* intType = llvm::Type::getInt32Ty(ctx.llvmCtx);
/// llvm::AllocaInst* alloca = createAlloca("counter", intType, ctx);
/// // No free needed - automatically freed when function returns
///
/// // Store a value
/// ctx.builder.CreateStore(value, alloca);
///
/// // Later, load the value (with explicit element type)
/// llvm::Value* loaded = loadIfNeeded(alloca, intType, ctx);
///
/// // Create a branch target
/// llvm::BasicBlock* loopBody = createBlock("loop_body", ctx);
/// ctx.builder.CreateBr(loopBody);
/// ctx.builder.SetInsertPoint(loopBody);
/// @endcode

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <string>

namespace codegen {

/// @brief Create an alloca instruction in the current function's entry block.
/// @param name The name for the alloca (for debugging).
/// @param type The LLVM type to allocate.
/// @param ctx The code generation context.
/// @return The alloca instruction, or nullptr if no current function.
///
/// @note All allocas are placed in the entry block to ensure they're always
///       available throughout the function. This matches LLVM's best practice
///       and enables the mem2reg optimization pass.
///
/// @warning This allocates STACK memory, which is automatically freed when
///          the function returns. DO NOT call free() on the result.
llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
);

/// @brief Create a new basic block in the current function.
/// @param name The name for the block (for debugging).
/// @param ctx The code generation context.
/// @return The new basic block, or nullptr if no current function.
///
/// @note The block is not automatically inserted into the control flow.
///       The caller must branch to it and set the insertion point.
llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
);

/// @brief Load a value from a pointer with explicit element type.
/// @param value The pointer value to load from.
/// @param elemType The element type (for opaque pointers).
/// @param ctx The code generation context.
/// @return The loaded value, or the original value if not a pointer.
///
/// @note With LLVM's opaque pointer model, the element type MUST be provided
///       explicitly. This helper handles the check: if the value is not a
///       pointer, it's returned unchanged.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
);

} // namespace codegen