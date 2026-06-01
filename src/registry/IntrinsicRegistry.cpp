/**
 * @file IntrinsicRegistry.cpp
 * @brief Implementation of the intrinsic registry namespace.
 */

#include "IntrinsicRegistry.hpp"
#include "diagnostics/Diagnostic.hpp"
#include <array>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// kEntries – static table of all built‑in intrinsics
// ─────────────────────────────────────────────────────────────────────────────
static IntrinsicEntry kEntries[] = {
    // ════════════════════════════════════════════════════════════════════════
    // Compile‑time type queries (folded at IR level, no runtime LLVM intrinsic)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "sizeof",
        "llvm.none",
        { IntrinsicArgKind::TypeArg },
        IntrinsicReturnKind::Uint64,
        false, 0, 0,
        "#sizeof(T) — compile‑time byte size of type T"
    },
    {
        InternedString(),
        "alignof",
        "llvm.none",
        { IntrinsicArgKind::TypeArg },
        IntrinsicReturnKind::Uint64,
        false, 0, 0,
        "#alignof(T) — compile‑time alignment requirement of type T in bytes"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Floating‑point math (LLVM intrinsics, overloaded on f32 / f64)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "sqrt", "llvm.sqrt",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#sqrt(x) — hardware‑accelerated square root; x must be float or double"
    },
    {
        InternedString(),
        "abs", "llvm.abs",
        { IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#abs(x) — absolute value; works on integers and floats"
    },
    {
        InternedString(),
        "min", "llvm.minnum",
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 2, 2,
        "#min(a, b) — minimum of two values; both must be the same type"
    },
    {
        InternedString(),
        "max", "llvm.maxnum",
        { IntrinsicArgKind::AnyValue, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 2, 2,
        "#max(a, b) — maximum of two values; both must be the same type"
    },
    {
        InternedString(),
        "floor", "llvm.floor",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#floor(x) — round x down to nearest integer (as float)"
    },
    {
        InternedString(),
        "ceil", "llvm.ceil",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#ceil(x) — round x up to nearest integer (as float)"
    },
    {
        InternedString(),
        "round", "llvm.round",
        { IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#round(x) — round x to nearest integer, halfway away from zero"
    },
    {
        InternedString(),
        "pow", "llvm.pow",
        { IntrinsicArgKind::FloatValue, IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 2, 2,
        "#pow(base, exp) — base raised to exp; both float/double"
    },
    {
        InternedString(),
        "fma", "llvm.fma",
        { IntrinsicArgKind::FloatValue, IntrinsicArgKind::FloatValue, IntrinsicArgKind::FloatValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 3, 3,
        "#fma(a, b, c) — fused multiply‑add: (a * b) + c, single rounding"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Bit manipulation (LLVM intrinsics, overloaded on integer width)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "clz", "llvm.ctlz",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#clz(x) — count leading zero bits in x; x must be an integer type"
    },
    {
        InternedString(),
        "ctz", "llvm.cttz",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#ctz(x) — count trailing zero bits in x"
    },
    {
        InternedString(),
        "popcount", "llvm.ctpop",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#popcount(x) — number of set (1) bits in x"
    },
    {
        InternedString(),
        "bswap", "llvm.bswap",
        { IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        true, 1, 1,
        "#bswap(x) — reverse byte order of x (endianness conversion)"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Memory operations (LLVM intrinsics)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "memcpy", "llvm.memcpy",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::PtrValue, IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true, 3, 3,
        "#memcpy(dest, src, len) — copy len bytes; regions must not overlap"
    },
    {
        InternedString(),
        "memmove", "llvm.memmove",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::PtrValue, IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true, 3, 3,
        "#memmove(dest, src, len) — copy len bytes; handles overlapping regions"
    },
    {
        InternedString(),
        "memset", "llvm.memset",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::IntValue, IntrinsicArgKind::SizeValue },
        IntrinsicReturnKind::Void,
        true, 3, 3,
        "#memset(dest, value, len) — fill len bytes with byte value"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Vulkan / GPU helpers (LLVM bitcast)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "bitcast", "llvm.bitcast",
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::AnyValue },
        IntrinsicReturnKind::SameAsArg1,
        false, 1, 1,
        "#bitcast(T, x) — reinterpret bits of x as type T; sizes must match"
    },

    // ════════════════════════════════════════════════════════════════════════
    // Pointer operations (Sealed Conduit model – handled directly in codegen)
    // ════════════════════════════════════════════════════════════════════════
    {
        InternedString(),
        "ptrToRef", "llvm.none",
        { IntrinsicArgKind::TypeArg, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::RefOfTypeArg0,
        false, 1, 1,
        "#ptrToRef(T, ptr) — convert raw pointer *T to safe reference &T"
    },
    {
        InternedString(),
        "refToPtr", "llvm.none",
        { IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::SameAsArg0,
        false, 1, 1,
        "#refToPtr(ref) — convert safe reference &T to raw pointer *T"
    },
    {
        InternedString(),
        "ptrOffset", "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::IntValue },
        IntrinsicReturnKind::SameAsArg0,
        false, 2, 2,
        "#ptrOffset(ptr, n) — pointer arithmetic: returns ptr + n"
    },
    {
        InternedString(),
        "ptrDiff", "llvm.none",
        { IntrinsicArgKind::PtrValue, IntrinsicArgKind::PtrValue },
        IntrinsicReturnKind::Int64,
        false, 2, 2,
        "#ptrDiff(p1, p2) — distance between two pointers (in elements)"
    },
};

static const size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

static StringPool* stringPool = nullptr;
static std::unordered_map<InternedString, const IntrinsicEntry*> idToEntry;
static InternedString sizeofId;
static InternedString alignofId;

namespace intrinsic {

void initialize(StringPool& pool) {
    stringPool = &pool;
    idToEntry.clear();

    for (size_t i = 0; i < kEntryCount; ++i) {
        auto& entry = kEntries[i];
        entry.id = pool.intern(std::string(entry.lucName));
        idToEntry[entry.id] = &entry;
    }
    sizeofId  = pool.intern("sizeof");
    alignofId = pool.intern("alignof");
}

void shutdown() {
    stringPool = nullptr;
    idToEntry.clear();
    sizeofId = InternedString();
    alignofId = InternedString();
}

const IntrinsicEntry* lookup(InternedString id) {
    if (!stringPool) return nullptr;
    auto it = idToEntry.find(id);
    return (it != idToEntry.end()) ? it->second : nullptr;
}

const IntrinsicEntry* lookup(std::string_view name) {
    if (!stringPool) return nullptr;
    return lookup(stringPool->intern(std::string(name)));
}

InternedString getId(std::string_view name) {
    const IntrinsicEntry* entry = lookup(name);
    return entry ? entry->id : InternedString();
}

bool isKnown(InternedString id) {
    return lookup(id) != nullptr;
}

bool isKnown(std::string_view name) {
    return lookup(name) != nullptr;
}

std::string allNames() {
    std::string result;
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (i) result += ", ";
        result += kEntries[i].lucName;
    }
    return result;
}

bool validateCall(const IntrinsicEntry& entry,
                  size_t numValueArgs,
                  InternedString file,
                  const SourceLocation& loc) {
    // Check argument count
    if (numValueArgs < static_cast<size_t>(entry.minArgs)) {
        diagnostic::error(DiagnosticCategory::Semantic, file, loc,
                         DiagCode::E3010, {"too few arguments"});
        return false;
    }
    if (entry.maxArgs != -1 && numValueArgs > static_cast<size_t>(entry.maxArgs)) {
        diagnostic::error(DiagnosticCategory::Semantic, file, loc,
                         DiagCode::E3010, {"too many arguments"});
        return false;
    }
    return true;
}

InternedString getSizeofId() { return sizeofId; }
InternedString getAlignofId() { return alignofId; }

} // namespace intrinsic