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
///            `src/runtime/closure/ClosureRuntime.cpp`
/// ════════════════════════════════════════════════════════════════════════════
///
/// ─── The Two Steps: Declaration vs Implementation ──────────────────────────
///
///   1. DECLARATION (This File + .cpp)
///      ┌─────────────────────────────────────────────────────────────────────┐
///      │  Purpose: Tell LLVM "there exists a function with this name and     │
///      │           signature" so CodeGen can generate call instructions.     │
///      │                                                                     │
///      │  Example:                                                           │
///      │    enum class RuntimeFn { RetainEnv };                              │
///      │    { RuntimeFn::RetainEnv, { "__lucid_retain_env",                  │
///      │        [](CodeGenContext& ctx) {                                    │
///      │            return FunctionType::get(void, {ptr});                   │
///      │        } } }                                                        │
///      └─────────────────────────────────────────────────────────────────────┘
///
///   2. IMPLEMENTATION (src/runtime/closure/ClosureRuntime.cpp)
///      ┌─────────────────────────────────────────────────────────────────────┐
///      │  Purpose: The actual C++ code that runs when the function is        │
///      │           called at runtime.                                        │
///      │                                                                     │
///      │  Example:                                                           │
///      │    extern "C" {                                                     │
///      │        void __lucid_retain_env(void* env) {                         │
///      │            auto* header = (ClosureEnvHeader*)env;                   │
///      │            header->refcount.fetch_add(1);                           │
///      │        }                                                            │
///      │    }                                                                │
///      └─────────────────────────────────────────────────────────────────────┘
///
/// ─── Why Both Are Needed ──────────────────────────────────────────────────
///
///    Without declaration: LLVM doesn't know what type of function to call
///    Without implementation: Linker can't resolve the symbol
///
///    Both must exist for the function to work correctly.
///
/// ─── The Full Pipeline ─────────────────────────────────────────────────────
///
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │  1. ENUMERATE (RuntimeFunctionRegistry.hpp)                             │
///    │     ┌───────────────────────────────────────────────────────────────┐   │
///    │     │  enum class RuntimeFn { RetainEnv, ReleaseEnv, ... };         │   │
///    │     └───────────────────────────────────────────────────────────────┘   │
///    │     Purpose: Give the function a unique ID in the compiler              │
///    └─────────────────────────────────────────────────────────────────────────┘
///                                      │
///                                      ▼
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │  2. DECLARE SIGNATURE (RuntimeFunctionRegistry.cpp)                     │
///    │     ┌───────────────────────────────────────────────────────────────┐   │
///    │     │  { RuntimeFn::RetainEnv, { "__lucid_retain_env",              │   │
///    │     │      [](CodeGenContext& ctx) {                                │   │
///    │     │          return FunctionType::get(void, {ptr});               │   │
///    │     │      } } }                                                    │   │
///    │     └───────────────────────────────────────────────────────────────┘   │
///    │     Purpose: Tell LLVM what the function signature is                   │
///    └─────────────────────────────────────────────────────────────────────────┘
///                                      │
///                                      ▼
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │  3. GENERATE CALL (CodeGen/*.cpp)                                       │
///    │     ┌───────────────────────────────────────────────────────────────┐   │
///    │     │  llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::RetainEnv); │   │
///    │     │  ctx.builder.CreateCall(fn, {envPtr});                        │   │
///    │     └───────────────────────────────────────────────────────────────┘   │
///    │     Purpose: Emits "call void @__lucid_retain_env(i8* %env)"            │
///    └─────────────────────────────────────────────────────────────────────────┘
///                                      │
///                                      ▼
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │  4. IMPLEMENT (src/runtime/closure/ClosureRuntime.cpp)                  │
///    │     ┌───────────────────────────────────────────────────────────────┐   │
///    │     │  extern "C" {                                                 │   │
///    │     │      void __lucid_retain_env(void* env) {                     │   │
///    │     │          auto* header = (ClosureEnvHeader*)env;               │   │
///    │     │          header->refcount.fetch_add(1);                       │   │
///    │     │      }                                                        │   │
///    │     │  }                                                            │   │
///    │     └───────────────────────────────────────────────────────────────┘   │
///    │     Purpose: The actual code that runs when the function is called      │
///    └─────────────────────────────────────────────────────────────────────────┘
///                                      │
///                                      ▼
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │  5. LINK (Build System)                                                 │
///    │     ┌───────────────────────────────────────────────────────────────┐   │
///    │     │  lucid.exe: CodeGen calls __lucid_retain_env                  │   │
///    │     │           + runtime.a (contains __lucid_retain_env)           │   │
///    │     │           = The call resolves to the implementation!          │   │
///    │     └───────────────────────────────────────────────────────────────┘   │
///    └─────────────────────────────────────────────────────────────────────────┘
///
/// ─── Runtime Functions vs Foreign Functions ─────────────────────────────────
///
///    ┌────────────────────────────────────────────────────────────────────────┐
///    │                    COMPARISON TABLE                                    │
///    ├──────────────────────┬──────────────────────────┬──────────────────────┤
///    │                      │ RUNTIME FUNCTIONS        │ FOREIGN FUNCTIONS    │
///    │                      │ (__lucid_*)              │ (@[foreign("C")])    │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ Where declared       │ RuntimeFunctionRegistry  │ User .luc files      │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ Where implemented    │ src/runtime/ (C++)       │ System libraries     │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ How linked           │ Statically into binary   │ Dynamically loaded   │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ User writes them?    │ No (compiler-generated)  │ Yes (@[foreign])     │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ Symbol prefix        │ __lucid_*                │ Any name             │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ Distribution         │ Inside the binary        │ System/game DLLs     │
///    ├──────────────────────┼──────────────────────────┼──────────────────────┤
///    │ Example              │ __lucid_retain_env       │ printf, SDL_Init     │
///    └──────────────────────┴──────────────────────────┴──────────────────────┘
///
/// ─── Runtime Function Distribution ──────────────────────────────────────────
///
///    ┌─────────────────────────────────────────────────────────────────────────┐
///    │                    HOW RUNTIME FUNCTIONS ARE DISTRIBUTED                │
///    ├─────────────────────────────────────────────────────────────────────────┤
///    │                                                                         │
///    │  JIT Mode (lucid run):                                                  │
///    │    Runtime functions are INSIDE lucid.exe                               │
///    │    JIT-compiled code calls them directly (same process)                 │
///    │                                                                         │
///    │  AOT Mode (lucid build):                                                │
///    │    Runtime functions are linked INTO the final binary                   │
///    │    User's game.exe contains all runtime functions                       │
///    │                                                                         │
///    │  Key point: User NEVER needs to install a separate runtime library.     │
///    │  Everything is statically linked into the binary.                       │
///    └─────────────────────────────────────────────────────────────────────────┘
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
enum class RuntimeFn {
    // ─── Closures ───────────────────────────────────────────────────────
    AllocEnv,           // void* __lucid_alloc_env(uint64_t size)
    IsClosure,          // bool __lucid_is_closure(void* value)
    RetainEnv,          // void __lucid_retain_env(void* env)
    ReleaseEnv,         // void __lucid_release_env(void* env)

    // ─── Memory Management ──────────────────────────────────────────────
    Alloc,              // void* __lucid_alloc(uint64_t size)
    Free,               // void __lucid_free(void* ptr)
    ArenaCreate,        // { void*, uint64_t } __lucid_arena_create(uint64_t size)
    ArenaAlloc,         // void* __lucid_arena_alloc(void* arena, uint64_t size)
    ArenaReset,         // void __lucid_arena_reset(void* arena)
    ArenaFree,          // void __lucid_arena_free(void* arena)

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
};

/// @brief One registry entry: the linker symbol name, plus a builder that
/// constructs the matching llvm::FunctionType on demand (deferred, since
/// building a FunctionType needs a live CodeGenContext for ctx.llvmCtx /
/// ctx.getStringType() / etc., which isn't available at static-init time).
///
/// ─── IMPORTANT: ──────────────────────────────────────────────────────────────
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