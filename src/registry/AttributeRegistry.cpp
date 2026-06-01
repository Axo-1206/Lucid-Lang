/**
 * @file AttributeRegistry.cpp
 * @brief Implementation of the attribute registry namespace.
 */

#include "AttributeRegistry.hpp"
#include "ast/DeclAST.hpp"
#include "diagnostics/Diagnostic.hpp"
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// Custom validators (static)
// ─────────────────────────────────────────────────────────────────────────────
static bool validateExtern(const ArenaSpan<AttributeArgPtr>& args,
                           const AttributeValidationContext& ctx) {
    if (args.size() >= 2) {
        if (args[1]->kind != AttributeArgKind::StringLit) {
            diagnostic::error(DiagnosticCategory::Syntax, ctx.file, ctx.loc,
                             DiagCode::E3046,
                             {"second argument to @extern must be a calling convention string"});
            return false;
        }
    }
    return true;
}

static bool validateDeprecated(const ArenaSpan<AttributeArgPtr>& args,
                               const AttributeValidationContext& ctx) {
    if (args.size() >= 1) {
        if (args[0]->kind != AttributeArgKind::StringLit) {
            diagnostic::error(DiagnosticCategory::Syntax, ctx.file, ctx.loc,
                             DiagCode::E3046,
                             {"@deprecated argument must be a string literal"});
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// kAttributes – static table of all built‑in attributes
// ─────────────────────────────────────────────────────────────────────────────
static AttributeEntry kAttributes[] = {
    {
        InternedString(),
        "extern",
        AttributeContext::Func | AttributeContext::Var,
        true, 0, 2, AttrArgKind::String, true,
        "",
        InternedString(),
        DiagCode::E2010,
        validateExtern
    },
    {
        InternedString(),
        "packed",
        AttributeContext::Struct,
        false, 0, 0, AttrArgKind::None, false,
        "",
        InternedString(),
        DiagCode::E2010,
        nullptr
    },
    {
        InternedString(),
        "inline",
        AttributeContext::Func,
        false, 0, 0, AttrArgKind::None, false,
        "noinline",
        InternedString(),
        DiagCode::E2010,
        nullptr
    },
    {
        InternedString(),
        "noinline",
        AttributeContext::Func,
        false, 0, 0, AttrArgKind::None, false,
        "inline",
        InternedString(),
        DiagCode::E2010,
        nullptr
    },
    {
        InternedString(),
        "deprecated",
        AttributeContext::Func | AttributeContext::Var |
        AttributeContext::Struct | AttributeContext::Impl |
        AttributeContext::Enum | AttributeContext::Trait |
        AttributeContext::From | AttributeContext::Package |
        AttributeContext::TypeAlias,
        true, 0, 1, AttrArgKind::String, false,
        "",
        InternedString(),
        DiagCode::E2010,
        validateDeprecated
    },
    {
        InternedString(),
        "link",
        AttributeContext::Package,      // top‑level file directive
        true, 1, -1, AttrArgKind::String, false,
        "",
        InternedString(),
        DiagCode::E2010,
        nullptr
    },
    {
        InternedString(),
        "phantom",
        AttributeContext::TypeAlias | AttributeContext::Struct | AttributeContext::Func,
        false, 0, 0, AttrArgKind::None, false,
        "",
        InternedString(),
        DiagCode::E2010,
        nullptr
    },
    {
        InternedString(),
        "aot",
        AttributeContext::Main,
        false, 0, 0, AttrArgKind::None, false,
        "jit",
        InternedString(),
        DiagCode::E3015,
        nullptr
    },
    {
        InternedString(),
        "jit",
        AttributeContext::Main,
        false, 0, 0, AttrArgKind::None, false,
        "aot",
        InternedString(),
        DiagCode::E3015,
        nullptr
    },
};

static const size_t kAttributeCount = sizeof(kAttributes) / sizeof(kAttributes[0]);

// ─────────────────────────────────────────────────────────────────────────────
// Static state
// ─────────────────────────────────────────────────────────────────────────────
static StringPool* stringPool = nullptr;
static std::unordered_map<InternedString, const AttributeEntry*> idToEntry;

// Pre‑interned IDs for well‑known attributes
static InternedString externId;
static InternedString packedId;
static InternedString inlineId;
static InternedString noinlineId;
static InternedString deprecatedId;
static InternedString linkPath;
static InternedString aotId;
static InternedString jitId;

// ─────────────────────────────────────────────────────────────────────────────
// Implementation of namespace functions
// ─────────────────────────────────────────────────────────────────────────────
namespace attribute {

void initialize(StringPool& pool) {
    stringPool = &pool;
    idToEntry.clear();

    for (size_t i = 0; i < kAttributeCount; ++i) {
        auto& entry = kAttributes[i];
        entry.id = pool.intern(std::string(entry.name));
        if (!entry.exclusiveWithName.empty()) {
            entry.exclusiveWithId = pool.intern(std::string(entry.exclusiveWithName));
        }
        idToEntry[entry.id] = &entry;
    }

    // Pre‑intern well‑known IDs
    externId     = pool.intern("extern");
    packedId     = pool.intern("packed");
    inlineId     = pool.intern("inline");
    noinlineId   = pool.intern("noinline");
    deprecatedId = pool.intern("deprecated");
    linkPath     = pool.intern("link");
    aotId        = pool.intern("aot");
    jitId        = pool.intern("jit");
}

void shutdown() {
    stringPool = nullptr;
    idToEntry.clear();
    externId = packedId = inlineId = noinlineId = deprecatedId = linkPath = aotId = jitId = InternedString();
}

const AttributeEntry* lookup(InternedString id) {
    if (!stringPool) return nullptr;
    auto it = idToEntry.find(id);
    return (it != idToEntry.end()) ? it->second : nullptr;
}

const AttributeEntry* lookup(std::string_view name) {
    if (!stringPool) return nullptr;
    return lookup(stringPool->intern(std::string(name)));
}

InternedString getId(std::string_view name) {
    const AttributeEntry* entry = lookup(name);
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
    for (size_t i = 0; i < kAttributeCount; ++i) {
        if (i) result += ", ";
        result += "@";
        result += kAttributes[i].name;
    }
    return result;
}

bool validateAttribute(const AttributeEntry& entry,
                       const ArenaSpan<AttributeArgPtr>& args,
                       AttributeContext ctx,
                       const std::string& declName,
                       DeclKeyword declKw,
                       InternedString file,
                       const SourceLocation& loc) {
    bool valid = true;

    // 1) Context check
    if (!hasFlag(entry.validContexts, ctx)) {
        diagnostic::error(DiagnosticCategory::Syntax, file, loc,
                         entry.errorCode,
                         {std::string("@") + std::string(entry.name)});
        valid = false;
    }

    // 2) Argument count
    int argCount = static_cast<int>(args.size());
    if (entry.takesArgs) {
        if (argCount < entry.minArgs) {
            diagnostic::error(DiagnosticCategory::Syntax, file, loc,
                             DiagCode::E3046,
                             {std::string("@") + std::string(entry.name)});
            valid = false;
        }
        if (entry.maxArgs != -1 && argCount > entry.maxArgs) {
            diagnostic::error(DiagnosticCategory::Syntax, file, loc,
                             DiagCode::E3046,
                             {std::string("@") + std::string(entry.name)});
            valid = false;
        }
    } else if (argCount > 0) {
        diagnostic::error(DiagnosticCategory::Syntax, file, loc,
                         DiagCode::E3046,
                         {std::string("@") + std::string(entry.name)});
        valid = false;
    }

    // 3) Argument kinds
    for (const auto& arg : args) {
        AttrArgKind needed = entry.allowedArgKinds;
        bool ok = false;
        if (hasFlag(needed, AttrArgKind::String) && arg->kind == AttributeArgKind::StringLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Int) && arg->kind == AttributeArgKind::IntLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Bool) && arg->kind == AttributeArgKind::BoolLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Type) && arg->kind == AttributeArgKind::TypeIdent) ok = true;
        if (!ok) {
            diagnostic::error(DiagnosticCategory::Syntax, file, arg->loc,
                             DiagCode::E3046,
                             {std::string("@") + std::string(entry.name)});
            valid = false;
        }
    }

    // 4) requiresConst (warning only)
    if (entry.requiresConst && declKw != DeclKeyword::Const) {
        diagnostic::warning(DiagnosticCategory::Semantic, file, loc,
                           DiagCode::W3001,
                           {std::string("@") + std::string(entry.name)});
    }

    // 5) Custom validator
    if (entry.validator) {
        AttributeValidationContext vctx(file, loc, declName, declKw);
        if (!entry.validator(args, vctx)) {
            valid = false;
        }
    }

    return valid;
}

bool checkMutualExclusion(InternedString id1, InternedString id2,
                          const SourceLocation& loc) {
    const AttributeEntry* a = lookup(id1);
    const AttributeEntry* b = lookup(id2);
    if (!a || !b) return true;

    if (a->exclusiveWithId.isValid() && a->exclusiveWithId == id2) {
        diagnostic::error(DiagnosticCategory::Syntax, InternedString(), loc,
                         DiagCode::E3015,
                         {std::string("@") + std::string(a->name),
                          std::string("@") + std::string(b->name)});
        return false;
    }
    if (b->exclusiveWithId.isValid() && b->exclusiveWithId == id1) {
        diagnostic::error(DiagnosticCategory::Syntax, InternedString(), loc,
                         DiagCode::E3015,
                         {std::string("@") + std::string(a->name),
                          std::string("@") + std::string(b->name)});
        return false;
    }
    return true;
}

bool checkMutualExclusion(std::string_view name1, std::string_view name2,
                          const SourceLocation& loc) {
    InternedString id1 = stringPool ? stringPool->intern(std::string(name1)) : InternedString();
    InternedString id2 = stringPool ? stringPool->intern(std::string(name2)) : InternedString();
    if (!id1.isValid() || !id2.isValid()) return true;
    return checkMutualExclusion(id1, id2, loc);
}

InternedString getExternId()     { return externId; }
InternedString getPackedId()     { return packedId; }
InternedString getInlineId()     { return inlineId; }
InternedString getNoinlineId()   { return noinlineId; }
InternedString getDeprecatedId() { return deprecatedId; }
InternedString getlinkPath()     { return linkPath; }
InternedString getAotId()        { return aotId; }
InternedString getJitId()        { return jitId; }

} // namespace attribute