/// @file runtime-abi/lucid_runtime.h
/// @brief The `extern "C"` prototype of every runtime function, expanded
///        from `functions.def`.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The runtime-side view of the table. Including it declares, for each row,
///
///     extern "C" <Ret> <symbol>(<Params>);
///
/// with the tags replaced by the C++ types in `lucid_abi.h`.
///
/// ─── Why It Is A Header ───────────────────────────────────────────────────
/// A signature check only works when the declaration and the definition are
/// in the SAME translation unit. Two declarations of a C-linkage function in
/// one file must agree, and the compiler says so:
///
///     error: conflicting declaration of C function ...
///
/// In separate files the compiler and the linker both stay silent — C names
/// are not mangled, so the linker matches on the name alone. That is why the
/// prototypes cannot live in a file nobody includes: every file in
/// `src/runtime/` that DEFINES a runtime function must `#include` this
/// header, and that is what makes a wrong parameter type, a wrong return
/// type or a wrong arity in a definition a compile error.
///
/// It re-derives its content from the table every time it is included, so it
/// is an expansion of `functions.def`, not a second place the ABI is written
/// down.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It does not check that a row HAS a definition. That is `exports.cpp`,
/// which takes every function's address and lets the linker complain.
///
/// It does not depend on LLVM or on `codegen/`. Its only include is
/// `lucid_abi.h`, so the runtime stays buildable without the compiler.
///
/// ─── Tag Macros ───────────────────────────────────────────────────────────
/// The table's rows use bare tag identifiers (`I64`, `Ptr`, ...). Here each
/// one is a macro for the corresponding C++ type. The macros exist only
/// while the table is expanded and are saved/restored with push_macro, so
/// including this header never leaks a name like `Ptr` into the includer.
/// A tag that is not in the list below (there is deliberately no by-value
/// struct tag) is an undeclared identifier, so a table error fails here, in
/// the runtime build, as well as in Abi.cpp.

#pragma once

#include "lucid_abi.h"

#pragma push_macro("LUCID_RT")
#pragma push_macro("LUCID_RT_UNPACK")
#pragma push_macro("Void")
#pragma push_macro("I1")
#pragma push_macro("I8")
#pragma push_macro("I32")
#pragma push_macro("I64")
#pragma push_macro("F64")
#pragma push_macro("Ptr")
#pragma push_macro("StrPtr")
#pragma push_macro("SlicePtr")
#pragma push_macro("ArenaPtr")
#pragma push_macro("DescPtr")

#undef LUCID_RT
#undef LUCID_RT_UNPACK
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

#define Void      void
#define I1        ::lucid::abi::LucidBool
#define I8        ::lucid::abi::LucidI8
#define I32       ::lucid::abi::LucidI32
#define I64       ::lucid::abi::LucidI64
#define F64       ::lucid::abi::LucidF64
#define Ptr       ::lucid::abi::LucidPtr
#define StrPtr    ::lucid::abi::LucidString*
#define SlicePtr  ::lucid::abi::LucidSlice*
#define ArenaPtr  ::lucid::abi::LucidArena*
#define DescPtr   ::lucid::abi::LucidArenaDescriptor*

// `LUCID_RT_UNPACK Params` turns the parenthesised tuple `(A, B)` into the
// bare list `A, B` (and `()` into nothing). No counting, so no arity cap.
#define LUCID_RT_UNPACK(...) __VA_ARGS__

#define LUCID_RT(EnumName, Symbol, Ret, Params) \
    extern "C" Ret Symbol(LUCID_RT_UNPACK Params);

#include "functions.def"

#pragma pop_macro("DescPtr")
#pragma pop_macro("ArenaPtr")
#pragma pop_macro("SlicePtr")
#pragma pop_macro("StrPtr")
#pragma pop_macro("Ptr")
#pragma pop_macro("F64")
#pragma pop_macro("I64")
#pragma pop_macro("I32")
#pragma pop_macro("I8")
#pragma pop_macro("I1")
#pragma pop_macro("Void")
#pragma pop_macro("LUCID_RT_UNPACK")
#pragma pop_macro("LUCID_RT")