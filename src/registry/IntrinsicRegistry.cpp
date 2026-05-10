/**
 * @file IntrinsicRegistry.cpp
 *
 * @brief Implementation of the IntrinsicRegistry singleton.
 *
 * This file contains:
 *   - The static table of all Luc '#' intrinsics (kEntries)
 *   - Initialisation of the table with interned string IDs
 *   - O(1) lookup functions using InternedString keys
 *
 * @see IntrinsicRegistry.hpp
 */

#include "IntrinsicRegistry.hpp"
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// IntrinsicRegistry::kEntries
//
// This table is intentionally *not* const because its `id` field is written
// once during setStringPool() (when the StringPool becomes available).
// The table is logically read‑only after initialisation, but the write during
// setup is necessary and well‑defined.
//
// Do NOT add `const` here. If you need a read‑only view, use const references.
// ─────────────────────────────────────────────────────────────────────────────
IntrinsicEntry IntrinsicRegistry::kEntries[] = {

    // ════════════════════════════════════════════════════════════════════════
    // Compile‑time type queries (folded at IR level, no runtime LLVM intrinsic)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),               // id (filled later)
        "sizeof",                       // Luc name: #sizeof(T)
        "llvm.none",                    // not a real LLVM intrinsic
        { IntrinsicArgKind::TypeArg },  // one type argument
        IntrinsicReturnKind::Uint64,    // returns compile-time integer
        false,                          // not overloaded
        0, 0,                           // no value arguments
        "#sizeof(T) — compile-time byte size of type T"
    },
    {
        InternedString(),
        "alignof",
        "llvm.none",
        { IntrinsicArgKind::TypeArg },
        IntrinsicReturnKind::Uint64,
        false,
        0, 0,
        "#alignof(T) — compile-time alignment requirement of type T in bytes"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Floating‑point math (LLVM intrinsics, overloaded on f32 / f64)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "sqrt",
        "llvm.sqrt",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,   // overloaded: .f32 or .f64 suffix
        1, 1,
        "#sqrt(x) — hardware-accelerated square root; x must be float or double"
    },
    {
        InternedString(),
        "abs",
        "llvm.abs",
        { IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#abs(x) — absolute value; works on integers and floats"
    },
    {
        InternedString(),
        "min",
        "llvm.minnum",
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "#min(a, b) — minimum of two values; both must be the same type"
    },
    {
        InternedString(),
        "max",
        "llvm.maxnum",
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "#max(a, b) — maximum of two values; both must be the same type"
    },
    {
        InternedString(),
        "floor",
        "llvm.floor",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#floor(x) — round x down to the nearest integer (as float)"
    },
    {
        InternedString(),
        "ceil",
        "llvm.ceil",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#ceil(x) — round x up to the nearest integer (as float)"
    },
    {
        InternedString(),
        "round",
        "llvm.round",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#round(x) — round x to the nearest integer, halfway away from zero"
    },
    {
        InternedString(),
        "pow",
        "llvm.pow",
        { IntrinsicArgKind::FloatValue, IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        2, 2,
        "#pow(base, exp) — base raised to exp; both float/double"
    },
    {
        InternedString(),
        "fma",
        "llvm.fma",
        { IntrinsicArgKind::FloatValue,
          IntrinsicArgKind::FloatValue,
          IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        3, 3,
        "#fma(a, b, c) — fused multiply‑add: (a * b) + c, single rounding"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Bit manipulation (LLVM intrinsics, overloaded on integer width)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "clz",
        "llvm.ctlz",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,   // suffix: .i32, .i64, etc.
        1, 1,
        "#clz(x) — count leading zero bits in x; x must be an integer type"
    },
    {
        InternedString(),
        "ctz",
        "llvm.cttz",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#ctz(x) — count trailing zero bits in x"
    },
    {
        InternedString(),
        "popcount",
        "llvm.ctpop",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#popcount(x) — number of set (1) bits in x"
    },
    {
        InternedString(),
        "bswap",
        "llvm.bswap",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true,
        1, 1,
        "#bswap(x) — reverse byte order of x (endianness conversion)"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Memory operations (LLVM intrinsics)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "memcpy",
        "llvm.memcpy",
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,   // overloaded on pointer address space
        3, 3,
        "#memcpy(dest, src, len) — copy len bytes; regions must not overlap"
    },
    {
        InternedString(),
        "memmove",
        "llvm.memmove",
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,
        3, 3,
        "#memmove(dest, src, len) — copy len bytes; handles overlapping regions"
    },
    {
        InternedString(),
        "memset",
        "llvm.memset",
        { IntrinsicArgKind::PtrValue,
          IntrinsicArgKind::IntValue,   // fill byte (uint8)
          IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true,
        3, 3,
        "#memset(dest, value, len) — fill len bytes with byte value"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Vulkan / GPU helpers (LLVM bitcast)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "bitcast",
        "llvm.bitcast",
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg1,   // result type = typeArg
        false,  // not overloaded – uses direct bitcast
        1, 1,
        "#bitcast(T, x) — reinterpret bits of x as type T; sizes must match"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Pointer operations (Sealed Conduit model – handled directly in codegen)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "ptrToRef",
        "llvm.none",   // no LLVM intrinsic – codegen uses addrspacecast
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::RefOfTypeArg0,
        false,
        1, 1,
        "#ptrToRef(T, ptr) — convert raw pointer *T to safe reference &T"
    },
    {
        InternedString(),
        "refToPtr",
        "llvm.none",
        { IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::SameAsArg0,
        false,
        1, 1,
        "#refToPtr(ref) — convert safe reference &T to raw pointer *T"
    },
    {
        InternedString(),
        "ptrOffset",
        "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        false,
        2, 2,
        "#ptrOffset(ptr, n) — pointer arithmetic: returns ptr + n"
    },
    {
        InternedString(),
        "ptrDiff",
        "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::Int64,
        false,
        2, 2,
        "#ptrDiff(p1, p2) — distance between two pointers (in elements)"
    },
};

// Number of entries in the table (computed at compile time)
const std::size_t IntrinsicRegistry::kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// ─────────────────────────────────────────────────────────────────────────────
// Singleton instance
// ─────────────────────────────────────────────────────────────────────────────
IntrinsicRegistry& IntrinsicRegistry::instance() {
    static IntrinsicRegistry registry;
    return registry;
}

// Private constructor – does nothing; initialisation happens in setStringPool
IntrinsicRegistry::IntrinsicRegistry() = default;

// ─────────────────────────────────────────────────────────────────────────────
// setStringPool(StringPool&)
//
// Called once per compilation session (e.g., in main() or during parser setup).
// It interns all intrinsic names using the provided StringPool, stores the
// InternedString IDs in each table entry, and builds the O(1) lookup map.
// For well‑known intrinsics (sizeof, alignof), it also stores their IDs
// separately for fast parser access.
// ─────────────────────────────────────────────────────────────────────────────
void IntrinsicRegistry::setStringPool(StringPool& pool) {
    if (stringPool) return; // already initialised
    stringPool = &pool;

    for (std::size_t i = 0; i < kEntryCount; ++i) {
        // const_cast is safe because we are modifying during initialisation only,
        // and no other thread accesses the table before setStringPool completes.
        auto& entry = const_cast<IntrinsicEntry&>(kEntries[i]);
        entry.id = pool.intern(entry.lucName);
        idToEntry[entry.id] = &entry;
    }
    sizeofId  = pool.intern("sizeof");
    alignofId = pool.intern("alignof");
}

void IntrinsicRegistry::resetStringPool() {
    stringPool = nullptr;
    idToEntry.clear();
    sizeofId = InternedString();
    alignofId = InternedString();
}

// ─────────────────────────────────────────────────────────────────────────────
// lookup(InternedString)
//
// O(1) hash map lookup by pre‑interned ID.
// ─────────────────────────────────────────────────────────────────────────────
const IntrinsicEntry* IntrinsicRegistry::lookup(InternedString id) const {
    if (!stringPool) return nullptr;
    auto it = idToEntry.find(id);
    return (it != idToEntry.end()) ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// lookup(const std::string&)
//
// Convenience wrapper – interns the string (if not already interned) and then
// delegates to lookup(InternedString). O(1) amortised.
// ─────────────────────────────────────────────────────────────────────────────
const IntrinsicEntry* IntrinsicRegistry::lookup(const std::string& name) const {
    if (!stringPool) return nullptr; // early return, no assertion
    return lookup(stringPool->intern(name));
}

// ─────────────────────────────────────────────────────────────────────────────
// getId(const std::string&)
//
// Returns the pre‑interned ID for a given intrinsic name, or 0 if unknown.
// Useful for the parser if it wants to compare IDs directly.
// ─────────────────────────────────────────────────────────────────────────────
InternedString IntrinsicRegistry::getId(const std::string& name) const {
    const IntrinsicEntry* entry = lookup(name);
    return entry ? entry->id : InternedString();
}

// ─────────────────────────────────────────────────────────────────────────────
// isKnown(InternedString)
// isKnown(const std::string&)
//
// Quick existence checks.
// ─────────────────────────────────────────────────────────────────────────────
bool IntrinsicRegistry::isKnown(InternedString id) const {
    if (!stringPool) return false;
    return lookup(id) != nullptr;
}

bool IntrinsicRegistry::isKnown(const std::string& name) const {
    if (!stringPool) return false;
    return lookup(name) != nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// allNames()
//
// Returns a comma‑separated list of all intrinsic names for diagnostic messages.
// ─────────────────────────────────────────────────────────────────────────────
std::string IntrinsicRegistry::allNames() const {
    std::string result;
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        if (i) result += ", ";
        result += kEntries[i].lucName;
    }
    return result;
}