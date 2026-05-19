/**
 * @class AttributeRegistry
 * @brief Singleton holding metadata for all built‑in attributes.
 *
 * If you need to add a new attribute, modify the constructor in
 * AttributeRegistry.cpp – do not rely on external registration calls.
 */
#pragma once

#include "ast/DeclAST.hpp"
#include "ast/support/StringPool.hpp"

#include <string>
#include <unordered_map>
#include <cstdint>

struct AttributeAST;
class DiagnosticEngine;

// ─────────────────────────────────────────────────────────────────────────────
// AttributeContext – where an attribute may appear
// ─────────────────────────────────────────────────────────────────────────────
enum class AttributeContext : uint32_t {
    None    = 0,
    Func    = 1 << 0,
    Var     = 1 << 1,
    Struct  = 1 << 2,
    Impl    = 1 << 3,
    Main    = 1 << 4,
    Enum    = 1 << 5,
    Trait   = 1 << 6,
    From    = 1 << 7,
};

inline AttributeContext operator|(AttributeContext a, AttributeContext b) {
    return static_cast<AttributeContext>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool hasFlag(AttributeContext value, AttributeContext flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AttrArgKind – allowed argument types for an attribute
// ─────────────────────────────────────────────────────────────────────────────
enum class AttrArgKind : uint32_t {
    None     = 0,
    String   = 1 << 0,
    Int      = 1 << 1,
    Bool     = 1 << 2,
    Type     = 1 << 3,
    Any      = String | Int | Bool | Type
};

inline AttrArgKind operator|(AttrArgKind a, AttrArgKind b) {
    return static_cast<AttrArgKind>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool hasFlag(AttrArgKind value, AttrArgKind flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AttributeInfo – metadata about one attribute
// ─────────────────────────────────────────────────────────────────────────────
struct AttributeInfo {
    InternedString id;                        // interned ID (key)
    InternedString name;                     
    AttributeContext validContexts;
    bool takesArgs;
    int minArgs;
    int maxArgs;                              // -1 = unlimited
    AttrArgKind allowedArgKinds;
    bool requiresConst;                       // @extern requires 'const'
    InternedString exclusiveWith;             // ID of mutually exclusive attribute, or invalid

    // Custom validator (optional)
    bool (*validator)(const std::vector<ASTPtr<AttributeArgAST>>& args,
                      const std::string& declName, DiagnosticEngine& dc,
                      const SourceLocation& loc) = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeRegistry – singleton providing attribute metadata
// ─────────────────────────────────────────────────────────────────────────────
class AttributeRegistry {
public:
    static AttributeRegistry& instance();

    void setStringPool(StringPool& pool);   // must be called once before any lookups
    void resetStringPool();                 // Call this when the StringPool is about to be destroyed.

    const AttributeInfo* lookup(InternedString id) const;
    const AttributeInfo* lookup(const std::string& name) const;

    bool isValidOn(InternedString id, AttributeContext ctx) const;
    bool isValidOn(const std::string& name, AttributeContext ctx) const;

    std::string allNames() const;

    bool validateAttribute(const AttributeAST& attr,
                           AttributeContext ctx,
                           const std::string& declName,
                           DeclKeyword declKw,
                           DiagnosticEngine& dc) const;

    bool isExternAttribute(InternedString id) const;
    bool isExternAttribute(const std::string& name) const;
    bool isMainOnlyAttribute(InternedString id) const;
    bool isMainOnlyAttribute(const std::string& name) const;

    bool checkMutualExclusion(InternedString id1, InternedString id2,
                              DiagnosticEngine& dc, const SourceLocation& loc) const;
    bool checkMutualExclusion(const std::string& name1, const std::string& name2,
                              DiagnosticEngine& dc, const SourceLocation& loc) const;

    // Pre‑interned well‑known IDs (zero‑cost after setStringPool)
    InternedString getExternId() const     { return externId; }
    InternedString getPackedId() const     { return packedId; }
    InternedString getInlineId() const     { return inlineId; }
    InternedString getNoinlineId() const   { return noinlineId; }
    InternedString getDeprecatedId() const { return deprecatedId; }

private:
    AttributeRegistry();

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, AttributeInfo> byId;
    std::unordered_map<std::string, InternedString> nameToId; // for lookup by string

    // Pre‑interned IDs for well‑known attributes
    InternedString externId, packedId, inlineId, noinlineId, deprecatedId;
};