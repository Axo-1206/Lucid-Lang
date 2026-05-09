/**
 * @file IntrinsicRegistry.hpp
 *
 * @brief Singleton registry for Luc '@' intrinsics, using InternedString keys.
 */

#pragma once

#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include <string>
#include <vector>
#include <unordered_map>

enum class IntrinsicArgKind {
    TypeArg, AnyValue, IntValue, FloatValue, PtrValue, SizeValue,
};

enum class IntrinsicReturnKind {
    Void, Uint64, Float32, Float64, SameAsArg0, SameAsArg1, RefOfTypeArg0, Int64,
};

struct IntrinsicEntry {
    InternedString id;                       // pre‑interned key (filled by setStringPool)
    const char* lucName;
    const char* llvmID;
    std::vector<IntrinsicArgKind> argKinds;
    IntrinsicReturnKind returnKind;
    bool isOverloaded;
    int minArgs;
    int maxArgs;
    const char* notes;
};

class IntrinsicRegistry {
public:
    static IntrinsicRegistry& instance();

    // Must be called once before any lookups (e.g., in main() or parser constructor)
    void setStringPool(StringPool& pool);

    // Lookup by interned ID (O(1))
    const IntrinsicEntry* lookup(InternedString id) const;

    // Lookup by name (interning on the fly, O(1) due to existing interning)
    const IntrinsicEntry* lookup(const std::string& name) const;

    // Get the pre‑interned ID for a known intrinsic (or 0 if unknown)
    InternedString getId(const std::string& name) const;

    bool isKnown(InternedString id) const;
    bool isKnown(const std::string& name) const;
    std::string allNames() const;

    // Convenience: pre‑interned IDs for the most common intrinsics
    InternedString getSizeofId() const   { return sizeofId; }
    InternedString getAlignofId() const  { return alignofId; }
    // (No need for individual getters for all; the parser can use lookup or getId)

private:
    IntrinsicRegistry();
    void buildMap();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, const IntrinsicEntry*> idToEntry;
    InternedString sizeofId, alignofId;

    static const IntrinsicEntry kEntries[];
    static const std::size_t kEntryCount;
};