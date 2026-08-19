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
///      doesn't exist in the runtime library at all (this happened this
///      session - #tostr/#ptrstr reference six functions with no runtime
///      implementation yet).
///
/// This mirrors registry/IntrinsicRegistry.hpp's own justification almost
/// exactly, just for the runtime ABI surface instead of the intrinsic
/// surface: a small, dependency-light, pure-data table that every codegen
/// file consults instead of re-deriving the same information locally.
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
enum class RuntimeFn {
    // ─── Closures ───────────────────────────────────────────────────────
    AllocEnv,           // void* __lucid_alloc_env(uint64_t size)

    // ─── Memory Management ──────────────────────────────────────────────
    Alloc,              // void* __lucid_alloc(uint64_t size)
    Free,               // void __lucid_free(void* ptr)
    ArenaCreate,         // { void*, uint64_t } __lucid_arena_create(uint64_t size)
    ArenaAlloc,          // void* __lucid_arena_alloc(void* arena, uint64_t size)
    ArenaReset,          // void __lucid_arena_reset(void* arena)
    ArenaFree,           // void __lucid_arena_free(void* arena)

    // ─── Strings ────────────────────────────────────────────────────────
    StrConcat,           // string __lucid_str_concat(string a, string b)
    StrSlice,            // string __lucid_str_slice(string s, int64 from, int64 to)
    StrEq,               // bool __lucid_str_eq(string a, string b)

    // ─── #tostr / #ptrstr formatters ────────────────────────────────────
    PtrToHexString,       // string __lucid_ptr_to_hex_string(void* ptr)
    BoolToStr,            // string __lucid_bool_to_str(bool b)
    CharToStr,            // string __lucid_char_to_str(int32 codepoint)
    IntToStr,             // string __lucid_int_to_str(int64 v)
    UintToStr,            // string __lucid_uint_to_str(int64 v)  (bit pattern, formatted unsigned)
    FloatToStr,           // string __lucid_float_to_str(double v)

    // ─── Panics ─────────────────────────────────────────────────────────
    Panic,                // void __lucid_panic(char* message)
};

/// @brief One registry entry: the linker symbol name, plus a builder that
/// constructs the matching llvm::FunctionType on demand (deferred, since
/// building a FunctionType needs a live CodeGenContext for ctx.llvmCtx /
/// ctx.getStringType() / etc., which isn't available at static-init time).
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