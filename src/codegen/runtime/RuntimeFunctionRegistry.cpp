/// @file runtime/RuntimeFunctionRegistry.cpp
/// @brief Implementation of the runtime function registry.
///
/// This is the ONLY file in CodeGen where a "__lucid_*" symbol name is
/// spelled as a string literal. Every call site elsewhere goes through
/// CodeGenContext::getRuntimeFn(RuntimeFn), which looks the entry up here.

#include "RuntimeFunctionRegistry.hpp"
#include "../context/CodeGenContext.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cassert>
#include <unordered_map>

namespace codegen {

namespace {

llvm::Type* i8Ptr(CodeGenContext& ctx) {
    return llvm::PointerType::get(ctx.llvmCtx, 0);
}
llvm::Type* i64(CodeGenContext& ctx) {
    return llvm::Type::getInt64Ty(ctx.llvmCtx);
}
llvm::Type* i32(CodeGenContext& ctx) {
    return llvm::Type::getInt32Ty(ctx.llvmCtx);
}
llvm::Type* i1(CodeGenContext& ctx) {
    return llvm::Type::getInt1Ty(ctx.llvmCtx);
}
llvm::Type* f64(CodeGenContext& ctx) {
    return llvm::Type::getDoubleTy(ctx.llvmCtx);
}
llvm::Type* voidTy(CodeGenContext& ctx) {
    return llvm::Type::getVoidTy(ctx.llvmCtx);
}
llvm::Type* strTy(CodeGenContext& ctx) {
    return ctx.getStringType();
}
// Matches the anonymous { void*, uint64_t } ArenaDescriptor built inline
// at the #arena_create call site (LucidIntrinsicEmitter.cpp) - kept here
// too so the registry's declared return type actually matches what the
// call site was already building, rather than guessing a new shape.
llvm::Type* arenaTy(CodeGenContext& ctx) {
    return llvm::StructType::get(ctx.llvmCtx, {i8Ptr(ctx), i64(ctx)});
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
                return llvm::FunctionType::get(i8Ptr(ctx), {i64(ctx)}, false);
            } } },

        { RuntimeFn::Alloc, { "__lucid_alloc",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(i8Ptr(ctx), {i64(ctx)}, false);
            } } },

        { RuntimeFn::Free, { "__lucid_free",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(voidTy(ctx), {i8Ptr(ctx)}, false);
            } } },

        { RuntimeFn::ArenaCreate, { "__lucid_arena_create",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(arenaTy(ctx), {i64(ctx)}, false);
            } } },

        { RuntimeFn::ArenaAlloc, { "__lucid_arena_alloc",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(i8Ptr(ctx), {i8Ptr(ctx), i64(ctx)}, false);
            } } },

        { RuntimeFn::ArenaReset, { "__lucid_arena_reset",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(voidTy(ctx), {i8Ptr(ctx)}, false);
            } } },

        { RuntimeFn::ArenaFree, { "__lucid_arena_free",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(voidTy(ctx), {i8Ptr(ctx)}, false);
            } } },

        { RuntimeFn::StrConcat, { "__lucid_str_concat",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {strTy(ctx), strTy(ctx)}, false);
            } } },

        { RuntimeFn::StrSlice, { "__lucid_str_slice",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(
                    strTy(ctx), {strTy(ctx), i64(ctx), i64(ctx)}, false);
            } } },

        { RuntimeFn::StrEq, { "__lucid_str_eq",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(i1(ctx), {strTy(ctx), strTy(ctx)}, false);
            } } },

        { RuntimeFn::PtrToHexString, { "__lucid_ptr_to_hex_string",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {i8Ptr(ctx)}, false);
            } } },

        { RuntimeFn::BoolToStr, { "__lucid_bool_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {i1(ctx)}, false);
            } } },

        { RuntimeFn::CharToStr, { "__lucid_char_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {i32(ctx)}, false);
            } } },

        { RuntimeFn::IntToStr, { "__lucid_int_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {i64(ctx)}, false);
            } } },

        { RuntimeFn::UintToStr, { "__lucid_uint_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {i64(ctx)}, false);
            } } },

        { RuntimeFn::FloatToStr, { "__lucid_float_to_str",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(strTy(ctx), {f64(ctx)}, false);
            } } },

        { RuntimeFn::Panic, { "__lucid_panic",
            [](CodeGenContext& ctx) {
                return llvm::FunctionType::get(voidTy(ctx), {i8Ptr(ctx)}, false);
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