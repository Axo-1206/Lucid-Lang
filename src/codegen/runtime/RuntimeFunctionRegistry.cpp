/// @file runtime/RuntimeFunctionRegistry.cpp
/// @brief Implementation of the runtime function registry.

#include "RuntimeFunctionRegistry.hpp"
#include "../context/CodeGenContext.hpp"
#include "../types/LLVMTypeHelpers.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cassert>
#include <unordered_map>

namespace codegen {

// ─── Registry Table ─────────────────────────────────────────────────────

const std::unordered_map<RuntimeFn, RuntimeFunctionInfo>& runtimeFunctionTable() {
    static const std::unordered_map<RuntimeFn, RuntimeFunctionInfo> table = {
        // ─── Closures ───────────────────────────────────────────────────────
        { RuntimeFn::AllocEnv, { "__lucid_alloc_env",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::RetainEnv, { "__lucid_retain_env",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ReleaseEnv, { "__lucid_release_env",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        // ─── Memory Management ──────────────────────────────────────────────
        { RuntimeFn::Alloc, { "__lucid_alloc",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::Free, { "__lucid_free",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        // ─── Arena ──────────────────────────────────────────────────────────
        { RuntimeFn::ArenaCreate, { "__lucid_arena_create",
            [](CodeGenContext& ctx) {
                // void __lucid_arena_create(ArenaDescriptor* out, uint64_t size)
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx)},
                    false);
            } } },

        { RuntimeFn::ArenaAlloc, { "__lucid_arena_alloc",
            [](CodeGenContext& ctx) {
                // void* __lucid_arena_alloc(Arena* arena, uint64_t size, uint64_t alignment)
                llvm::Type* arenaPtr = getPtrType(ctx.llvmCtx);
                llvm::Type* i64 = getI64Type(ctx.llvmCtx);
                return llvm::FunctionType::get(
                    arenaPtr, {arenaPtr, i64, i64}, false);
            } } },

        { RuntimeFn::ArenaReset, { "__lucid_arena_reset",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaFree, { "__lucid_arena_free",
            [](CodeGenContext& ctx) {
                // void __lucid_arena_free(Arena* arena)
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaCapacity, { "__lucid_arena_capacity",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getI64Type(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaRemaining, { "__lucid_arena_remaining",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getI64Type(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaIsEmpty, { "__lucid_arena_is_empty",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getI1Type(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaSpace, { "__lucid_arena_space",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getI64Type(ctx.llvmCtx), {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaCanFit, { "__lucid_arena_can_fit",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getI1Type(ctx.llvmCtx), 
                    {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx), getI64Type(ctx.llvmCtx)}, 
                    false);
            } } },

        // ─── Strings ────────────────────────────────────────────────────────
        { RuntimeFn::StrConcat, { "__lucid_str_concat",
            [](CodeGenContext& ctx) {
                llvm::StructType* str = ctx.getStringType();
                return llvm::FunctionType::get(str, {str, str}, false);
            } } },

        { RuntimeFn::StrSlice, { "__lucid_str_slice",
            [](CodeGenContext& ctx) {
                llvm::StructType* str = ctx.getStringType();
                return llvm::FunctionType::get(
                    str, {str, getI64Type(ctx.llvmCtx), getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::StrEq, { "__lucid_str_eq",
            [](CodeGenContext& ctx) {
                llvm::StructType* str = ctx.getStringType();
                return llvm::FunctionType::get(getI1Type(ctx.llvmCtx), {str, str}, false);
            } } },

        // ─── #tostr / #ptrstr formatters ────────────────────────────────────
        { RuntimeFn::PtrToHexString, { "__lucid_ptr_to_hex_string",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::BoolToStr, { "__lucid_bool_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getI1Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::CharToStr, { "__lucid_char_to_str",
            [](CodeGenContext& ctx) {
                // string __lucid_char_to_str(int32 codepoint)
                // Note: Lucid `char` is an 8-bit byte at AST level; CodeGen zero-extends
                // it to i32 for Unicode codepoint compatibility when calling this runtime function.
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getI32Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::IntToStr, { "__lucid_int_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::UintToStr, { "__lucid_uint_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::FloatToStr, { "__lucid_float_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    ctx.getStringType(), {getDoubleType(ctx.llvmCtx)}, false);
            } } },

        // ─── Panics ─────────────────────────────────────────────────────────
        { RuntimeFn::Panic, { "__lucid_panic",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        // ─── Concurrency ────────────────────────────────────────────────────
        { RuntimeFn::Async, { "__lucid_async",
            [](CodeGenContext& ctx) {
                // void* __lucid_async(void* callable, void* args, void* future_handle)
                // Returns a FutureHandle* (opaque pointer)
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx), getPtrType(ctx.llvmCtx), getPtrType(ctx.llvmCtx)},
                    false);
            } } },

        { RuntimeFn::Await, { "__lucid_await",
            [](CodeGenContext& ctx) {
                // void* __lucid_await(void* future_handle_ptr)
                // Blocks the current thread until the future is ready and returns result.
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx)},
                    false);
            } } },

        { RuntimeFn::Spawn, { "__lucid_spawn",
            [](CodeGenContext& ctx) {
                // void* __lucid_spawn(void* callable, void* args, void* thread_handle)
                // Returns a ThreadHandle* (opaque pointer)
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx), getPtrType(ctx.llvmCtx), getPtrType(ctx.llvmCtx)},
                    false);
            } } },

        { RuntimeFn::Join, { "__lucid_join",
            [](CodeGenContext& ctx) {
                // void* __lucid_join(void* thread_handle_ptr)
                // Blocks the current thread until the thread completes and returns result.
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx)},
                    false);
            } } },

        // ─── System ──────────────────────────────────────────────────────────
        { RuntimeFn::Shutdown, { "__lucid_shutdown",
            [](CodeGenContext& ctx) {
                // void __lucid_shutdown()
                // Signals all threads to stop, waits for them to finish,
                // and cleans up all pending futures/threads.
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx),
                    {},
                    false);
            } } },
    };
    return table;
}

const RuntimeFunctionInfo& getRuntimeFunctionInfo(RuntimeFn fn) {
    const auto& table = runtimeFunctionTable();
    auto it = table.find(fn);
    assert(it != table.end() && "RuntimeFn enumerator has no registry entry - "
                                 "add a row in runtimeFunctionTable()");
    return it->second;
}

} // namespace codegen