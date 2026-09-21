/// @file codegen/emit/EmitConcurrency.cpp
/// @brief Concurrency statement lowering — async, spawn, await, join.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The four concurrency statements and their two support builders:
///
///   - `emitAsyncStmt`       — `async const x int = f(a, b)`.
///   - `emitAwaitStmt`       — `await x`.
///   - `emitSpawnStmt`       — `spawn const x int = f(a, b)`,
///                             `spawn _ = f(a, b)`.
///   - `emitJoinStmt`        — `join x`.
///   - `buildConcurrencyThunk`  — the synthesized `void* (void*)` thunk.
///   - `buildConcurrencyPacket` — the heap-allocated argument packet.
///
/// ─── The Concurrency ABI ──────────────────────────────────────────────────
/// From `functions.def` and `src/runtime/ConcurrencyEntry.cpp`:
///
///     void  __lucid_async(void* thunk, void* packet, void** out);
///     void  __lucid_spawn(void* thunk, void* packet, void** out);
///     void* __lucid_await(void** handle_slot);
///     void* __lucid_join (void** handle_slot);
///
/// `thunk` is a compiler-emitted `void* (*)(void*)`. It takes the packet,
/// unpacks the arguments, frees the packet, calls the real function,
/// boxes the result (allocates a slot, stores the result into it),
/// and returns the box pointer. For a void function, it returns a
/// 1-byte dummy box.
///
/// `packet` is a heap-allocated `__lucid_alloc` block holding the call's
/// arguments, one field per argument.
///
/// `out` is a `void**` slot. Non-null: the runtime writes the handle into
/// `*out`. Null: fire-and-forget.
///
/// `handle_slot` (for `await`/`join`) is the address of the binding's
/// handle storage. The runtime consumes the handle, waits for completion,
/// and returns the box. The emitter loads `T` from the box, frees the
/// box, and stores `T` into the binding's value storage.
///
/// ─── Two Slots Per Binding ────────────────────────────────────────────────
/// A `Future<T>` or `Thread<T>` binding has TWO pieces of storage:
///
///   - **Handle slot**: a `ptr` holding the runtime handle. Written by
///     `__lucid_async`/`__lucid_spawn`; consumed (set to null) by
///     `__lucid_await`/`__lucid_join`.
///
///   - **Value slot**: a `T` holding the eventual result. Written by
///     `await`/`join` after the runtime returns the box and the emitter
///     loads `T` from it.
///
/// The binding's storage in the `FunctionState` value map is the **value
/// slot** — that's what `emitIdentifier` loads `T` from after narrowing.
/// The handle slot is tracked in a side table (`FunctionState`'s
/// `concurrencyHandles`) so `await`/`join` can find it.
///
/// ─── Required FunctionState Support ──────────────────────────────────────
/// This file assumes `FunctionState` has:
///
///     void storeHandle(ValueDeclAST* decl, llvm::Value* slot);
///     llvm::Value* lookupHandle(ValueDeclAST* decl) const;
///
/// Backed by a `std::unordered_map<ValueDeclAST*, llvm::Value*>` field.
/// The methods are populated by `emitAsyncStmt`/`emitSpawnStmt` and read
/// by `emitAwaitStmt`/`emitJoinStmt`.
///
/// ─── Thunk Naming ────────────────────────────────────────────────────────
/// Each async/spawn site produces one thunk. The name is `async.thunk.N`
/// or `spawn.thunk.N` with a global counter. Internal linkage; the thunk
/// is only reachable through the runtime's stored reference.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>
#include <string>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Thunk and packet name counters
// ─────────────────────────────────────────────────────────────────────────────
// Process-global counters. A process runs one `generate()` at a time, so
// the counters are fine as plain `static`s. A future multi-threaded
// codegen would make them atomic or move them onto `ProgramState`.

namespace {

uint64_t nextThunkId() {
    static uint64_t counter = 0;
    return counter++;
}

uint64_t nextPacketId() {
    static uint64_t counter = 0;
    return counter++;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// emitAsyncStmt — `async const x T = f(args)`
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Steps ────────────────────────────────────────────────────────────────
//   1. Resolve the binding and the call.
//   2. Determine `T` (the inner type of `Future<T>`).
//   3. Allocate two slots: a `ptr` handle slot and a `T` value slot.
//   4. Register the value slot as the binding's storage.
//   5. Record the handle slot in the side table.
//   6. Build the thunk (via `buildConcurrencyThunk`).
//   7. Build the packet (via `buildConcurrencyPacket`).
//   8. Call `__lucid_async(thunk, packet, handleSlot)`.
//   9. Mark the binding alive (a formality — `Handle` drops are no-ops).

void Emitter::emitAsyncStmt(AsyncStmtAST* stmt) {
    assert(stmt && "emitAsyncStmt() with null statement");

    if (!stmt->call || !stmt->call->isa<CallExprAST>()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->loc,
            "async statement requires a call expression");
        return;
    }
    if (!stmt->binding) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->loc,
            "async statement has no binding");
        return;
    }

    CallExprAST* call = stmt->call->as<CallExprAST>();
    VarDeclAST* binding = stmt->binding;

    // ─── Inner type `T` ───────────────────────────────────────────────────
    // The binding's type is `Future<T>`; the value slot holds `T`.
    TypeAST* innerTy = nullptr;
    if (binding->type && binding->type->isa<FutureTypeAST>()) {
        innerTy = binding->type->as<FutureTypeAST>()->inner;
    }
    if (!innerTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->loc,
            "async binding has no `Future<T>` type");
        return;
    }

    llvm::Type* innerLlvmTy = program.types().get(innerTy);
    if (!innerLlvmTy) return;

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    // ─── Handle slot and value slot ───────────────────────────────────────
    std::string name = program.pool.lookup(binding->name);
    llvm::AllocaInst* handleSlot = createEntryAlloca(
        llvm::PointerType::get(ctx, 0), name + ".handle");
    llvm::AllocaInst* valueSlot = createEntryAlloca(
        innerLlvmTy, name + ".value");
    if (!handleSlot || !valueSlot) return;

    // Register the value slot as the binding's storage.
    func().storeValue(binding, valueSlot);

    // Record the handle slot in the side table.
    func().storeHandle(binding, handleSlot);

    // ─── Thunk and packet ─────────────────────────────────────────────────
    llvm::Function* thunk = buildConcurrencyThunk(call, innerTy);
    if (!thunk) return;

    llvm::Value* packet = buildConcurrencyPacket(call);
    if (!packet) return;

    // ─── Call __lucid_async ───────────────────────────────────────────────
    llvm::Value* thunkPtr = b.CreatePointerCast(
        thunk, llvm::PointerType::get(ctx, 0), "thunk.ptr");
    program.abi().Async(b, thunkPtr, packet, handleSlot);

    // ─── Mark alive ───────────────────────────────────────────────────────
    // The binding holds a handle until await consumes it. Sema guarantees
    // the await happens before scope exit; the mark is a formality
    // because Handle drops are no-ops.
    func().markAlive(binding);

    Trace::detail("Lowered async statement: ", name);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitAwaitStmt — `await x`
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Steps ────────────────────────────────────────────────────────────────
//   1. Look up the handle slot (from the side table) and the value slot
//      (from the value map).
//   2. Call `__lucid_await(handleSlot)` → box pointer.
//   3. Load `T` from the box.
//   4. Free the box.
//   5. Store `T` into the value slot.
//   6. Mark the binding consumed.

void Emitter::emitAwaitStmt(AwaitStmtAST* stmt) {
    assert(stmt && "emitAwaitStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_AwaitNonAsync, target->loc,
                "await target must be an identifier");
            continue;
        }
        IdentifierExprAST* id = target->as<IdentifierExprAST>();
        ValueDeclAST* decl = id->resolvedDecl;
        if (!decl) continue;

        // ─── Handle slot ──────────────────────────────────────────────────
        llvm::Value* handleSlot = func().lookupHandle(decl);
        if (!handleSlot) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, target->loc,
                "await target '", program.pool.lookup(id->name),
                "' has no handle slot — Sema should have matched it to an "
                "async binding");
            continue;
        }

        // ─── Value slot ───────────────────────────────────────────────────
        llvm::Value* valueSlot = func().lookupValue(decl);
        if (!valueSlot) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, target->loc,
                "await target '", program.pool.lookup(id->name),
                "' has no value slot");
            continue;
        }

        // ─── Call __lucid_await ───────────────────────────────────────────
        llvm::Value* box = program.abi().Await(b, handleSlot);
        if (!box) continue;

        // ─── Load T from the box ──────────────────────────────────────────
        TypeAST* innerTy = nullptr;
        if (decl->type && decl->type->isa<FutureTypeAST>()) {
            innerTy = decl->type->as<FutureTypeAST>()->inner;
        }
        if (!innerTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, target->loc,
                "await target has no `Future<T>` type");
            continue;
        }
        llvm::Type* innerLlvmTy = program.types().get(innerTy);
        if (!innerLlvmTy) continue;

        // The box is a `void*` pointing at a heap slot holding `T`.
        llvm::Value* typedBox = b.CreatePointerCast(
            box, llvm::PointerType::get(ctx, 0), "await.box");
        llvm::Value* awaited = b.CreateLoad(
            innerLlvmTy, typedBox, "await.value");

        // ─── Free the box ─────────────────────────────────────────────────
        program.abi().Free(b, box);

        // ─── Store T into the value slot ──────────────────────────────────
        b.CreateStore(awaited, valueSlot);

        // ─── Mark consumed ────────────────────────────────────────────────
        func().markConsumed(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitSpawnStmt — `spawn const x T = f(args)` or `spawn _ = f(args)`
// ─────────────────────────────────────────────────────────────────────────────
//
// Symmetric to `emitAsyncStmt`, with two differences:
//   - The runtime call is `__lucid_spawn`.
//   - The discard pattern (`spawn _ = f()`) has no binding and passes a
//     null `out` slot.

void Emitter::emitSpawnStmt(SpawnStmtAST* stmt) {
    assert(stmt && "emitSpawnStmt() with null statement");

    if (!stmt->call || !stmt->call->isa<CallExprAST>()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->loc,
            "spawn statement requires a call expression");
        return;
    }
    CallExprAST* call = stmt->call->as<CallExprAST>();

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    // ─── Determine the return type for the thunk's box ────────────────────
    // For a named binding, the inner type is `Thread<T>`'s inner. For the
    // discard pattern, the emitter uses the call's return type — which
    // is the same `T`.
    TypeAST* innerTy = nullptr;
    if (stmt->binding && stmt->binding->type
        && stmt->binding->type->isa<ThreadTypeAST>()) {
        innerTy = stmt->binding->type->as<ThreadTypeAST>()->inner;
    } else {
        // Discard pattern: get `T` from the call's resolved type.
        if (call->resolvedType
            && call->resolvedType->isa<FuncTypeAST>()) {
            innerTy = call->resolvedType->as<FuncTypeAST>()->returnType;
        }
    }

    // ─── Thunk and packet ─────────────────────────────────────────────────
    // The thunk boxes the result. If there's no result type (a void
    // function), the thunk allocates a dummy byte.
    llvm::Function* thunk = buildConcurrencyThunk(call, innerTy);
    if (!thunk) return;

    llvm::Value* packet = buildConcurrencyPacket(call);
    if (!packet) return;

    llvm::Value* thunkPtr = b.CreatePointerCast(
        thunk, llvm::PointerType::get(ctx, 0), "thunk.ptr");

    // ─── Discard pattern ──────────────────────────────────────────────────
    if (!stmt->binding) {
        llvm::Value* nullSlot = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(ctx, 0));
        program.abi().Spawn(b, thunkPtr, packet, nullSlot);
        Trace::detail("Lowered spawn discard statement");
        return;
    }

    // ─── Named binding ────────────────────────────────────────────────────
    if (!innerTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, stmt->loc,
            "spawn binding has no inner type");
        return;
    }
    llvm::Type* innerLlvmTy = program.types().get(innerTy);
    if (!innerLlvmTy) return;

    std::string name = program.pool.lookup(stmt->binding->name);
    llvm::AllocaInst* handleSlot = createEntryAlloca(
        llvm::PointerType::get(ctx, 0), name + ".thread_handle");
    llvm::AllocaInst* valueSlot = createEntryAlloca(
        innerLlvmTy, name + ".value");
    if (!handleSlot || !valueSlot) return;

    func().storeValue(stmt->binding, valueSlot);
    func().storeHandle(stmt->binding, handleSlot);

    program.abi().Spawn(b, thunkPtr, packet, handleSlot);

    func().markAlive(stmt->binding);
    Trace::detail("Lowered spawn statement: ", name);
}

// ─────────────────────────────────────────────────────────────────────────────
// emitJoinStmt — `join x`
// ─────────────────────────────────────────────────────────────────────────────
//
// Symmetric to `emitAwaitStmt` but calls `__lucid_join`.

void Emitter::emitJoinStmt(JoinStmtAST* stmt) {
    assert(stmt && "emitJoinStmt() with null statement");

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_JoinNonSpawn, target->loc,
                "join target must be an identifier");
            continue;
        }
        IdentifierExprAST* id = target->as<IdentifierExprAST>();
        ValueDeclAST* decl = id->resolvedDecl;
        if (!decl) continue;

        llvm::Value* handleSlot = func().lookupHandle(decl);
        if (!handleSlot) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, target->loc,
                "join target '", program.pool.lookup(id->name),
                "' has no handle slot");
            continue;
        }
        llvm::Value* valueSlot = func().lookupValue(decl);
        if (!valueSlot) continue;

        llvm::Value* box = program.abi().Join(b, handleSlot);
        if (!box) continue;

        TypeAST* innerTy = nullptr;
        if (decl->type && decl->type->isa<ThreadTypeAST>()) {
            innerTy = decl->type->as<ThreadTypeAST>()->inner;
        }
        if (!innerTy) continue;
        llvm::Type* innerLlvmTy = program.types().get(innerTy);
        if (!innerLlvmTy) continue;

        llvm::Value* typedBox = b.CreatePointerCast(
            box, llvm::PointerType::get(ctx, 0), "join.box");
        llvm::Value* joined = b.CreateLoad(
            innerLlvmTy, typedBox, "join.value");
        program.abi().Free(b, box);

        b.CreateStore(joined, valueSlot);

        func().markConsumed(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// buildConcurrencyThunk — the synthesized thunk function
// ─────────────────────────────────────────────────────────────────────────────
//
// Signature: `void* thunk(void* packet)`.
//
// Body:
//   1. Unpack: GEP to each packet field, load the argument.
//   2. Free the packet: `__lucid_free(packet)`.
//   3. Call the target function with the unpacked arguments.
//   4. Box the result:
//      - For a non-void return: allocate a slot via `__lucid_alloc`,
//        store the result into it, return the slot pointer.
//      - For a void return: allocate a 1-byte dummy, return its pointer.

llvm::Function* Emitter::buildConcurrencyThunk(CallExprAST* call,
                                                TypeAST* returnTy) {
    if (!call) return nullptr;

    // ─── Callee's function type ───────────────────────────────────────────
    FuncTypeAST* calleeFnTy = call->callee->resolvedType
        ? (call->callee->resolvedType->isa<FuncTypeAST>()
              ? call->callee->resolvedType->as<FuncTypeAST>()
              : nullptr)
        : nullptr;
    if (!calleeFnTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, call->loc,
            "concurrency thunk: callee is not a function type");
        return nullptr;
    }

    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

    // ─── Create the function ──────────────────────────────────────────────
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        ptrTy, {ptrTy}, /*isVarArg=*/false);
    std::string name = "concurrency.thunk." + std::to_string(nextThunkId());
    llvm::Function* thunk = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, program.module());
    thunk->getArg(0)->setName("packet");

    // ─── Save the caller's insertion point ────────────────────────────────
    // The thunk is emitted at module scope while the enclosing function's
    // lowering is in progress.
    llvm::IRBuilderBase::InsertPointGuard guard(program.builder());
    llvm::IRBuilder<>& b = program.builder();

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", thunk);
    b.SetInsertPoint(entry);

    // ─── Packet struct type ───────────────────────────────────────────────
    // The packet is a struct with one field per argument. Its LLVM type
    // is built by `buildConcurrencyPacket`, but that emitter doesn't
    // return the type; the thunk reconstructs it from the argument count
    // and parameter types. The two must match.
    //
    // For simplicity, this implementation only handles scalar argument
    // types (integers, floats, pointers). Aggregate arguments (structs,
    // slices, strings) are passed by pointer in the packet, and the
    // thunk materializes them by loading from the packet's pointer field.
    std::vector<llvm::Type*> packetFieldTys;
    packetFieldTys.reserve(calleeFnTy->params.size());
    for (ParamAST* param : calleeFnTy->params) {
        llvm::Type* paramTy = program.types().get(param->type);
        if (!paramTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, call->loc,
                "concurrency thunk: parameter type is unresolvable");
            return nullptr;
        }
        // Aggregates are stored by pointer in the packet.
        if (paramTy->isStructTy() || paramTy->isArrayTy()) {
            packetFieldTys.push_back(ptrTy);
        } else {
            packetFieldTys.push_back(paramTy);
        }
    }
    llvm::StructType* packetTy = llvm::StructType::get(
        ctx, packetFieldTys);

    // ─── Unpack the packet ────────────────────────────────────────────────
    llvm::Value* packetPtr = b.CreatePointerCast(
        thunk->getArg(0), ptrTy, "packet.ptr");

    std::vector<llvm::Value*> args;
    args.reserve(calleeFnTy->params.size());

    for (size_t i = 0; i < calleeFnTy->params.size(); ++i) {
        ParamAST* param = calleeFnTy->params[i];
        llvm::Type* paramTy = program.types().get(param->type);
        if (!paramTy) continue;

        llvm::Value* fieldPtr = b.CreateStructGEP(
            packetTy, packetPtr, static_cast<unsigned>(i),
            "packet.field." + std::to_string(i));
        llvm::Value* loaded = b.CreateLoad(
            packetFieldTys[i], fieldPtr,
            "packet.arg." + std::to_string(i));

        // For aggregate parameters, `loaded` is a pointer to the
        // aggregate in the packet. The call takes the aggregate value
        // by pointer (matching the materialize-argument convention);
        // pass the pointer directly.
        args.push_back(loaded);
    }

    // ─── Free the packet ──────────────────────────────────────────────────
    program.abi().Free(b, packetPtr);

    // ─── Call the target ──────────────────────────────────────────────────
    llvm::Value* callee = program.emitter().emit(call->callee).v;
    if (!callee) {
        // No callee — emit an unreachable thunk. The runtime treats a
        // null return as a failure.
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        b.CreateRet(nullPtr);
        return thunk;
    }

    llvm::FunctionType* callFnTy = program.types().functionType(
        calleeFnTy, /*isClosure=*/false);
    if (!callFnTy) {
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        b.CreateRet(nullPtr);
        return thunk;
    }

    llvm::Value* result = nullptr;
    if (calleeFnTy->shape == FuncShape::Fn) {
        result = b.CreateCall(callFnTy, callee, args, "thunk.call");
    } else {
        // `cls`-shaped callees are not supported in async/spawn yet.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, call->loc,
            "closures in async/spawn expressions are not yet implemented");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        b.CreateRet(nullPtr);
        return thunk;
    }

    // ─── Box the result ───────────────────────────────────────────────────
    if (returnTy) {
        llvm::Type* returnLlvmTy = program.types().get(returnTy);
        if (!returnLlvmTy) returnLlvmTy = llvm::Type::getInt8Ty(ctx);

        uint64_t boxSize =
            program.module().getDataLayout()
                .getTypeAllocSize(returnLlvmTy).getFixedValue();
        if (boxSize == 0) boxSize = 1;

        llvm::Value* box = program.abi().Alloc(
            b, llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx), boxSize));
        if (!box) {
            llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            b.CreateRet(nullPtr);
            return thunk;
        }

        llvm::Value* typedBox = b.CreatePointerCast(box, ptrTy, "box.typed");
        b.CreateStore(result, typedBox);
        b.CreateRet(box);
    } else {
        // Void: allocate a 1-byte dummy box.
        llvm::Value* box = program.abi().Alloc(
            b, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 1));
        b.CreateRet(box);
    }

    return thunk;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildConcurrencyPacket — the argument packet
// ─────────────────────────────────────────────────────────────────────────────
//
// Builds a heap-allocated struct with one field per argument. Each
// argument is emitted, coerced to its parameter type, `intoOwned`-ed
// (the packet becomes the owner), and stored into its field.
//
// The packet's LLVM type is built the same way as in
// `buildConcurrencyThunk`; the two must agree.

llvm::Value* Emitter::buildConcurrencyPacket(CallExprAST* call) {
    if (!call) return nullptr;

    FuncTypeAST* calleeFnTy = call->callee->resolvedType
        ? (call->callee->resolvedType->isa<FuncTypeAST>()
              ? call->callee->resolvedType->as<FuncTypeAST>()
              : nullptr)
        : nullptr;
    if (!calleeFnTy) return nullptr;

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

    // ─── Build the packet struct type ─────────────────────────────────────
    std::vector<llvm::Type*> fieldTys;
    fieldTys.reserve(call->args.size());
    for (size_t i = 0; i < call->args.size(); ++i) {
        TypeAST* paramTy = (i < calleeFnTy->params.size())
            ? calleeFnTy->params[i]->type
            : nullptr;
        if (!paramTy) {
            fieldTys.push_back(ptrTy);
            continue;
        }
        llvm::Type* paramLlvmTy = program.types().get(paramTy);
        if (!paramLlvmTy) {
            fieldTys.push_back(ptrTy);
            continue;
        }
        // Aggregates are stored by pointer.
        if (paramLlvmTy->isStructTy() || paramLlvmTy->isArrayTy()) {
            fieldTys.push_back(ptrTy);
        } else {
            fieldTys.push_back(paramLlvmTy);
        }
    }
    llvm::StructType* packetTy = llvm::StructType::get(ctx, fieldTys);

    // ─── Allocate the packet ──────────────────────────────────────────────
    uint64_t packetSize = program.module().getDataLayout()
        .getTypeAllocSize(packetTy).getFixedValue();
    if (packetSize == 0) packetSize = 1;

    llvm::Value* packet = program.abi().Alloc(
        b, llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx), packetSize));
    if (!packet) return nullptr;

    // ─── Store each argument ──────────────────────────────────────────────
    for (size_t i = 0; i < call->args.size(); ++i) {
        ExprAST* argExpr = call->args[i];
        Val argVal = emit(argExpr);
        if (!argVal.isValid()) return nullptr;

        TypeAST* paramTy = (i < calleeFnTy->params.size())
            ? calleeFnTy->params[i]->type
            : nullptr;
        if (paramTy) {
            argVal = coerceArgument(argVal, paramTy);
            if (!argVal.isValid()) return nullptr;
        }

        // Materialize (spill if aggregate). But the packet stores the
        // pointer for aggregates, so materialize returns the pointer,
        // which is what we store.
        llvm::Value* materialized = materializeArgument(argVal);
        if (!materialized) return nullptr;

        Val matVal{materialized, argVal.ty, argVal.own};
        Val owned = program.ownership().intoOwned(matVal, b);
        if (!owned.isValid()) return nullptr;

        llvm::Value* fieldPtr = b.CreateStructGEP(
            packetTy, packet, static_cast<unsigned>(i),
            "packet.store." + std::to_string(i));

        // Coerce to the field type.
        llvm::Value* stored = owned.v;
        llvm::Type* fieldTy = fieldTys[i];
        if (stored->getType() != fieldTy) {
            llvm::Value* coerced = coerceValueToType(stored, fieldTy, b);
            if (coerced) stored = coerced;
        }

        b.CreateStore(stored, fieldPtr);
    }

    return packet;
}

} // namespace codegen