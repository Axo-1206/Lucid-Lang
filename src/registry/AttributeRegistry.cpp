#include "AttributeRegistry.hpp"
#include "ast/DeclAST.hpp"
#include "diagnostics/DiagnosticEngine.hpp"

#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Built‑in attribute definitions
//
// Each attribute is defined with its metadata. The registry is initialised
// from this static array when setStringPool() is called.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct BuiltinAttribute {
    std::string_view name;
    AttributeContext contexts;
    bool takesArgs;
    int minArgs;
    int maxArgs;
    AttrArgKind argKinds;
    bool requiresConst;
    std::string_view exclusiveWith;   // empty if none
    DiagCode errorCode;               // primary error code for this attribute
};

// Validator for @extern attribute
bool validateExtern(const ArenaSpan<AttributeArgPtr>& args,
                    const AttributeValidationContext& ctx) {
    if (args.size() >= 2) {
        if (args[1]->kind != AttributeArgKind::StringLit) {
            ctx.dc.error(DiagnosticCategory::Syntax, InternedString(), ctx.loc,
                         DiagCode::E2011, {"second argument to @extern must be a calling convention string"});
            return false;
        }
    }
    return true;
}

// Validator for @deprecated attribute
bool validateDeprecated(const ArenaSpan<AttributeArgPtr>& args,
                        const AttributeValidationContext& ctx) {
    if (args.size() >= 1) {
        if (args[0]->kind != AttributeArgKind::StringLit) {
            ctx.dc.error(DiagnosticCategory::Syntax, InternedString(), ctx.loc,
                         DiagCode::E2011, {"@deprecated argument must be a string literal"});
            return false;
        }
    }
    return true;
}

const BuiltinAttribute kBuiltinAttrs[] = {
    {
        .name = "extern",
        .contexts = AttributeContext::Func | AttributeContext::Var,
        .takesArgs = true,
        .minArgs = 0,
        .maxArgs = 2,
        .argKinds = AttrArgKind::String,
        .requiresConst = true,
        .exclusiveWith = "",
        .errorCode = DiagCode::E2010
    },
    {
        .name = "packed",
        .contexts = AttributeContext::Struct,
        .takesArgs = false,
        .minArgs = 0,
        .maxArgs = 0,
        .argKinds = AttrArgKind::None,
        .requiresConst = false,
        .exclusiveWith = "",
        .errorCode = DiagCode::E2010
    },
    {
        .name = "inline",
        .contexts = AttributeContext::Func,
        .takesArgs = false,
        .minArgs = 0,
        .maxArgs = 0,
        .argKinds = AttrArgKind::None,
        .requiresConst = false,
        .exclusiveWith = "noinline",
        .errorCode = DiagCode::E2010
    },
    {
        .name = "noinline",
        .contexts = AttributeContext::Func,
        .takesArgs = false,
        .minArgs = 0,
        .maxArgs = 0,
        .argKinds = AttrArgKind::None,
        .requiresConst = false,
        .exclusiveWith = "inline",
        .errorCode = DiagCode::E2010
    },
    {
        .name = "deprecated",
        .contexts = AttributeContext::Func | AttributeContext::Var | 
                    AttributeContext::Struct | AttributeContext::Impl | 
                    AttributeContext::Enum | AttributeContext::Trait |
                    AttributeContext::From | AttributeContext::Package |
                    AttributeContext::TypeAlias,
        .takesArgs = true,
        .minArgs = 0,
        .maxArgs = 1,
        .argKinds = AttrArgKind::String,
        .requiresConst = false,
        .exclusiveWith = "",
        .errorCode = DiagCode::E2010
    },
    {
        .name = "aot",
        .contexts = AttributeContext::Main,
        .takesArgs = false,
        .minArgs = 0,
        .maxArgs = 0,
        .argKinds = AttrArgKind::None,
        .requiresConst = false,
        .exclusiveWith = "jit",
        .errorCode = DiagCode::E3015
    },
    {
        .name = "jit",
        .contexts = AttributeContext::Main,
        .takesArgs = false,
        .minArgs = 0,
        .maxArgs = 0,
        .argKinds = AttrArgKind::None,
        .requiresConst = false,
        .exclusiveWith = "aot",
        .errorCode = DiagCode::E3015
    },
};

const size_t kNumBuiltinAttrs = sizeof(kBuiltinAttrs) / sizeof(kBuiltinAttrs[0]);

} // unnamed namespace

AttributeRegistry& AttributeRegistry::instance() {
    static AttributeRegistry registry;
    return registry;
}

AttributeRegistry::AttributeRegistry() = default;

void AttributeRegistry::reportAttrError(DiagnosticEngine& dc, const SourceLocation& loc,
                                        DiagCode code, std::initializer_list<std::string> args) const {
    // Note: file is not set here – the caller should provide it. For now, use empty InternedString.
    dc.error(DiagnosticCategory::Syntax, InternedString(), loc, code, args);
}

void AttributeRegistry::setStringPool(StringPool& pool) {
    stringPool = &pool;

    // Clear existing maps
    byId.clear();
    nameToId.clear();

    for (const auto& builtin : kBuiltinAttrs) {
        InternedString id = pool.intern(std::string(builtin.name));
        
        AttributeInfo info;
        info.id = id;
        info.name = id;
        info.validContexts = builtin.contexts;
        info.takesArgs = builtin.takesArgs;
        info.minArgs = builtin.minArgs;
        info.maxArgs = builtin.maxArgs;
        info.allowedArgKinds = builtin.argKinds;
        info.requiresConst = builtin.requiresConst;
        info.exclusiveWith = builtin.exclusiveWith.empty() ? InternedString() : pool.intern(std::string(builtin.exclusiveWith));
        info.errorCode = builtin.errorCode;

        // Attach validators
        if (builtin.name == "extern") {
            info.validator = validateExtern;
        } else if (builtin.name == "deprecated") {
            info.validator = validateDeprecated;
        }

        byId[id] = info;
        nameToId[std::string(builtin.name)] = id;
    }

    // Pre‑intern well‑known IDs for fast access
    externId     = pool.intern("extern");
    packedId     = pool.intern("packed");
    inlineId     = pool.intern("inline");
    noinlineId   = pool.intern("noinline");
    deprecatedId = pool.intern("deprecated");
    aotId        = pool.intern("aot");
    jitId        = pool.intern("jit");
}

void AttributeRegistry::resetStringPool() {
    stringPool = nullptr;
    byId.clear();
    nameToId.clear();
    externId = InternedString();
    packedId = InternedString();
    inlineId = InternedString();
    noinlineId = InternedString();
    deprecatedId = InternedString();
    aotId = InternedString();
    jitId = InternedString();
}

const AttributeInfo* AttributeRegistry::lookup(InternedString id) const {
    if (!stringPool) return nullptr;
    auto it = byId.find(id);
    return (it != byId.end()) ? &it->second : nullptr;
}

const AttributeInfo* AttributeRegistry::lookup(const std::string& name) const {
    if (!stringPool) return nullptr;
    auto it = nameToId.find(name);
    if (it == nameToId.end()) return nullptr;
    return lookup(it->second);
}

bool AttributeRegistry::isValidOn(InternedString id, AttributeContext ctx) const {
    const AttributeInfo* info = lookup(id);
    return info && hasFlag(info->validContexts, ctx);
}

bool AttributeRegistry::isValidOn(const std::string& name, AttributeContext ctx) const {
    const AttributeInfo* info = lookup(name);
    return info && hasFlag(info->validContexts, ctx);
}

std::string AttributeRegistry::allNames() const {
    std::stringstream ss;
    bool first = true;
    for (const auto& [name, _] : nameToId) {
        if (!first) ss << ", ";
        ss << "@" << name;
        first = false;
    }
    return ss.str();
}

bool AttributeRegistry::validateAttribute(const AttributeAST& attr,
                                          AttributeContext ctx,
                                          const std::string& declName,
                                          DeclKeyword declKw,
                                          DiagnosticEngine& dc) const {
    if (!stringPool) return false;

    const AttributeInfo* info = lookup(attr.name);
    if (!info) {
        std::string_view name = stringPool->lookup(attr.name);
        dc.error(DiagnosticCategory::Syntax, InternedString(), attr.loc,
                 DiagCode::E2010, {std::string(name)});
        return false;
    }

    bool valid = true;

    // Context check
    if (!hasFlag(info->validContexts, ctx)) {
        std::string_view name = stringPool->lookup(info->name);
        dc.error(DiagnosticCategory::Syntax, InternedString(), attr.loc,
                 info->errorCode, {std::string("@") + std::string(name)});
        valid = false;
    }

    // Arg count
    int argCount = static_cast<int>(attr.args.size());
    if (info->takesArgs) {
        if (argCount < info->minArgs) {
            std::string_view name = stringPool->lookup(info->name);
            dc.error(DiagnosticCategory::Syntax, InternedString(), attr.loc,
                     DiagCode::E2011, {std::string("@") + std::string(name)});
            valid = false;
        }
        if (info->maxArgs != -1 && argCount > info->maxArgs) {
            std::string_view name = stringPool->lookup(info->name);
            dc.error(DiagnosticCategory::Syntax, InternedString(), attr.loc,
                     DiagCode::E2011, {std::string("@") + std::string(name)});
            valid = false;
        }
    } else if (argCount > 0) {
        std::string_view name = stringPool->lookup(info->name);
        dc.error(DiagnosticCategory::Syntax, InternedString(), attr.loc,
                 DiagCode::E2011, {std::string("@") + std::string(name)});
        valid = false;
    }

    // Arg kinds
    for (const auto& arg : attr.args) {
        AttrArgKind needed = info->allowedArgKinds;
        bool ok = false;
        if (hasFlag(needed, AttrArgKind::String) && arg->kind == AttributeArgKind::StringLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Int) && arg->kind == AttributeArgKind::IntLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Bool) && arg->kind == AttributeArgKind::BoolLit) ok = true;
        if (hasFlag(needed, AttrArgKind::Type) && arg->kind == AttributeArgKind::TypeIdent) ok = true;
        if (!ok) {
            std::string_view name = stringPool->lookup(info->name);
            dc.error(DiagnosticCategory::Syntax, InternedString(), arg->loc,
                     DiagCode::E2011, {std::string("@") + std::string(name)});
            valid = false;
        }
    }

    // requiresConst
    if (info->requiresConst && declKw != DeclKeyword::Const) {
        std::string_view name = stringPool->lookup(info->name);
        dc.warning(DiagnosticCategory::Semantic, InternedString(), attr.loc,
                   DiagCode::W3001, {std::string("@") + std::string(name)});
        // Not an error, just a warning – still valid to use 'let' but discouraged
    }

    // Custom validator
    if (info->validator) {
        AttributeValidationContext vctx(declName, declKw, dc, attr.loc);
        if (!info->validator(attr.args, vctx)) {
            valid = false;
        }
    }

    return valid;
}

bool AttributeRegistry::checkMutualExclusion(InternedString id1, InternedString id2,
                                             DiagnosticEngine& dc,
                                             const SourceLocation& loc) const {
    const AttributeInfo* a = lookup(id1);
    const AttributeInfo* b = lookup(id2);
    if (!a || !b) return true; // already reported elsewhere

    if (a->exclusiveWith.isValid() && a->exclusiveWith == id2) {
        std::string_view name1 = stringPool->lookup(a->name);
        std::string_view name2 = stringPool->lookup(b->name);
        dc.error(DiagnosticCategory::Syntax, InternedString(), loc,
                 DiagCode::E3015, {std::string("@") + std::string(name1), std::string("@") + std::string(name2)});
        return false;
    }
    if (b->exclusiveWith.isValid() && b->exclusiveWith == id1) {
        std::string_view name1 = stringPool->lookup(a->name);
        std::string_view name2 = stringPool->lookup(b->name);
        dc.error(DiagnosticCategory::Syntax, InternedString(), loc,
                 DiagCode::E3015, {std::string("@") + std::string(name1), std::string("@") + std::string(name2)});
        return false;
    }
    return true;
}

bool AttributeRegistry::checkMutualExclusion(const std::string& name1, const std::string& name2,
                                             DiagnosticEngine& dc,
                                             const SourceLocation& loc) const {
    auto it1 = nameToId.find(name1);
    auto it2 = nameToId.find(name2);
    if (it1 == nameToId.end() || it2 == nameToId.end()) return true;
    return checkMutualExclusion(it1->second, it2->second, dc, loc);
}