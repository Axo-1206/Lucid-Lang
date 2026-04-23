/**
 * @file IntrinsicRegistry.hpp
 *
 * @nutshell Maps Luc '@' intrinsic names to LLVM intrinsic IDs and their
 *   argument / return-type shapes.
 *
 * @reason Every new @intrinsic would otherwise require a manual if/switch
 *   in the Codegen phase. The registry centralises all that information in
 *   one table so Codegen can look up what to emit without branching.
 *
 * @responsibility Compile-time table consumed by the Codegen phase (and
 *   cross-checked by the Semantic phase for argument validation).
 *
 * @architecture
 *   The plan ("Backend Mapping Expansion") mandates:
 *   "Implement an Intrinsic Registry. Instead of hardcoding logic, use a
 *    table that maps the Luc name to the LLVM Intrinsic ID."
 *
 *   This file provides:
 *     IntrinsicArgKind  — what kind of argument each slot expects
 *     IntrinsicEntry    — one row in the registry (name → LLVM ID + shapes)
 *     IntrinsicRegistry — the static table + lookup helpers
 *
 * @usage (Codegen):
 *   const IntrinsicEntry* e = IntrinsicRegistry::lookup("sqrt");
 *   if (e) {
 *       // emit LLVM intrinsic call using e->llvmID
 *   }
 *
 * @usage (Semantic — already handled in SemanticExpr.cpp):
 *   The semantic pass validates arg counts and types directly; it does NOT
 *   use this registry so the semantic layer stays independent of LLVM.
 *   The registry is a codegen concern only.
 *
 * @note
 *   LLVM intrinsic IDs are referenced symbolically here as string names
 *   (e.g. "llvm.sqrt.f32") rather than numeric llvm::Intrinsic::ID values.
 *   This keeps IntrinsicRegistry.hpp free of LLVM headers and lets it be
 *   included by any compiler layer that only needs the metadata.
 *   Codegen translates the string ID to the numeric ID via
 *   llvm::Function::lookupIntrinsicID() or a local mapping table.
 */

#pragma once

#include <string>
#include <vector>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicArgKind  — The expected kind of each argument slot
//
// Codegen uses this to decide how to emit each operand.
// ─────────────────────────────────────────────────────────────────────────────
enum class IntrinsicArgKind {
    TypeArg,   // compile-time type operand  (@sizeof(T))      — no runtime value
    AnyValue,  // any runtime value — codegen defers to the LLVM signature
    IntValue,  // must be an integer type at the semantic level
    FloatValue,// must be float or double at the semantic level
    PtrValue,  // pointer / reference — used for memcpy/memset destination
    SizeValue, // uint64 size argument — used for memcpy/memset length
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicReturnKind  — What the intrinsic call evaluates to
// ─────────────────────────────────────────────────────────────────────────────
enum class IntrinsicReturnKind {
    Void,          // no value produced  (memcpy, memset)
    Uint64,        // compile-time size  (sizeof, alignof)
    Float32,       // 32-bit float       (sqrt when arg is float)
    Float64,       // 64-bit float       (sqrt when arg is double)
    SameAsArg0,    // result = type of first value argument  (abs, min, max)
    SameAsArg1,    // result = type of second argument
    RefOfTypeArg0, // result = &T where T is the first type argument
    Int64,         // 64-bit integer
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicEntry  — One row in the registry
//
// Fields:
//   lucName       — the Luc '@' name, e.g. "sqrt"
//   llvmID        — the LLVM intrinsic name, e.g. "llvm.sqrt.f32"
//                   For overloaded intrinsics (sqrt.f32 vs sqrt.f64) Codegen
//                   selects the correct variant based on the operand type at
//                   the call site.  The base name without suffix is stored here.
//   argKinds      — expected shape of each argument in order
//   returnKind    — what the call produces
//   isOverloaded  — true when the LLVM ID must be suffixed with a type string
//                   at codegen time (e.g. "llvm.sqrt" → "llvm.sqrt.f32")
//   minArgs       — minimum number of value arguments (excluding type args)
//   maxArgs       — maximum number of value arguments (-1 = variadic)
//   notes         — human-readable description for IDE / diagnostics
// ─────────────────────────────────────────────────────────────────────────────
struct IntrinsicEntry {
    const char*                     lucName;
    const char*                     llvmID;
    std::vector<IntrinsicArgKind>   argKinds;
    IntrinsicReturnKind             returnKind;
    bool                            isOverloaded;
    int                             minArgs;
    int                             maxArgs;     // -1 = variadic
    const char*                     notes;
};

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry  — The static lookup table + helpers
//
// All entries are stored as a flat array of IntrinsicEntry values.  Lookup is
// O(N) over the small fixed-size table — acceptable for a compiler pass.
//
// Adding a new intrinsic:
//   1. Add a row to kEntries below.
//   2. Add validation logic in SemanticExpr.cpp::checkIntrinsicCallExpr().
//   3. Add codegen logic in CodeGenExpr.cpp (future).
// ─────────────────────────────────────────────────────────────────────────────
class IntrinsicRegistry {
public:

    // ── Static registry table ─────────────────────────────────────────────────
    static const IntrinsicEntry kEntries[];
    static const std::size_t    kEntryCount;

    // ── Lookup by Luc name ───────────────────────────────────────────────────
    // Returns a pointer into kEntries, or nullptr if unknown.
    static const IntrinsicEntry* lookup(const std::string& lucName) {
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            if (lucName == kEntries[i].lucName)
                return &kEntries[i];
        }
        return nullptr;
    }

    // ── Known name check ─────────────────────────────────────────────────────
    static bool isKnown(const std::string& lucName) {
        return lookup(lucName) != nullptr;
    }

    // ── Convenience: list all known names (for diagnostic messages) ──────────
    static std::string allNames() {
        std::string result;
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            if (i) result += ", ";
            result += kEntries[i].lucName;
        }
        return result;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// kEntries  — The registry table definition
//
// Column layout:
//   lucName | llvmID | argKinds | returnKind | isOverloaded | minArgs | maxArgs | notes
// ─────────────────────────────────────────────────────────────────────────────
inline const IntrinsicEntry IntrinsicRegistry::kEntries[] = {

    // ── Compile-time type queries ─────────────────────────────────────────────
    {
        "sizeof",
        "llvm.none",          // not a runtime LLVM intrinsic — folded at IR level
        { IntrinsicArgKind::TypeArg },
        IntrinsicReturnKind::Uint64,
        false,
        0, 0,
        "@sizeof(T) — compile-time byte size of type T; equivalent to C sizeof(T)"
    },
    {
        "alignof",
        "llvm.none",          // not a runtime LLVM intrinsic — folded at IR level
        { IntrinsicArgKind::TypeArg },
        IntrinsicReturnKind::Uint64,
        false,
        0, 0,
        "@alignof(T) — compile-time alignment requirement of type T in bytes"
    },

    // ── Floating-point math ───────────────────────────────────────────────────
    {
        "sqrt",
        "llvm.sqrt",          // overloaded: llvm.sqrt.f32 / llvm.sqrt.f64
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@sqrt(x) — hardware-accelerated square root; x must be float or double"
    },
    {
        "abs",
        "llvm.abs",           // overloaded: llvm.abs.i32 etc.; also fabs for floats
        { IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@abs(x) — absolute value; works on integers and floats"
    },
    {
        "min",
        "llvm.minnum",        // overloaded; for integers: llvm.smin / llvm.umin
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "@min(a, b) — minimum of two values; both must be the same type"
    },
    {
        "max",
        "llvm.maxnum",        // overloaded; for integers: llvm.smax / llvm.umax
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "@max(a, b) — maximum of two values; both must be the same type"
    },
    {
        "floor",
        "llvm.floor",         // overloaded: llvm.floor.f32 / llvm.floor.f64
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@floor(x) — round x down to the nearest integer (as float); x must be float/double"
    },
    {
        "ceil",
        "llvm.ceil",          // overloaded: llvm.ceil.f32 / llvm.ceil.f64
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@ceil(x) — round x up to the nearest integer (as float); x must be float/double"
    },
    {
        "round",
        "llvm.round",         // overloaded
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@round(x) — round x to the nearest integer, halfway away from zero"
    },
    {
        "pow",
        "llvm.pow",           // overloaded: llvm.pow.f32 / llvm.pow.f64
        { IntrinsicArgKind::FloatValue, IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "@pow(base, exp) — base raised to exp; both must be float/double"
    },
    {
        "fma",
        "llvm.fma",           // overloaded: fused multiply-add
        { IntrinsicArgKind::FloatValue,
          IntrinsicArgKind::FloatValue,
          IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        3, 3,
        "@fma(a, b, c) — fused multiply-add: (a * b) + c, single rounding; all float/double"
    },

    // ── Bit manipulation ──────────────────────────────────────────────────────
    {
        "clz",
        "llvm.ctlz",          // count leading zeros; overloaded on int width
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@clz(x) — count leading zero bits in x; x must be an integer type"
    },
    {
        "ctz",
        "llvm.cttz",          // count trailing zeros
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@ctz(x) — count trailing zero bits in x; x must be an integer type"
    },
    {
        "popcount",
        "llvm.ctpop",         // population count (number of set bits)
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@popcount(x) — number of set (1) bits in x; x must be an integer type"
    },
    {
        "bswap",
        "llvm.bswap",         // byte-swap (reverse byte order)
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "@bswap(x) — reverse the byte order of x; useful for endianness conversion"
    },

    // ── Memory operations ─────────────────────────────────────────────────────
    {
        "memcpy",
        "llvm.memcpy",        // overloaded on pointer type width
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,
        3, 3,
        "@memcpy(dest, src, len) — copy len bytes from src to dest; non-overlapping"
    },
    {
        "memmove",
        "llvm.memmove",       // handles overlapping regions
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,
        3, 3,
        "@memmove(dest, src, len) — move len bytes from src to dest; handles overlap"
    },
    {
        "memset",
        "llvm.memset",
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::IntValue,   // fill byte value (uint8)
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,
        3, 3,
        "@memset(dest, value, len) — fill len bytes at dest with byte value"
    },

    // ── Vulkan / GPU helpers ──────────────────────────────────────────────────
    {
        "bitcast",
        "llvm.bitcast",       // unsafe bit-level reinterpret between same-size types
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg1,  // return type = typeArg
        false,
        1, 1,
        "@bitcast(T, x) — reinterpret the bits of x as type T; sizes must match"
    },

    // ── Pointer operations (Sealed Conduit model) ─────────────────────────────
    {
        "ptrToRef",
        "llvm.none",          // handled in codegen via bitcast/addrspacecast
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::RefOfTypeArg0,
        false,
        1, 1,
        "@ptrToRef(T, ptr) — cross the safety boundary: convert raw pointer *T to safe reference &T"
    },
    {
        "refToPtr",
        "llvm.none",
        { IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::SameAsArg0, // In Luc, &T and *T are both PtrValue at low level, but Semantic will fix it
        false,
        1, 1,
        "@refToPtr(ref) — convert a safe reference &T to a raw pointer *T"
    },
    {
        "ptrOffset",
        "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        false,
        2, 2,
        "@ptrOffset(ptr, n) — pointer arithmetic: returns ptr + n as a raw pointer"
    },
    {
        "ptrDiff",
        "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::Int64,
        false,
        2, 2,
        "@ptrDiff(p1, p2) — returns the distance between two pointers in elements"
    },
};

inline const std::size_t IntrinsicRegistry::kEntryCount =
    sizeof(IntrinsicRegistry::kEntries) / sizeof(IntrinsicRegistry::kEntries[0]);