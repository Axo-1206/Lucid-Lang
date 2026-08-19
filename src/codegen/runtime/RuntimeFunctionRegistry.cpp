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

// Only arenaTy lives here - every generic type constant (i8Ptr, i64, i32,
// i1, f64, void, string) already exists in LLVMHelpers.hpp
// (getPtrType/getI64Type/getI32Type/getI1Type/getDoubleType/getVoidType/
// getStringType) and is used directly below instead of being re-wrapped.
// arenaTy doesn't belong there - it's the ArenaDescriptor ABI shape, not a
// generic LLVM type constant, so LLVMHelpers.hpp (deliberately
// domain-agnostic, per its own file header) is the wrong home for it.

// Matches the anonymous { void*, uint64_t } ArenaDescriptor built inline
// at the #arena_create call site (LucidIntrinsicEmitter.cpp) - kept here
// too so the registry's declared return type actually matches what the
// call site was already building, rather than guessing a new shape.
llvm::Type* arenaTy(CodeGenContext& ctx) {
    return llvm::StructType::get(ctx.llvmCtx, {getPtrType(ctx.llvmCtx), getI64Type(ctx.llvmCtx)});
}

} // namespace

// ─── Registry Table ─────────────────────────────────────────────────────
//
// One row per RuntimeFn enumerator. Every buildType lambda is deferred
// (not evaluated until a live CodeGenContext exists), since llvm::Type
// factories need ctx.llvmCtx.
//
// NOTE on ArenaReset/ArenaFree: they're declared here exactly as found at
// their call sites - taking a bare `void*`, not the { void*, uint64_t }
// ArenaDescriptor struct ArenaCreate returns. That's a pre-existing
// mismatch between what ArenaCreate hands back and what ArenaReset/
// ArenaFree expect to receive, surfaced by having all three sit next to
// each other for the first time - worth checking against the actual
// runtime library signatures rather than assumed away here.
const std::unordered_map<RuntimeFn, RuntimeFunctionInfo>& runtimeFunctionTable() {
    static const std::unordered_map<RuntimeFn, RuntimeFunctionInfo> table = {
        { RuntimeFn::AllocEnv, { "__lucid_alloc_env",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    getPtrType(ctx.llvmCtx), {getI64Type(ctx.llvmCtx)}, false);
            } } },

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
    // A missing entry here means a RuntimeFn enumerator was added without
    // a matching table row - a programmer error in this file, not
    // something a caller can recover from, hence the hard assert rather
    // than a null-returning lookup callers would need to guard everywhere.
    assert(it != table.end() && "RuntimeFn enumerator has no registry entry - "
                                 "add a row in runtimeFunctionTable()");
    return it->second;
}

} // namespace codegen