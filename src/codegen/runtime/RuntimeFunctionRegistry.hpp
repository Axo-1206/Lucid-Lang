/// @file runtime/RuntimeFunctionRegistry.hpp
/// @brief Single source of truth for every `__lucid_*` runtime library
///        function CodeGen declares and calls.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────
/// Every place that needs to call into the Lucid runtime library used to
/// spell the symbol name as a raw string literal at the call site
/// ("__lucid_int_to_str", "__lucid_alloc_env", ...) and hand-build its own
/// llvm::FunctionType to match. That has two problems:
///   1. A typo in the string is a link-time failure, or worse, two call
///      sites silently declaring the "same" function under two different
///      names, each getting its own independent llvm::Function in the
///      module.
///   2. There is no single place to see what the runtime surface actually
///      is, so it's easy to add a call site that depends on a symbol that
///      doesn't exist in the runtime library at all.
///
/// This mirrors registry/IntrinsicRegistry.hpp's own justification almost
/// exactly, just for the runtime ABI surface instead of the intrinsic
/// surface: a small, dependency-light, pure-data table that every codegen
/// file consults instead of re-deriving the same information locally.
///
/// ─── What This File Does ──────────────────────────────────────────────────
/// ════════════════════════════════════════════════════════════════════════════
/// IMPORTANT: This file DECLARES runtime functions to LLVM. It does NOT
///            IMPLEMENT them. The actual C++ implementations live in:
///            `src/codegen/runtime/closure/ClosureRuntime.cpp`
///            `src/codegen/runtime/concurrency/ConcurrencyRuntime.cpp`
///            `src/codegen/runtime/memory/MemoryRuntime.cpp`
/// ════════════════════════════════════════════════════════════════════════════
///
/// ─── The Two Steps: Declaration vs Implementation ──────────────────────────
///
///   1. DECLARATION (This File + .cpp)
///      ┌─────────────────────────────────────────────────────────────────────┐
///      │  Purpose: Tell LLVM "there exists a function with this name and     │
///      │           signature" so CodeGen can generate call instructions.     │
///      └─────────────────────────────────────────────────────────────────────┘
///
///   2. IMPLEMENTATION (src/runtime/memory/MemoryRuntime.cpp and
///      src/runtime/concurrency/ConcurrencyRuntime.cpp)
///      ┌─────────────────────────────────────────────────────────────────────┐
///      │  Purpose: The actual C++ code that runs when the function is        │
///      │           called at runtime.                                        │
///      └─────────────────────────────────────────────────────────────────────┘
///
/// ─── Usage ──────────────────────────────────────────────────────────────
/// @code
/// llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::IntToStr);
/// ctx.builder.CreateCall(fn, {intVal});
/// @endcode
///
/// Call sites never spell a "__lucid_*" name again - CodeGenContext::
/// getRuntimeFn(RuntimeFn) is the only thing that touches this registry,
/// and RuntimeFunctionRegistry.cpp is the only file where the string names
/// are written down at all.

#pragma once

#include <functional>
#include <string_view>

namespace llvm {
class FunctionType;
} // namespace llvm

namespace codegen {

struct CodeGenContext;

/// @brief Enumerates every runtime library function CodeGen can call.
/// One entry per distinct `__lucid_*` symbol - never call a runtime
/// function by a raw string anywhere else in CodeGen.
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// Adding a new enumerator here is only HALF the work. You must also:
///   1. Add a table row in RuntimeFunctionRegistry.cpp with the signature
///   2. Implement the function in src/runtime/ (C++)
///   3. Add calls to the function in CodeGen/*.cpp
///
/// Without step 2, the linker will fail with "undefined symbol".
///
/// ─── Concurrency ABI ──────────────────────────────────────────────────────────
/// The concurrency functions use a "handle storage" pattern:
///
///   CodeGen creates an alloca: %handle = alloca i8*
///   CodeGen passes the alloca to: __lucid_async(callable, args, %handle)
///   Runtime writes the handle into: *((void**)%handle) = actual_handle
///   CodeGen later reads: %handle_val = load i8*, i8** %handle
///   CodeGen passes to: __lucid_await(%handle_val)
///
/// This is why the third parameter is `void* future_handle_ptr` (a pointer
/// to the storage location where the handle should be written), NOT the
/// handle itself. The function returns the handle as the return value
/// for convenience, but the primary mechanism is the pointer parameter.
///
/// @see CodeGenStmt.cpp - lowerAsyncStmt(), lowerAwaitStmt()
/// @see CodeGenStmt.cpp - lowerSpawnStmt(), lowerJoinStmt()
enum class RuntimeFn {
    // ─── Closures ───────────────────────────────────────────────────────
    AllocEnv,           // void* __lucid_alloc_env(uint64_t size)
    IsClosure,          // bool __lucid_is_closure(void* value)
    RetainEnv,          // void __lucid_retain_env(void* env)
    ReleaseEnv,         // void __lucid_release_env(void* env)

    // ─── Memory Management ──────────────────────────────────────────────
    Alloc,              // void* __lucid_alloc(uint64_t size)
    Free,               // void __lucid_free(void* ptr)
    
    // ─── Arena ──────────────────────────────────────────────────────────
    ArenaCreate,        // ArenaDescriptor __lucid_arena_create(uint64_t size)
    ArenaAlloc,         // void* __lucid_arena_alloc(Arena* arena, uint64_t size, uint64_t alignment)
    ArenaReset,         // void __lucid_arena_reset(Arena* arena)
    ArenaCapacity,      // uint64_t __lucid_arena_capacity(const Arena* arena)
    ArenaRemaining,     // uint64_t __lucid_arena_remaining(const Arena* arena)
    ArenaIsEmpty,       // bool __lucid_arena_is_empty(const Arena* arena)
    ArenaSpace,         // uint64_t __lucid_arena_space(const Arena* arena, uint64_t elem_size)
    ArenaCanFit,        // bool __lucid_arena_can_fit(const Arena* arena, uint64_t elem_size, uint64_t count)

    // ─── Strings ────────────────────────────────────────────────────────
    StrConcat,          // string __lucid_str_concat(string a, string b)
    StrSlice,           // string __lucid_str_slice(string s, int64 from, int64 to)
    StrEq,              // bool __lucid_str_eq(string a, string b)

    // ─── #tostr / #ptrstr formatters ────────────────────────────────────
    PtrToHexString,     // string __lucid_ptr_to_hex_string(void* ptr)
    BoolToStr,          // string __lucid_bool_to_str(bool b)
    CharToStr,          // string __lucid_char_to_str(int32 codepoint)
    IntToStr,           // string __lucid_int_to_str(int64 v)
    UintToStr,          // string __lucid_uint_to_str(int64 v) (bit pattern, formatted unsigned)
    FloatToStr,         // string __lucid_float_to_str(double v)

    // ─── Panics ─────────────────────────────────────────────────────────
    Panic,              // void __lucid_panic(char* message)

    /// ─── Concurrency ──────────────────────────────────────────────────────
    /// ─── IMPORTANT: Handle Storage Pattern ──────────────────────────────
    // 
    // Async/Spawn functions follow this pattern:
    //   1. CodeGen allocates storage: alloca i8*
    //   2. CodeGen passes the storage pointer as the 3rd argument
    //   3. Runtime writes the handle into the storage: *(void**)arg3 = handle
    //   4. Runtime returns the handle as the function result
    //   5. CodeGen loads the handle from storage for await/join
    //
    // This is why the 3rd parameter is a pointer-to-pointer, not a pointer.
    //
    // ─── Example IR ──────────────────────────────────────────────────────
    //   %handle_storage = alloca i8*
    //   %result = call i8* @__lucid_async(i8* %callable, i8* %args, i8* %handle_storage)
    //   ; Runtime writes to %handle_storage
    //   %handle = load i8*, i8** %handle_storage
    //   call void @__lucid_await(i8* %handle)
    //
    // @see ConcurrencyRuntime.cpp - __lucid_async()
    // @see ConcurrencyRuntime.cpp - __lucid_spawn()
    Async,              // void* __lucid_async(void* callable, void* args, void* future_handle_ptr)
                        //   - callable: function to execute (i8*)
                        //   - args: arguments for the callable (i8*)
                        //   - future_handle_ptr: pointer to storage where handle is written (void**)
                        //   - returns: the FutureHandle* (same as stored)
                        // @note: future_handle_ptr is a void**, NOT a void*
                        // @note: CodeGen must pass an alloca i8* as the 3rd argument
    
    Await,              // void __lucid_await(void* future_handle)
                        //   - future_handle: the FutureHandle* to wait for (i8*)
                        //   - blocks until the future is ready
                        //   - consumes the future (linear type)
    
    Spawn,              // void* __lucid_spawn(void* callable, void* args, void* thread_handle_ptr)
                        //   - callable: function to execute (i8*)
                        //   - args: arguments for the callable (i8*)
                        //   - thread_handle_ptr: pointer to storage where handle is written (void**)
                        //   - returns: the ThreadHandle* (same as stored)
                        // @note: thread_handle_ptr is a void**, NOT a void*
                        // @note: CodeGen must pass an alloca i8* as the 3rd argument
    
    Join,               // void __lucid_join(void* thread_handle)
                        //   - thread_handle: the ThreadHandle* to wait for (i8*)
                        //   - blocks until the thread completes
                        //   - consumes the thread (linear type)
    
    Shutdown,           // void __lucid_shutdown()
                        //   - signals all threads to stop
                        //   - waits for all threads to finish
                        //   - cleans up all pending futures/threads
                        //   - called automatically when main returns
};

/// @brief One registry entry: the linker symbol name, plus a builder that
/// constructs the matching llvm::FunctionType on demand (deferred, since
/// building a FunctionType needs a live CodeGenContext for ctx.llvmCtx /
/// ctx.getStringType() / etc., which isn't available at static-init time).
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// This struct only DECLARES the function to LLVM. The actual implementation
/// must be provided separately in src/runtime/.
struct RuntimeFunctionInfo {
    std::string_view name;
    std::function<llvm::FunctionType*(CodeGenContext&)> buildType;
};

/// @brief Look up a runtime function's registry entry.
/// @param fn The runtime function to look up.
/// @return The registry entry. Always valid for a well-formed RuntimeFn -
///         every enumerator has a corresponding table row, checked by
///         static_assert in the .cpp against the enum's own size.
const RuntimeFunctionInfo& getRuntimeFunctionInfo(RuntimeFn fn);

} // namespace codegen