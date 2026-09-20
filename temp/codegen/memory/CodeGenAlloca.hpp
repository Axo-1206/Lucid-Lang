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

/// ───────────────────────────────────────────────────────────────────────────
/// createAlloca — Allocate Stack Memory for a Local Variable
/// ───────────────────────────────────────────────────────────────────────────
///
/// @brief Create an `alloca` instruction in the current function's entry block.
/// @param name  The debug name for the alloca (appears in LLVM IR).
/// @param type  The LLVM type to allocate (e.g., i32, double, { i32, i32 }).
/// @param ctx   The code generation context.
/// @return      The alloca instruction, or nullptr if no current function.
///
/// ─── Why Place Allocas in the Entry Block? ──────────────────────────────
///
/// All LLVM `alloca` instructions for a function should be grouped together
/// at the top of the entry block. This is not just a style convention — it's
/// required for the `mem2reg` optimization pass to work correctly.
///
/// The `mem2reg` pass (Memory to Register promotion) converts `alloca`-based
/// variables into SSA registers when possible. This eliminates the memory
/// access (load/store) and replaces it with direct register usage, which is
/// much faster.
///
/// For `mem2reg` to promote an alloca, the alloca MUST be in the entry block
/// AND all uses must be dominated by the alloca. By placing allocas at the
/// very start of the function, we guarantee the dominance condition.
///
/// ─── Stack vs Heap Memory ──────────────────────────────────────────────────
///
/// ════════════════════════════════════════════════════════════════════════════
/// IMPORTANT: `alloca` allocates memory on the STACK, NOT the heap.
/// ════════════════════════════════════════════════════════════════════════════
///
/// Stack memory is automatically freed when the function returns. There is
/// NO manual free operation. The stack frame is popped when the function
/// exits, and all `alloca` memory goes with it.
///
///   ┌────────────────────────────────────────────────────────────────────┐
///   │  // This memory lives on the stack:                                │
///   │  llvm::AllocaInst* alloca = createAlloca("x", intType, ctx);       │
///   │  // NO free needed — automatically freed on function return        │
///   └────────────────────────────────────────────────────────────────────┘
///
/// ─── Lifetime and Visibility ──────────────────────────────────────────────
///
/// An `alloca` remains valid for the entire lifetime of the function.
/// However, the *logical* variable may go out of scope earlier (e.g., inside
/// a loop or block). In those cases, you can:
///   - Leave the alloca allocated (memory stays reserved).
///   - The variable is "dead" conceptually, but the alloca remains.
///   - The optimizer may reuse the alloca for other dead variables.
///
/// ─── Usage Example ──────────────────────────────────────────────────────────
///
/// @code
/// // Allocate a 32-bit integer on the stack named "counter"
/// llvm::Type* intTy = llvm::Type::getInt32Ty(ctx.llvmCtx);
/// llvm::AllocaInst* counter = createAlloca("counter", intTy, ctx);
///
/// // Store an initial value
/// ctx.builder.CreateStore(
///     llvm::ConstantInt::get(intTy, 0),
///     counter
/// );
///
/// // Later, load the value
/// llvm::Value* val = ctx.builder.CreateLoad(intTy, counter, "counter_val");
///
/// // Store a new value
/// ctx.builder.CreateStore(val2, counter);
/// @endcode
///
/// ─── Error Handling ────────────────────────────────────────────────────────
///
/// If `getCurrentFunction()` returns nullptr (e.g., we're generating
/// top-level code outside any function), `createAlloca` returns nullptr.
/// Callers should check the return value or assert that the current
/// function is set before calling this helper.
///
/// @see CodeGenContext::getCurrentFunction()
/// @see CodeGenContext::pushFunction()
/// @see CodeGenContext::popFunction()
///
llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
);

/// ───────────────────────────────────────────────────────────────────────────
/// createBlock — Create a Control Flow Basic Block
/// ───────────────────────────────────────────────────────────────────────────
///
/// @brief Create a new basic block and attach it to the current function.
/// @param name  The debug name for the block (appears in LLVM IR).
/// @param ctx   The code generation context.
/// @return      The new basic block, or nullptr if no current function.
///
/// ─── What Is a Basic Block? ────────────────────────────────────────────────
///
/// In LLVM IR, a "Basic Block" is a straight-line sequence of instructions
/// that has exactly one entry point (the first instruction) and exactly one
/// exit point (the last instruction, which must be a terminator like `br`,
/// `ret`, or `switch`).
///
/// Control flow is built by connecting basic blocks with branch instructions.
/// This is how LLVM represents `if`, `while`, `for`, `switch`, and function
/// calls with multiple return paths.
///
/// ─── Typical Block Structure ──────────────────────────────────────────────
///
/// ┌─────────────────────────────────────────────────────────────────────┐
/// │  if (cond) {          →  condBlock:                                 │
/// │      doSomething();   →      %cond = ...                            │
/// │  } else {             →      br i1 %cond, label %then, %else        │
/// │      doOther();       →                                             │
/// │  }                    →  thenBlock:                                 │
/// │                       →      call doSomething()                     │
/// │                       →      br %merge                              │
/// │                       →                                             │
/// │                       →  elseBlock:                                 │
/// │                       →      call doOther()                         │
/// │                       →      br %merge                              │
/// │                       →                                             │
/// │                       →  mergeBlock:                                │
/// │                       →      ... continue ...                       │
/// └─────────────────────────────────────────────────────────────────────┘
///
/// ─── Block Creation Pattern ───────────────────────────────────────────────
///
/// The typical pattern for creating and using blocks is:
///
/// @code
/// // 1. Create blocks
/// llvm::BasicBlock* condBlock = createBlock("if_cond", ctx);
/// llvm::BasicBlock* thenBlock = createBlock("if_then", ctx);
/// llvm::BasicBlock* elseBlock = createBlock("if_else", ctx);
/// llvm::BasicBlock* mergeBlock = createBlock("if_merge", ctx);
///
/// // 2. Branch to condition block
/// ctx.builder.CreateBr(condBlock);
///
/// // 3. Generate condition block
/// ctx.builder.SetInsertPoint(condBlock);
/// llvm::Value* cond = lowerExpression(stmt->condition, ctx);
/// ctx.builder.CreateCondBr(cond, thenBlock, elseBlock);
///
/// // 4. Generate then block
/// ctx.builder.SetInsertPoint(thenBlock);
/// lowerStatement(stmt->thenBranch, ctx);
/// ctx.builder.CreateBr(mergeBlock);
///
/// // 5. Generate else block
/// ctx.builder.SetInsertPoint(elseBlock);
/// lowerStatement(stmt->elseBranch, ctx);
/// ctx.builder.CreateBr(mergeBlock);
///
/// // 6. Continue in merge block
/// ctx.builder.SetInsertPoint(mergeBlock);
/// @endcode
///
/// ─── Important: Blocks Must Have Terminators ─────────────────────────────
///
/// Every basic block MUST end with a terminator instruction (`br`, `ret`,
/// `switch`, `unreachable`, etc.). If you forget to add a terminator, LLVM
/// will assert/error when you try to verify the module.
///
/// The helper creates an empty block — the caller is responsible for:
///   1. Setting the insertion point to the new block.
///   2. Generating instructions inside the block.
///   3. Ending the block with a terminator.
///
/// ─── Naming Convention ────────────────────────────────────────────────────
///
/// Use descriptive names for blocks to make the LLVM IR readable:
///   - `if_cond`, `if_then`, `if_else`, `if_merge`
///   - `while_cond`, `while_body`, `while_cont`, `while_exit`
///   - `for_header`, `for_body`, `for_inc`, `for_exit`
///   - `switch_<case_value>`, `switch_default`, `switch_merge`
///
/// @see llvm::BasicBlock
/// @see llvm::IRBuilder::SetInsertPoint()
/// @see llvm::IRBuilder::CreateBr()
/// @see llvm::IRBuilder::CreateCondBr()
///
llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
);

/// ───────────────────────────────────────────────────────────────────────────
/// loadIfNeeded — Conditionally Load a Value From Memory
/// ───────────────────────────────────────────────────────────────────────────
///
/// @brief Load a value from memory if the operand is a pointer.
/// @param value    The value to potentially load from.
/// @param elemType The element type (required for opaque pointers).
/// @param ctx      The code generation context.
/// @return         The loaded value if `value` is a pointer,
///                 or the original value if it's already a value.
///
/// ─── Why Is This Helper Needed? ──────────────────────────────────────────
///
/// LLVM 17+ uses "opaque pointers" where the pointer type `ptr` does NOT
/// carry element type information. In older LLVM, you could write:
///
///     CreateLoad(pointer)  // Infer type from pointer->getElementType()
///
/// That API is DEPRECATED/REMOVED. Now you MUST write:
///
///     CreateLoad(elementType, pointer)  // Explicit type required
///
/// This helper abstracts away the pointer check and the explicit type
/// requirement, so the rest of CodeGen can be simpler.
///
/// ─── Lvalue vs Rvalue Context ──────────────────────────────────────────────
///
/// In a compiler, expressions can produce either:
///
///   - **Lvalue**: A memory location (e.g., a variable `x`). It has a pointer
///     type and you can store to it.
///   - **Rvalue**: A computed value (e.g., `x + 1`). It has a value type
///     and cannot be stored to directly.
///
/// When generating code, you often need the *value* at a memory location.
/// For example, in `x + 1`, you need to load `x` from memory first.
/// This helper handles that by checking if the value is a pointer and
/// loading it if necessary.
///
/// ─── When to Use This Helper ──────────────────────────────────────────────
///
/// Use `loadIfNeeded` when you have an expression result and you need the
/// actual value for an operation. For example:
///
/// @code
/// // Lower the left-hand side (may be an Lvalue)
/// llvm::Value* left = lowerExpression(binOp->left, ctx);
/// // Lower the right-hand side
/// llvm::Value* right = lowerExpression(binOp->right, ctx);
///
/// // Load both sides if they're Lvalues
/// llvm::Type* resultType = getType(ctx, binOp->resolvedType);
/// left = loadIfNeeded(left, resultType, ctx);
/// right = loadIfNeeded(right, resultType, ctx);
///
/// // Now left and right are values, not pointers
/// llvm::Value* sum = ctx.builder.CreateAdd(left, right, "add");
/// @endcode
///
/// ─── When NOT to Use This Helper ────────────────────────────────────────────
///
/// Do NOT use `loadIfNeeded` when:
///   - You need the address for a store operation (e.g., `x = 5`).
///   - You're passing a pointer to a function that expects a pointer.
///   - You're computing `&x` (taking the address).
///
/// In these cases, you want the pointer itself, not the loaded value.
///
/// ─── Type Safety ──────────────────────────────────────────────────────────
///
/// The `elemType` parameter should match the actual element type of the
/// pointer. If you pass the wrong type, LLVM will generate an incorrect load
/// (e.g., loading i32 from an i8*), which can lead to miscompilation or
/// crashes. Always pass the type from the AST's `resolvedType`.
///
/// ─── Handling Non-Pointer Values ──────────────────────────────────────────
///
/// If `value` is not a pointer (e.g., it's an integer constant or an
/// arithmetic result), the helper returns it unchanged. This allows the same
/// code path to work for both Lvalues and Rvalues seamlessly.
///
/// ─── Example: Lowering an Identifier ──────────────────────────────────────
///
/// @code
/// // Lower an identifier expression (e.g., `x`)
/// llvm::Value* lowerIdentifier(IdentifierExprAST* expr, CodeGenContext& ctx) {
///     // Look up the variable's alloca
///     llvm::AllocaInst* alloca = ctx.lookupValue(expr->resolvedDecl);
///     if (!alloca) return nullptr;
///
///     // Get the type from the AST
///     llvm::Type* type = getType(ctx, expr->resolvedType);
///
///     // Load if needed — this converts the alloca (pointer) to a value
///     return loadIfNeeded(alloca, type, ctx);
/// }
/// @endcode
///
/// ─── LLVM Version Compatibility ────────────────────────────────────────────
///
/// This helper is designed to work with LLVM 17+ (opaque pointers). It does
/// NOT attempt to handle the old typed pointer API. If your project uses an
/// older LLVM version, you'll need to adapt this helper.
///
/// @see llvm::IRBuilder::CreateLoad()
/// @see llvm::Value::getType()
/// @see llvm::Type::isPointerTy()
///
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
);

} // namespace codegen