/// @file runtime/exports.cpp
/// @brief Forces every runtime function's definition to be linked.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The link-completeness half of the ABI check. It takes the address of every
/// function in `runtime-abi/functions.def` into a table. If a row's symbol is
/// never defined, that reference is unresolved and the linker fails with the
/// symbol's name:
///
///     undefined reference to `__lucid_leak_report'
///
/// Without this file, a row with no implementation would go unnoticed until
/// generated code called it.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the signature check. That belongs to `runtime-abi/lucid_runtime.h`,
/// which every runtime implementation file includes. A signature check only
/// works when the declaration and the definition share a translation unit;
/// this file shares none with the runtime's definitions, so a prototype here
/// could never catch a mismatch (C names are not mangled, and the linker
/// matches on the name alone). It includes the header only so that the
/// symbols it takes the address of are declared, from the table, in one place.
///
/// It is NOT the implementation. The `extern "C"` functions are defined in
/// `src/runtime/*.cpp`.
///
/// It is NOT a runtime symbol registry. There is no name lookup and no
/// dynamic dispatch; the table holds addresses only, and nothing reads it.
///
/// ─── Where To Build It ────────────────────────────────────────────────────
/// Compile this file into the targets that must contain the whole runtime:
/// the compiler binary (whose JIT resolves any row by name) and the runtime
/// test target. Do NOT put it in the AOT runtime library. There it would
/// reference every function, so the linker could no longer drop the runtime
/// objects a program does not use, defeating the manifest-driven selection
/// that `Abi` records `usedRuntimeFns()` for.
///
/// ─── Why Not Include LLVM? ────────────────────────────────────────────────
/// The runtime is on the far side of the ABI boundary. This file depends only
/// on `runtime-abi/lucid_runtime.h` (and through it `lucid_abi.h`), so the
/// runtime stays buildable without LLVM or `codegen/`.

#include "runtime-abi/lucid_runtime.h"

// ─────────────────────────────────────────────────────────────────────────────
// Force the definitions to be linked
// ─────────────────────────────────────────────────────────────────────────────
//
// The prototypes from lucid_runtime.h are declarations. An unused declaration
// does not require a definition, so to force the linker to require every
// runtime function, take the address of each into an array. The array must
// have EXTERNAL linkage, or the compiler is free to drop it as unused and the
// check silently vanishes. A `const` object at namespace scope has internal
// linkage by default, and with optimisation on the whole table is discarded
// (the object file comes out empty). The explicit `extern` declaration below
// gives the definition external linkage, so it survives at every -O level.
//
// `Symbol` is the table's bare identifier, so `&Symbol` is the function's
// address. The `reinterpret_cast` to `const void*` is how function pointers of
// different signatures are stored in one array; nothing ever calls through it.

extern "C" {
extern const void* const __lucid_runtime_function_addresses[];
const void* const __lucid_runtime_function_addresses[] = {
#define LUCID_RT(EnumName, Symbol, Ret, Params) \
    reinterpret_cast<const void*>(&Symbol),
#include "runtime-abi/functions.def"
#undef LUCID_RT
};
}

// A compile-time check that the array is non-empty. This catches the failure
// mode where `functions.def` did not expand at all (misdefined `LUCID_RT`,
// missing include, ...), which would leave the table empty and the pass
// silent.
static_assert(
    sizeof(__lucid_runtime_function_addresses) > 0,
    "the runtime function address table is empty — functions.def did not "
    "expand, or LUCID_RT was misdefined");