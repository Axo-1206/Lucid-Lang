/**
 * @file AttributeRegistry.hpp
 * @brief Singleton registry for all built‑in compiler attributes.
 *
 * Provides metadata about each attribute: where it can appear, what arguments
 * it accepts, and validation rules. The registry is initialised once when
 * the StringPool is set, and then used read‑only.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <string>
#include <unordered_map>
#include <cstdint>
#include <functional>

struct AttributeAST;
class DiagnosticEngine;

// ─────────────────────────────────────────────────────────────────────────────
// AttributeContext – where an attribute may appear (bitmask)
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
    Package = 1 << 8,
    TypeAlias = 1 << 9,
};

inline AttributeContext operator|(AttributeContext a, AttributeContext b) {
    return static_cast<AttributeContext>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline AttributeContext operator&(AttributeContext a, AttributeContext b) {
    return static_cast<AttributeContext>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(AttributeContext value, AttributeContext flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AttrArgKind – allowed argument types for an attribute (bitmask)
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
// ValidationContext – passed to custom validators
// ─────────────────────────────────────────────────────────────────────────────
struct AttributeValidationContext {
    const std::string& declName;
    DeclKeyword declKeyword;
    DiagnosticEngine& dc;
    const SourceLocation& loc;
    
    AttributeValidationContext(const std::string& name, DeclKeyword kw,
                               DiagnosticEngine& engine, const SourceLocation& location)
        : declName(name), declKeyword(kw), dc(engine), loc(location) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeInfo – metadata about one built‑in attribute
// ─────────────────────────────────────────────────────────────────────────────
struct AttributeInfo {
    InternedString id;                        // interned ID (key for lookup)
    InternedString name;                      // attribute name (same as id, stored separately for convenience)
    AttributeContext validContexts;           // where this attribute may appear
    bool takesArgs;                           // does it accept arguments?
    int minArgs;                              // minimum argument count (0 if takesArgs == false)
    int maxArgs;                              // maximum argument count (-1 = unlimited)
    AttrArgKind allowedArgKinds;              // what argument types are allowed
    bool requiresConst;                       // must be on a 'const' declaration
    InternedString exclusiveWith;             // ID of mutually exclusive attribute (or invalid)
    DiagCode errorCode;                       // diagnostic code for this attribute's errors

    // Custom validator (optional)
    using Validator = std::function<bool(const ArenaSpan<AttributeArgPtr>& args,
                                      const AttributeValidationContext& ctx)>;
    Validator validator;
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeRegistry – singleton providing attribute metadata
// ─────────────────────────────────────────────────────────────────────────────
class AttributeRegistry {
public:
    static AttributeRegistry& instance();

    // Must be called once before any lookups (usually by main() after creating StringPool)
    void setStringPool(StringPool& pool);
    
    // Call before destroying the StringPool to avoid dangling references
    void resetStringPool();

    // Lookup by interned ID (fast)
    const AttributeInfo* lookup(InternedString id) const;
    
    // Lookup by string name (slower, use only for parser diagnostics)
    const AttributeInfo* lookup(const std::string& name) const;

    // Quick checks
    bool isValidOn(InternedString id, AttributeContext ctx) const;
    bool isValidOn(const std::string& name, AttributeContext ctx) const;

    // Get comma‑separated list of all attribute names (for error messages)
    std::string allNames() const;

    // Validate a single attribute against its declaration context
    bool validateAttribute(const AttributeAST& attr,
                           AttributeContext ctx,
                           const std::string& declName,
                           DeclKeyword declKw,
                           DiagnosticEngine& dc) const;

    // Check mutual exclusion between two attributes (used when multiple attributes present)
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
    InternedString getAotId() const        { return aotId; }
    InternedString getJitId() const        { return jitId; }

private:
    AttributeRegistry();

    // Helper to build a diagnostic message using the new static message system
    void reportAttrError(DiagnosticEngine& dc, const SourceLocation& loc,
                         DiagCode code, std::initializer_list<std::string> args = {}) const;

    StringPool* stringPool = nullptr;
    std::unordered_map<InternedString, AttributeInfo> byId;
    std::unordered_map<std::string, InternedString> nameToId;

    // Pre‑interned IDs for well‑known attributes
    InternedString externId, packedId, inlineId, noinlineId, deprecatedId, aotId, jitId;
};