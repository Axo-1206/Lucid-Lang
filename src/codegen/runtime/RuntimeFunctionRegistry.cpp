/// @file runtime/RuntimeFunctionRegistry.cpp
/// @brief Implementation of the runtime function registry.
///
/// This is the ONLY file in CodeGen where a "__lucid_*" symbol name is
/// spelled as a string literal. Every call site elsewhere goes through
/// CodeGenContext::getRuntimeFn(RuntimeFn), which looks the entry up here.

#include "RuntimeFunctionRegistry.hpp"
#include "../context/CodeGenContext.hpp"
#include "../support/LLVMHelpers.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cassert>
#include <unordered_map>

namespace codegen {

namespace {

// Matches the anonymous { void*, uint64_t } ArenaDescriptor built inline
// at the #arena_create call site (LucidIntrinsicEmitter.cpp).
llvm::Type* arenaTy(CodeGenContext& ctx) {
    return llvm::StructType::get(ctx.llvmCtx, {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx)});
}

} // namespace

// ─── Registry Table ─────────────────────────────────────────────────────

const std::unordered_map<RuntimeFn, RuntimeFunctionInfo>& runtimeFunctionTable() {
    static const std::unordered_map<RuntimeFn, RuntimeFunctionInfo> table = {
        // ─── Closures ───────────────────────────────────────────────────────
        { RuntimeFn::AllocEnv, { "__lucid_alloc_env",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::IsClosure, { "__lucid_is_closure",
            [](CodeGenContext& ctx) {
                // bool __lucid_is_closure(void* value)
                // Returns true if the value is a closure (has an environment).
                return llvm::FunctionType::get(
                    getI1Type(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::RetainEnv, { "__lucid_retain_env",
            [](CodeGenContext& ctx) {
                // void __lucid_retain_env(void* env)
                // Increments the reference count of a closure environment.
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ReleaseEnv, { "__lucid_release_env",
            [](CodeGenContext& ctx) {
                // void __lucid_release_env(void* env)
                // Decrements the reference count of a closure environment.
                // Frees the environment when the count reaches zero.
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

        { RuntimeFn::ArenaCreate, { "__lucid_arena_create",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(arenaTy(ctx), {getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaAlloc, { "__lucid_arena_alloc",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx),
                    {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaReset, { "__lucid_arena_reset",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
            } } },

        { RuntimeFn::ArenaFree, { "__lucid_arena_free",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getVoidType(ctx.llvmCtx), {getPtrType(ctx.llvmCtx)}, false);
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