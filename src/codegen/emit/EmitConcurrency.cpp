/// @file codegen/emit/EmitConcurrency.cpp
/// @brief Concurrency lowering: async, spawn, await, join.
///
/// ─── The New ABI ──────────────────────────────────────────────────────────
/// The runtime functions in `src/runtime/` and `functions.def` already use
/// the new ABI:
///
///     void __lucid_async(void* thunk, void* packet, void** out);
///     void __lucid_spawn(void* thunk, void* packet, void** out);
///     void* __lucid_await(void** handle_slot);
///     void* __lucid_join (void** handle_slot);
///
/// The emitter's job is to build the thunk and the packet for each
/// async/spawn expression, call the runtime, and interpret the handle.
///
/// ─── The Thunk ────────────────────────────────────────────────────────────
/// The thunk is a generated LLVM function with signature
/// `void* (void* packet)`. It:
///
///   1. Loads each argument from the packet's corresponding field.
///   2. Frees the packet.
///   3. Calls the real function.
///   4. Boxes the result (allocates a slot, stores the result into it,
///      returns a pointer to the slot).
///
/// The boxing is what gives the thunk a uniform `void*` return type
/// regardless of the user function's actual return type. `await`/`join`
/// load `T` from the box and free it.
///
/// ─── The Packet ───────────────────────────────────────────────────────────
/// The packet is a heap-allocated struct with one field per argument.
/// The emitter allocates it via `__lucid_alloc`, stores each argument
/// into its field, and passes the packet pointer to the runtime.
///
/// The packet must live on the heap (not the caller's stack) because the
/// runtime may execute the thunk on a different thread after the caller's
/// stack frame is gone.
///
/// ─── Discard Pattern ──────────────────────────────────────────────────────
/// `spawn _ = f()` passes a null `out` pointer. The runtime spawns the
/// thunk and discards the handle; the thunk still runs to completion and
/// releases its box.
///
/// ─── Why This File Is Separate ────────────────────────────────────────────
/// Concurrency lowering is a self-contained subsystem. The thunk and
/// packet builders are only used by the four statement emitters in this
/// file. Keeping them out of `EmitStmt.cpp` keeps that file focused on
/// ordinary control flow.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitAsyncStmt
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitAsyncStmt(AsyncStmtAST* stmt) {
    if (!stmt || !stmt->call || !stmt->call->isa<CallExprAST>()) return;

    CallExprAST* call = stmt->call->as<CallExprAST>();
    llvm::IRBuilder<>& b = program.builder();

    // ─── Step 1: Build the thunk ──────────────────────────────────────────
    TypeAST* innerTy = stmt->binding ? stmt->binding->type : nullptr;
    llvm::Function* thunk = buildConcurrencyThunk(call, innerTy);
    if (!thunk) return;

    // ─── Step 2: Build the packet ─────────────────────────────────────────
    llvm::Value* packet = buildConcurrencyPacket(call);
    if (!packet) return;

    // ─── Step 3: Allocate the handle slot ─────────────────────────────────
    llvm::AllocaInst* handleSlot = b.CreateAlloca(
        llvm::PointerType::get(program.llvmContext(), 0),
        nullptr,
        program.pool.lookup(stmt->binding->name) + "_future");

    // ─── Step 4: Call __lucid_async ───────────────────────────────────────
    llvm::Function* asyncFn =
        program.abi().declareOrGet(RuntimeFn::Async);
    llvm::Value* thunkPtr = b.CreatePointerCast(
        thunk, llvm::PointerType::get(program.llvmContext(), 0));
    b.CreateCall(asyncFn, {thunkPtr, packet, handleSlot});

    // ─── Step 5: Store the handle in the binding ──────────────────────────
    // The binding holds a pointer to the handle; `store` gives it to
    // the binding's alloca. The handle is a `Handle`-kind resource, so
    // it's tracked for the await to consume.
    llvm::Value* handle = b.CreateLoad(
        llvm::PointerType::get(program.llvmContext(), 0), handleSlot);
    Val handleVal{handle, stmt->binding->type, Own::Owned};

    // Store into the binding's storage. The binding's alloca is created
    // by the emitter's variable-declaration path (Sema synthesized a
    // VarDeclAST as the binding).
    Place place = emitIdentifierPlace(...);  // or create the alloca here
    store(place, handleVal, stmt->binding);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitAwaitStmt
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitAwaitStmt(AwaitStmtAST* stmt) {
    // `await x` where x is a binding holding a Future<T>:
    //   1. Load the handle pointer from the binding's storage.
    //   2. Call __lucid_await(&handle_ptr) — passes the address of the
    //      storage so the runtime can clear it.
    //   3. The runtime returns a box pointer (or null for a void result).
    //   4. Load T from the box.
    //   5. Free the box.
    //   6. Store T into the binding's storage.

    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) continue;
        IdentifierExprAST* id = target->as<IdentifierExprAST>();
        ValueDeclAST* decl = id->resolvedDecl;
        if (!decl) continue;

        Place place = emitIdentifierPlace(id);
        if (!place.isValid()) continue;

        llvm::IRBuilder<>& b = program.builder();
        llvm::Function* awaitFn =
            program.abi().declareOrGet(RuntimeFn::Await);

        // Pass the address of the binding's storage.
        llvm::Value* box = b.CreateCall(awaitFn, {place.ptr}, "await_box");

        // Load T from the box (or skip if void).
        TypeAST* innerTy = decl->type && decl->type->isa<FutureTypeAST>()
            ? decl->type->as<FutureTypeAST>()->inner
            : nullptr;
        if (innerTy && program.types().get(innerTy)->isSized()) {
            llvm::Type* innerLlvmTy = program.types().get(innerTy);
            llvm::Value* typedBox = b.CreatePointerCast(
                box, llvm::PointerType::get(innerLlvmTy, 0), "typed_box");
            llvm::Value* result = b.CreateLoad(
                innerLlvmTy, typedBox, "await_result");

            // Free the box.
            llvm::Function* freeFn =
                program.abi().declareOrGet(RuntimeFn::Free);
            b.CreateCall(freeFn, {box});

            // Store the result into the binding. The binding's storage
            // still holds the handle; drop the handle first (no-op) and
            // store the result.
            Val resultVal{result, innerTy, Own::Owned};
            store(place, resultVal, decl);
        }

        // Mark the binding as consumed (the handle has been consumed by
        // the runtime).
        func().markConsumed(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitSpawnStmt
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitSpawnStmt(SpawnStmtAST* stmt) {
    // Symmetric to emitAsyncStmt but for `spawn`.
    // If stmt->binding is null (discard pattern), pass a null `out` slot.
    // ...
}

// ─────────────────────────────────────────────────────────────────────────────
// emitJoinStmt
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitJoinStmt(JoinStmtAST* stmt) {
    // Symmetric to emitAwaitStmt but for `join`.
    // ...
}

// ─────────────────────────────────────────────────────────────────────────────
// buildConcurrencyThunk
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Emitter::buildConcurrencyThunk(CallExprAST* call,
                                                TypeAST* returnTy) {
    // Generate: `void* thunk_N(void* packet)`. Unique per call site.
    //
    // Body:
    //   1. GEP to each packet field, load the argument.
    //   2. Free the packet.
    //   3. Call the real function with the loaded arguments.
    //   4. Box the result: alloca, store, return the alloca as void*.
    //      For a void return, return null.
    //
    // The thunk is emitted at the module level, not inside the calling
    // function, so the closure-lowering pattern of saving the insertion
    // point applies.
    // ...
}

// ─────────────────────────────────────────────────────────────────────────────
// buildConcurrencyPacket
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* Emitter::buildConcurrencyPacket(CallExprAST* call) {
    // Build an LLVM struct with one field per argument; allocate via
    // __lucid_alloc; store each evaluated argument into its field.
    //
    // Returns a pointer to the packet as `void*`.
    // ...
}

} // namespace codegen