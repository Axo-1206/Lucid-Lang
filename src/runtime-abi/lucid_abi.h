/// @file runtime-abi/lucid_abi.h
/// @brief The ABI contract between CodeGen, the runtime, and the interpreter.
///
/// ─── What This File Is ─────────────────────────────────────────────────────
/// This is a leaf header. It depends on <cstdint>, <cstddef>, and nothing
/// else — no LLVM, no Lucid AST, no C++ standard library containers. It is
/// included by:
///
///   - src/codegen/      (to construct and consume the layouts, and to
///                        declare the runtime functions it calls)
///   - src/runtime/      (to define those functions with matching signatures)
///   - src/interpreter/  (to look up dynamic symbols by name)
///
/// Because it is a leaf and contains only facts, it is the one file every
/// side of the ABI boundary can agree on without creating a cycle. Before
/// this file existed, "the layout of a Lucid string" was spelled out
/// independently in CodeGenType.cpp, in StringRuntime.cpp, and implicitly
/// in the interpreter's symbol table — three spellings that had to be kept
/// in sync by hand.
///
/// ─── What This File Is NOT ─────────────────────────────────────────────────
/// It is NOT the implementation. The functions whose signatures appear in
/// functions.def are implemented in src/runtime/*.cpp. This header only
/// pins the *shape* of the data they exchange.
///
/// It is NOT a C++ API. The structs here are C-layout structs; they exist
/// to be laid out identically on both sides of the boundary. CodeGen never
/// constructs a `LucidString` in C++; it constructs the equivalent
/// `llvm::StructType` in `codegen/Types.cpp`. The `static_assert`s at the
/// bottom of this file are what keep the two in sync: if a layout changes
/// here without the corresponding change in CodeGen's type mapping, the
/// generated module fails `llvm::verifyModule` — but more importantly, if
/// it changes here without the corresponding change in functions.def's
/// type tags, `runtime/exports.cpp` fails to compile.
///
/// ─── The Static-Assert Strategy ────────────────────────────────────────────
/// This file is the only place where the ABI's numeric widths and offsets
/// are pinned. Every consumer derives its own view from these pins:
///
///   - src/runtime/*.cpp uses the C++ types directly. If a runtime
///     implementation uses `int` where the ABI says `LucidI64`, the
///     mismatch shows up when exports.cpp compares the function pointer
///     against the table's expected signature — a compile error, not a
///     runtime miscompile.
///
///   - src/codegen/Abi.cpp builds an llvm::FunctionType from each
///     functions.def row. The row's type tag (I32, I64, Str, ...) maps to
///     an LLVM type via a table in Abi.cpp. If CodeGen builds an i32 where
///     the ABI says i64, llvm::verifyModule rejects the module at the
///     call site, because the callee's declared parameter type and the
///     argument's type disagree.
///
///   - src/interpreter/ only reads symbol names, so it does not need the
///     widths. It reads functions.def for the name column.
///
/// The upshot: a width change requires editing this header, and the edit
/// either compiles everywhere or fails everywhere. There is no state where
/// one consumer silently disagrees with another.

#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace lucid::abi {

// ─────────────────────────────────────────────────────────────────────────────
// Named scalar types
// ─────────────────────────────────────────────────────────────────────────────
//
// Every width that crosses the ABI is given a name here, and the name is
// what functions.def's type tags and the runtime's extern "C" definitions
// use. This is what makes the static_asserts below load-bearing: they
// assert the width of the *named* type, and every consumer uses the name.
//
// Do not spell `int64_t` in a runtime signature. Spell `LucidI64`. The
// two are the same type, but only the name is pinned by the asserts.

using LucidI1  = bool;      // ABI bool; see LucidBool below for the nuance
using LucidI8  = int8_t;    // Lucid char, Lucid int8, Lucid byte
using LucidI32 = int32_t;   // Lucid int32, Unicode codepoint
using LucidI64 = int64_t;   // Lucid int64, size, length, capacity, refcount
using LucidF64 = double;    // Lucid double; the ABI's only float width
using LucidPtr = void*;     // opaque pointer, untyped at the ABI level

/// @brief The C-side representation of a Lucid `bool`.
///
/// Lucid `bool` is an i1 in the generated IR. The C ABI does not pass i1
/// parameters directly on all platforms — an i1 is promoted to the
/// platform's minimum integer width, which is 8 on every target Lucid
/// currently supports. Rather than rely on that promotion being
/// consistent, the ABI uses an explicit 8-bit type.
///
/// A Lucid `bool` value is 0 or 1; every value that is not 0 is treated as
/// true by the runtime. The runtime does not assume the caller has
/// canonicalized.
using LucidBool = uint8_t;

// ─────────────────────────────────────────────────────────────────────────────
// lucid.String — { ptr, i64 len, i64 cap }
// ─────────────────────────────────────────────────────────────────────────────
//
// A heap-owned UTF-8 byte buffer plus its length and capacity. `cap == 0`
// is the sentinel for a *static* string: a string literal lowered as a
// private GlobalVariable, whose data pointer must never be passed to
// `lucid_free`. Every consumer that frees a string checks `cap == 0`
// first — see Ownership::drop in the new CodeGen.
//
// `len` is the number of bytes, not the number of codepoints. `cap` is the
// number of bytes the buffer can hold without reallocating; for a static
// string, `cap` is 0 regardless of `len`.
//
// This layout is mirrored by codegen/Types.cpp's `getStringType()`. The
// two must produce the same LLVM struct. The static_asserts at the bottom
// of this file pin the C++ side; codegen's mirror is checked by the
// type-cache invariant that all `lucid.String` types in a module are the
// same llvm::StructType*, and by verifyModule catching a call through a
// differently-shaped type.

struct LucidString {
    const char* data;   // pointer to the bytes; never null when len > 0
    LucidI64    len;    // number of bytes, not codepoints
    LucidI64    cap;    // 0 == static (do not free); > 0 == heap
};

// ─────────────────────────────────────────────────────────────────────────────
// lucid.Slice — { ptr, i64 len, i64 cap }
// ─────────────────────────────────────────────────────────────────────────────
//
// A borrowed view over a contiguous range. Same shape as LucidString, but
// the data pointer is not owned by the holder of the slice; `cap` is the
// number of elements from `data` to the end of the backing allocation,
// not the number of elements the slice covers.
//
// Slices are *not* a resource. There is no retain, no release, no drop
// glue for a slice. The backing array (or arena) owns the memory.
//
// Element type is not part of the layout. A slice of `int` and a slice of
// `Point` have the same three fields; the element type lives in the AST,
// and CodeGen emits a distinct llvm::StructType per element type whose
// field types are identical. This is intentional: the per-element-type
// structs are what make a slice of Point not assignable to a slice of int
// at the LLVM level, even though their layouts are the same.

struct LucidSlice {
    const void* data;
    LucidI64    len;
    LucidI64    cap;
};

// ─────────────────────────────────────────────────────────────────────────────
// lucid.Arena — { ptr base, i64 size, i64 cursor }
// ─────────────────────────────────────────────────────────────────────────────
//
// A bump allocator. `base` is the start of the arena's backing region;
// `size` is the total byte capacity; `cursor` is the current bump offset
// from `base`. The compiler manages this struct directly; user code never
// sees the fields.
//
// Arena is a resource: Ownership::drop frees `base`. It is linear — Sema
// rejects copying an Arena, because a bump allocator's whole point is that
// there is exactly one cursor.

struct LucidArena {
    void*    base;    // start of the backing region; null for an empty arena
    LucidI64 size;    // total byte capacity of the region
    LucidI64 cursor;  // current bump offset from base
};

// ─────────────────────────────────────────────────────────────────────────────
// lucid.ArenaDescriptor — { ptr base, i64 size }
// ─────────────────────────────────────────────────────────────────────────────
//
// The POD projection of an Arena that crosses the FFI boundary. The C
// side sees:
//
//     typedef struct {
//         uint8_t* base;
//         uint64_t size;
//     } LGE_ArenaDescriptor;
//
// This is the *only* arena-related type the C side ever sees. `cursor` is
// deliberately excluded — the internal allocation cursor is not part of
// the stable boundary, so that a future change to the bump strategy does
// not change the FFI layout.

struct LucidArenaDescriptor {
    void*    base;
    LucidI64 size;
};

// ─────────────────────────────────────────────────────────────────────────────
// lucid.ClosureHeader — the prefix of every closure environment
// ─────────────────────────────────────────────────────────────────────────────
//
// A closure value is a fat pointer `{ fn, env }`. `fn` is a bare function
// pointer; `env` points at a heap allocation whose first bytes are a
// ClosureHeader, followed by the captured fields.
//
// The header holds the refcount and a drop callback. When the refcount
// reaches zero, the runtime calls `drop(env)` to release whatever the
// captured fields own (retained closure envs, owned buffers), then frees
// the block. A non-capturing closure has `env == null` and a null-env
// fat pointer is valid to retain/release as a no-op.
//
// `drop` may be null, meaning "the captured fields own nothing; just free
// the block."
//
// The header is 16 bytes. The captured fields begin at offset 16. CodeGen
// emits an llvm::StructType per closure whose first two fields are the
// header's fields and whose remaining fields are the captures; the
// environment pointer is always the address of that struct.

struct LucidClosureHeader {
    LucidI64 refcount;
    void   (*drop)(void* env);
};

// ─────────────────────────────────────────────────────────────────────────────
// lucid.Closure — { fn, env }
// ─────────────────────────────────────────────────────────────────────────────
//
// The runtime shape of every `cls`-shaped function value. `fn` is a
// function pointer whose first parameter is `env`; `env` is either null
// (non-capturing) or a pointer to a LucidClosureHeader-prefixed block.
//
// This struct is the ABI shape, not the C++ calling convention. CodeGen
// never constructs one of these in C++; it builds the equivalent
// `llvm::StructType { ptr, ptr }` and the runtime produces the value.

struct LucidClosure {
    void* fn;
    void* env;
};

// ─────────────────────────────────────────────────────────────────────────────
// The out-pointer convention
// ─────────────────────────────────────────────────────────────────────────────
//
// Every runtime function that *produces* a heap-owning value takes an
// out-pointer as its first parameter and returns void. This includes the
// string constructors, the formatters, and the arena create. The caller
// allocates the slot (an alloca or a stack temporary), passes its address,
// and reads the result back from the slot.
//
//     void __lucid_str_concat(LucidString* out, LucidString a, LucidString b);
//
// This is not a stylistic choice. It is what makes two things possible:
//
//   1. A failure path. A by-value return has no way to signal "the
//      allocation failed" without overloading a sentinel value (a null
//      data pointer, a negative len, ...). An out-pointer leaves the
//      return channel free; a future revision can change the return type
//      to a status code without touching every call site's value type.
//
//   2. One allocator. The runtime's string constructors allocate through
//      `__lucid_alloc`. With a by-value return, the callee allocates and
//      the caller owns the result with no explicit transfer step; with an
//      out-pointer, the transfer is the write through the pointer, and
//      every allocated buffer is unambiguously owned by whoever's slot it
//      was written into. The one-allocator invariant ("every heap object
//      is allocated by __lucid_alloc and freed by __lucid_free, and
//      Ownership::drop is the only caller of __lucid_free") is what makes
//      the leak report meaningful, and the out-pointer convention is what
//      makes that invariant checkable.
//
// The type tag `Ptr` in functions.def means "an out-pointer, or a
// genuinely opaque pointer." The doc comment on each row says which. The
// first parameter of every formatter and string constructor is an
// out-pointer; the `env` parameter of the closure functions and the
// `packet` parameter of the concurrency functions are opaque.

// ─────────────────────────────────────────────────────────────────────────────
// Static assertions
// ─────────────────────────────────────────────────────────────────────────────
//
// These are the contract. If any of them fires, a layout changed without
// the corresponding change in functions.def or in CodeGen's type mapping,
// and the right move is to update all three together — not to silence the
// assert.

// ─── Scalar widths ────────────────────────────────────────────────────────
// The ABI's width names must mean what they say. A future change to a
// platform where int32_t is not 32 bits is out of scope; Lucid targets
// platforms where these holds.

static_assert(sizeof(LucidI1)  == 1,  "LucidI1 must be 1 byte");
static_assert(sizeof(LucidI8)  == 1,  "LucidI8 must be 1 byte");
static_assert(sizeof(LucidI32) == 4,  "LucidI32 must be 4 bytes");
static_assert(sizeof(LucidI64) == 8,  "LucidI64 must be 8 bytes");
static_assert(sizeof(LucidF64) == 8,  "LucidF64 must be 8 bytes");
static_assert(sizeof(LucidPtr) == sizeof(void*), "LucidPtr must be pointer-sized");

static_assert(sizeof(LucidBool) == 1, "LucidBool must be 1 byte");

// Signedness. These are belt-and-braces: int8_t and int32_t are signed by
// definition, but the asserts document the intent and catch a future
// typedef swap.

static_assert(std::is_signed_v<LucidI8>,  "LucidI8 must be signed");
static_assert(std::is_signed_v<LucidI32>, "LucidI32 must be signed");
static_assert(std::is_signed_v<LucidI64>, "LucidI64 must be signed");
static_assert(std::is_unsigned_v<LucidBool>, "LucidBool must be unsigned");
static_assert(std::is_same_v<LucidF64, double>, "LucidF64 must be double");

// ─── Struct sizes ─────────────────────────────────────────────────────────
// No padding is allowed in any ABI struct. A struct with padding would
// have a different size on a different target, and the layout would no
// longer be a single fact the codegen and the runtime agree on.

static_assert(sizeof(LucidString)          == 24, "LucidString must be { ptr, i64, i64 } with no padding");
static_assert(sizeof(LucidSlice)           == 24, "LucidSlice must be { ptr, i64, i64 } with no padding");
static_assert(sizeof(LucidArena)           == 24, "LucidArena must be { ptr, i64, i64 } with no padding");
static_assert(sizeof(LucidArenaDescriptor) == 16, "LucidArenaDescriptor must be { ptr, i64 } with no padding");
static_assert(sizeof(LucidClosureHeader)   == 16, "LucidClosureHeader must be { i64, ptr } with no padding");
static_assert(sizeof(LucidClosure)         == 16, "LucidClosure must be { ptr, ptr } with no padding");

// ─── Struct field offsets ─────────────────────────────────────────────────
// Size alone is not enough: a struct could be the right size with the
// fields in the wrong order. The offsets pin the order.

static_assert(offsetof(LucidString, data)          == 0,  "LucidString.data must be at offset 0");
static_assert(offsetof(LucidString, len)           == 8,  "LucidString.len must be at offset 8");
static_assert(offsetof(LucidString, cap)           == 16, "LucidString.cap must be at offset 16");

static_assert(offsetof(LucidSlice, data)           == 0,  "LucidSlice.data must be at offset 0");
static_assert(offsetof(LucidSlice, len)            == 8,  "LucidSlice.len must be at offset 8");
static_assert(offsetof(LucidSlice, cap)            == 16, "LucidSlice.cap must be at offset 16");

static_assert(offsetof(LucidArena, base)           == 0,  "LucidArena.base must be at offset 0");
static_assert(offsetof(LucidArena, size)           == 8,  "LucidArena.size must be at offset 8");
static_assert(offsetof(LucidArena, cursor)         == 16, "LucidArena.cursor must be at offset 16");

static_assert(offsetof(LucidArenaDescriptor, base) == 0,  "LucidArenaDescriptor.base must be at offset 0");
static_assert(offsetof(LucidArenaDescriptor, size) == 8,  "LucidArenaDescriptor.size must be at offset 8");

static_assert(offsetof(LucidClosureHeader, refcount) == 0, "LucidClosureHeader.refcount must be at offset 0");
static_assert(offsetof(LucidClosureHeader, drop)     == 8, "LucidClosureHeader.drop must be at offset 8");

static_assert(offsetof(LucidClosure, fn)  == 0, "LucidClosure.fn must be at offset 0");
static_assert(offsetof(LucidClosure, env) == 8, "LucidClosure.env must be at offset 8");

// ─── Alignment ────────────────────────────────────────────────────────────
// The alignment of a struct is the maximum alignment of its fields. For
// all of these, that is pointer alignment. The asserts document it so
// that a future field addition with a stricter alignment requirement
// (a 16-byte SIMD field, say) fails here rather than silently changing
// the layout on one target.

static_assert(alignof(LucidString)          == alignof(void*), "LucidString alignment must be pointer alignment");
static_assert(alignof(LucidSlice)           == alignof(void*), "LucidSlice alignment must be pointer alignment");
static_assert(alignof(LucidArena)           == alignof(void*), "LucidArena alignment must be pointer alignment");
static_assert(alignof(LucidArenaDescriptor) == alignof(void*), "LucidArenaDescriptor alignment must be pointer alignment");
static_assert(alignof(LucidClosureHeader)   == alignof(void*), "LucidClosureHeader alignment must be pointer alignment");
static_assert(alignof(LucidClosure)         == alignof(void*), "LucidClosure alignment must be pointer alignment");

// ─── Triviality ───────────────────────────────────────────────────────────
// These structs are passed by value across the ABI. They must be trivially
// copyable and trivially destructible; a non-trivial copy or destructor
// would change the calling convention in ways LLVM's generated code does
// not model.

static_assert(std::is_trivially_copyable_v<LucidString>,          "LucidString must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LucidSlice>,           "LucidSlice must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LucidArena>,           "LucidArena must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LucidArenaDescriptor>, "LucidArenaDescriptor must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LucidClosureHeader>,   "LucidClosureHeader must be trivially copyable");
static_assert(std::is_trivially_copyable_v<LucidClosure>,         "LucidClosure must be trivially copyable");

static_assert(std::is_trivially_destructible_v<LucidString>,          "LucidString must be trivially destructible");
static_assert(std::is_trivially_destructible_v<LucidSlice>,           "LucidSlice must be trivially destructible");
static_assert(std::is_trivially_destructible_v<LucidArena>,           "LucidArena must be trivially destructible");
static_assert(std::is_trivially_destructible_v<LucidArenaDescriptor>, "LucidArenaDescriptor must be trivially destructible");
static_assert(std::is_trivially_destructible_v<LucidClosureHeader>,   "LucidClosureHeader must be trivially destructible");
static_assert(std::is_trivially_destructible_v<LucidClosure>,         "LucidClosure must be trivially destructible");

// ─────────────────────────────────────────────────────────────────────────────
// Type-tag mapping
// ─────────────────────────────────────────────────────────────────────────────
//
// functions.def's rows use short tags (I1, I8, I32, I64, F64, Ptr, Str,
// Arena, Desc, Void) rather than the full C++ type names, because the rows
// are read by three consumers and the tag is the vocabulary they share.
// This mapping — tag to C++ type — is the agreement between the tag and
// the named type above.
//
// There is no static_assert for the mapping itself, because the mapping
// is a convention enforced by the code generator that expands the table.
// The asserts above pin the C++ types; the table's rows pin the tags; the
// code generator that expands the table is what connects them, and a
// mismatch there is a compile error in runtime/exports.cpp.
//
// The tags, for reference:
//
//   Void   — no value (return position only)
//   I1     — LucidBool, the ABI's boolean; not C++ bool
//   I8     — LucidI8
//   I32    — LucidI32
//   I64    — LucidI64
//   F64    — LucidF64
//   Ptr    — LucidPtr, an opaque or out-pointer
//   Str    — LucidString, by value
//   Slice  — LucidSlice, by value
//   Arena  — LucidArena, by value
//   Desc   — LucidArenaDescriptor, by value

} // namespace lucid::abi