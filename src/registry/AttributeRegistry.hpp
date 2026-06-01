/**
 * @file AttributeRegistry.hpp
 * @brief Registry for Luc '@' attributes, following the IntrinsicRegistry design.
 *
 * Provides metadata about each built‑in attribute: where it can appear, what
 * arguments it accepts, and validation rules. Lookups are O(1) using
 * InternedString keys.
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// AttributeContext – where an attribute may appear (bitmask)
// ─────────────────────────────────────────────────────────────────────────────
enum class AttributeContext : uint32_t {
    None        = 0,
    Func        = 1 << 0,
    Var         = 1 << 1,
    Struct      = 1 << 2,
    Impl        = 1 << 3,
    Main        = 1 << 4,
    Enum        = 1 << 5,
    Trait       = 1 << 6,
    From        = 1 << 7,
    Package     = 1 << 8,
    TypeAlias   = 1 << 9,
};

inline AttributeContext operator|(AttributeContext a, AttributeContext b) {
    return static_cast<AttributeContext>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool hasFlag(AttributeContext value, AttributeContext flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AttrArgKind – allowed argument types for an attribute (bitmask)
// ─────────────────────────────────────────────────────────────────────────────
enum class AttrArgKind : uint32_t {
    None    = 0,
    String  = 1 << 0,
    Int     = 1 << 1,
    Bool    = 1 << 2,
    Type    = 1 << 3,
    Any     = String | Int | Bool | Type
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
    InternedString file;
    const SourceLocation& loc;
    const std::string& declName;
    DeclKeyword declKeyword;

    AttributeValidationContext(InternedString f, const SourceLocation& l,
                               const std::string& name, DeclKeyword kw)
        : file(f), loc(l), declName(name), declKeyword(kw) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeEntry – metadata for a single built‑in attribute
// ─────────────────────────────────────────────────────────────────────────────
struct AttributeEntry {
    InternedString id;                              // interned key (filled by initialize)
    std::string_view name;                          // attribute name (e.g., "extern")
    AttributeContext validContexts;                 // where this attribute may appear
    bool takesArgs;                                 // does it accept arguments?
    int minArgs;                                    // minimum argument count
    int maxArgs;                                    // maximum argument count (-1 = unlimited)
    AttrArgKind allowedArgKinds;                    // allowed argument types
    bool requiresConst;                             // must be on a 'const' declaration
    std::string_view exclusiveWithName;             // name of mutually exclusive attribute (or empty)
    InternedString exclusiveWithId;                 // interned version (filled by initialize)
    DiagCode errorCode;                             // primary diagnostic code for this attribute

    // Custom validator (optional)
    using Validator = bool (*)(const ArenaSpan<AttributeArgPtr>& args,
                               const AttributeValidationContext& ctx);
    Validator validator;
};

// ─────────────────────────────────────────────────────────────────────────────
// namespace `attribute` – procedural interface for attribute metadata
// ─────────────────────────────────────────────────────────────────────────────
namespace attribute {

// Must be called once before any lookup (usually by main() after creating StringPool)
void initialize(StringPool& pool);

// Call before destroying the StringPool to avoid dangling references
void shutdown();

// Lookup by interned ID (O(1))
const AttributeEntry* lookup(InternedString id);

// Lookup by name (interning on the fly)
const AttributeEntry* lookup(std::string_view name);

// Get the pre‑interned ID for a known attribute (or invalid ID if unknown)
InternedString getId(std::string_view name);

// Quick existence checks
bool isKnown(InternedString id);
bool isKnown(std::string_view name);

// Returns a comma‑separated list of all attribute names (with '@' prefix, for error messages)
std::string allNames();

// Validate a single attribute call
// Reports errors using the diagnostic module and returns true if valid.
bool validateAttribute(const AttributeEntry& entry,
                       const ArenaSpan<AttributeArgPtr>& args,
                       AttributeContext ctx,
                       const std::string& declName,
                       DeclKeyword declKw,
                       InternedString file,
                       const SourceLocation& loc);

// Check mutual exclusion between two attributes (used when multiple attributes are present)
bool checkMutualExclusion(InternedString id1, InternedString id2,
                          const SourceLocation& loc);
bool checkMutualExclusion(std::string_view name1, std::string_view name2,
                          const SourceLocation& loc);

// Pre‑interned IDs for well‑known attributes (zero‑cost after initialize)
InternedString getExternId();
InternedString getPackedId();
InternedString getInlineId();
InternedString getNoinlineId();
InternedString getDeprecatedId();
InternedString getAotId();
InternedString getJitId();

} // namespace attribute