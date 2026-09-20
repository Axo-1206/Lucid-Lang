/// @file runtime/exports.cpp
/// @brief Declares every runtime function's `extern "C"` prototype, and
///        forces its definition to be linked.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The link between `runtime-abi/functions.def` and the runtime
/// implementations in `src/runtime/*.cpp`. It re-includes `functions.def`
/// twice:
///
///   1. To emit an `extern "C"` prototype for each row. This is what makes
///      a signature mismatch a compile error: if `StringRuntime.cpp`
///      defines `__lucid_str_concat` with a parameter of type `int*`
///      instead of `LucidString*`, the compiler sees two conflicting
///      declarations and rejects the file.
///
///   2. To take the address of each function into a table. This is what
///      makes a missing implementation a link error: if a row's symbol is
///      never defined, the table's reference is unresolved and the linker
///      fails with the symbol's name.
///
/// Together, the two passes mean any drift between the table and the
/// runtime is caught at build time, with a diagnostic that names the
/// function. The header comment in `functions.def` promises this; this
/// file is what delivers it.
///
/// ─── Why Not a Header? ────────────────────────────────────────────────────
/// A header of hand-written prototypes would be a fourth place the runtime
/// surface is written down, after `functions.def`'s three uses (in
/// `Abi.hpp`, in `Abi.cpp`, and here) and the runtime implementations
/// themselves. That's the failure mode `functions.def` exists to prevent.
/// This file is a translation unit, not a header: it doesn't get included
/// by anything else, and it re-derives its content from the table every
/// time it's compiled.
///
/// ─── Why Not Include LLVM? ────────────────────────────────────────────────
/// The runtime is on the far side of the ABI boundary. It should not depend
/// on LLVM, on `codegen/`, or on any other compiler-internal type. This
/// file's only dependency is `runtime-abi/lucid_abi.h`, the leaf header
/// every other ABI consumer uses. That keeps the runtime buildable without
/// LLVM, which is what makes it linkable into an AOT binary that doesn't
/// bundle the compiler.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the implementation. The `extern "C"` functions are defined in
/// `src/runtime/*.cpp` (`MemoryRuntime.cpp`, `StringRuntime.cpp`, etc.).
/// This file only declares them and forces their definitions to be linked.
///
/// It is NOT the ABI contract. The layouts (`LucidString`, `LucidArena`,
/// ...) and the type tags come from `runtime-abi/lucid_abi.h`. This file
/// reads them; it does not define them.
///
/// It is NOT a runtime symbol registry. There is no reflection, no name
/// lookup, no dynamic dispatch. Everything is static, compile-time, and
/// link-time. The interpreter's dynamic symbol registration (in
/// `interpreter/JITSession.cpp`) is a separate concern and reads
/// `functions.def` for the symbol names it needs.

#include "runtime-abi/lucid_abi.h"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Tag expansion
// ─────────────────────────────────────────────────────────────────────────────
//
// Each tag in `functions.def` (`I1`, `I8`, ..., `Ptr`, `StrPtr`, ...) is
// mapped here to the C++ type that crosses the ABI. The mapping matches
// the "Type-tag mapping" section of `runtime-abi/lucid_abi.h`; the two
// must agree, and the agreement is what makes `functions.def`'s rows
// usable as C++ prototypes.

using lucid::abi::LucidBool;
using lucid::abi::LucidI1;
using lucid::abi::LucidI8;
using lucid::abi::LucidI32;
using lucid::abi::LucidI64;
using lucid::abi::LucidF64;
using lucid::abi::LucidPtr;
using lucid::abi::LucidString;
using lucid::abi::LucidSlice;
using lucid::abi::LucidArena;
using lucid::abi::LucidArenaDescriptor;

#define Void     void
#define I1       LucidBool
#define I8       LucidI8
#define I32      LucidI32
#define I64      LucidI64
#define F64      LucidF64
#define Ptr      LucidPtr

// Pointer-to-struct tags. These are the tags the string, arena, and
// descriptor rows actually use. They expand to the corresponding C++
// pointer type, which lets the compiler check the runtime implementation's
// parameter types against the table. A row that declared `StrPtr` and a
// runtime that implemented `int*` would conflict at compile time, because
// `LucidString*` and `int*` are different types.
#define StrPtr   LucidString*
#define SlicePtr LucidSlice*
#define ArenaPtr LucidArena*
#define DescPtr  LucidArenaDescriptor*

// By-value struct tags. No row uses them — see the by-pointer convention
// in functions.def — but they are defined so an accidental use fails
// loudly. A row that used `Str` would produce a prototype with the
// `LucidString` struct itself, not a pointer; the runtime implementation,
// which takes `LucidString*`, would conflict at compile time.
//
// This is weaker than it looks: the compiler catches the mismatch between
// the by-value declaration and the pointer-based implementation, but the
// error message names the conflicting declarations rather than the
// by-pointer convention. `Abi.cpp`'s debug assertion gives the clearer
// message. Between the two, an accidental use is caught either at build
// time (here) or at the debug-build declaration site (Abi.cpp).
#define Str      LucidString
#define Slice    LucidSlice
#define Arena    LucidArena
#define Desc     LucidArenaDescriptor

// ─────────────────────────────────────────────────────────────────────────────
// Pass 1 — declare the prototypes
// ─────────────────────────────────────────────────────────────────────────────
//
// Each row becomes an `extern "C"` prototype:
//
//     LUCID_RT(Alloc, "__lucid_alloc", Ptr, (I64))
//     → extern "C" LucidPtr __lucid_alloc(LucidI64);
//
//     LUCID_RT(StrConcat, "__lucid_str_concat", Void, (StrPtr, StrPtr, StrPtr))
//     → extern "C" void __lucid_str_concat(LucidString*, LucidString*,
//                                          LucidString*);
//
// The `LUCID_RT_EXPAND_PARAMS` indirection is what turns the row's
// parenthesized parameter list into the prototype's parameter list. The
// parameter tuple `(I64)` is passed as a single argument to
// `LUCID_RT_EXPAND_PARAMS`, which expands to its own `__VA_ARGS__` — the
// bare list `I64` — and the surrounding `(...)` in the `LUCID_RT` macro
// gives the C++ parameter list syntax.
//
// For a zero-parameter row (`LUCID_RT(LeakReport, ..., Void, ())`), the
// parameter tuple is `()`, the expansion is `LUCID_RT_EXPAND_PARAMS()`,
// which produces an empty argument list, and the prototype is
// `extern "C" void __lucid_leak_report();`. In C++ that is a prototype
// with no parameters, which is the intent.

#define LUCID_RT_EXPAND_PARAMS(...) __VA_ARGS__

#define LUCID_RT(EnumName, Symbol, Ret, Params)  \
    extern "C" Ret Symbol(LUCID_RT_EXPAND_PARAMS Params);

#include "runtime-abi/functions.def"

#undef LUCID_RT
#undef LUCID_RT_EXPAND_PARAMS

// ─────────────────────────────────────────────────────────────────────────────
// Pass 2 — force the definitions to be linked
// ─────────────────────────────────────────────────────────────────────────────
//
// The prototypes above are declarations. An unused declaration does not
// require a definition, so if `StringRuntime.cpp` forgot to implement
// `__lucid_str_concat`, the build would still succeed — the reference in
// `functions.def` would be a table entry, not a use.
//
// To force the linker to require every runtime function's definition, take
// the address of each function into an array. The array has external
// linkage and is `extern "C"`, so the compiler cannot prove it is unused
// and cannot optimize it away. Every element is a reference to a runtime
// function; the linker resolves each against the runtime object files, and
// a missing definition is a link-time error naming the symbol.
//
// The `reinterpret_cast<const void*>` is the portable way to store function
// pointers of different signatures in one array. The standard guarantees
// that a function pointer converts to `void*` and back (for calling) via
// `reinterpret_cast`; here we never call through the array — its only
// purpose is to hold the references — so no calling-convention issue
// arises.

extern "C" {
const void* const __lucid_runtime_function_addresses[] = {
#define LUCID_RT(EnumName, Symbol, Ret, Params) \
    reinterpret_cast<const void*>(&Symbol),
#include "runtime-abi/functions.def"
#undef LUCID_RT
};
}

// ─────────────────────────────────────────────────────────────────────────────
// Pass 3 — sanity check
// ─────────────────────────────────────────────────────────────────────────────
//
// A compile-time check that the array is non-empty. This catches the
// failure mode where `functions.def` did not expand at all (misdefined
// `LUCID_RT`, missing include, etc.), which would leave the table empty
// and the pass silent.

static_assert(
    sizeof(__lucid_runtime_function_addresses) > 0,
    "the runtime function address table is empty — functions.def did not "
    "expand, or LUCID_RT was misdefined");

// ─────────────────────────────────────────────────────────────────────────────
// Cleanup
// ─────────────────────────────────────────────────────────────────────────────

#undef Void
#undef I1
#undef I8
#undef I32
#undef I64
#undef F64
#undef Ptr
#undef StrPtr
#undef SlicePtr
#undef ArenaPtr
#undef DescPtr
#undef Str
#undef Slice
#undef Arena
#undef Desc