#pragma once

#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

// Forward declarations
struct AttributeAST;
struct AttributeArgAST;
class DiagnosticEngine;
class StringPool;

// ─────────────────────────────────────────────────────────────────────────────
// AttributeContext - where the attribute can appear
// ─────────────────────────────────────────────────────────────────────────────
enum class AttributeContext : uint32_t {
    None    = 0,
    Func    = 1 << 0,   // Regular function
    Var     = 1 << 1,   // Variable
    Struct  = 1 << 2,   // Struct declaration
    Impl    = 1 << 3,   // Impl block
    Main    = 1 << 4,   // Only the 'main' function (special)
};

inline AttributeContext operator|(AttributeContext a, AttributeContext b) {
    return static_cast<AttributeContext>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasFlag(AttributeContext value, AttributeContext flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AttrArgKind - what kind of arguments are allowed
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
// AttributeInfo - metadata about a single attribute
// ─────────────────────────────────────────────────────────────────────────────
struct AttributeInfo {
    std::string name;
    AttributeContext validContexts;
    bool takesArgs;
    int minArgs;
    int maxArgs;                    // -1 = unlimited
    AttrArgKind allowedArgKinds;
    bool requiresConst;             // @extern requires 'const', not 'let'
    std::string exclusiveWith;      // e.g., "aot" excludes "jit"
    
    // Custom validator for complex constraints
    // Returns true if valid, false if error (error already reported via dc)
    bool (*validator)(const std::vector<AttributeArgAST>& args,
                      const std::string& declName, DiagnosticEngine& dc,
                      const SourceLocation& loc, const StringPool& pool) = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeRegistry - singleton registry
// ─────────────────────────────────────────────────────────────────────────────
class AttributeRegistry {
public:
    static AttributeRegistry& instance();
    
    void setStringPool(StringPool* pool);
    
    const AttributeInfo* lookup(const std::string& name) const;
    bool isValidOn(const std::string& name, AttributeContext ctx) const;
    std::string allNames() const;
    
    // Validate an attribute with full context
    bool validateAttribute(const AttributeAST& attr,
                           AttributeContext ctx,
                           const std::string& declName,
                           DeclKeyword declKw,
                           DiagnosticEngine& dc) const;
    
    // Special getters for known attributes
    bool isExternAttribute(const std::string& name) const { return name == "extern"; }
    bool isMainOnlyAttribute(const std::string& name) const;

    bool checkMutualExclusion(const std::string& name1, 
                            const std::string& name2,
                            DiagnosticEngine& dc,
                            const SourceLocation& loc) const;
    
private:
    AttributeRegistry();
    void registerAttribute(const AttributeInfo& info);
    
    std::unordered_map<std::string, AttributeInfo> attributes_;
    StringPool* stringPool;
};