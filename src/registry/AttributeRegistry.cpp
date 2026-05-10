#include "AttributeRegistry.hpp"
#include "ast/DeclAST.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <sstream>

AttributeRegistry& AttributeRegistry::instance() {
    static AttributeRegistry registry;
    return registry;
}

// ─────────────────────────────────────────────────────────────────────────
    // Built-in attribute registration
    //
    // The registry is a singleton that must know about every built-in attribute
    // before any source file is parsed. Registration happens here, in the
    // constructor, because:
    //
    //   1. The singleton instance is created on first call to instance().
    //   2. That call typically happens after the StringPool is available,
    //      but before any attribute lookup is performed.
    //   3. The actual string interning is deferred until setStringPool(),
    //      which processes the pending registrations.
    //
    // If you add a new built-in attribute (e.g., @must_use, @cold), register it
    // below using registerAttribute(). Do NOT rely on static initialisation
    // outside this constructor — the `pending` vector is not guaranteed to be
    // usable before main() on all platforms, and we want a single source of
    // truth for the built‑in set.
    //
    // Parameters for registerAttribute():
    //   - name:        string literal of the attribute (without '@')
    //   - contexts:    OR-ed AttributeContext flags where this attribute is allowed
    //   - takesArgs:   true if the attribute accepts arguments
    //   - minArgs:     minimum number of arguments (0 if takesArgs == false)
    //   - maxArgs:     maximum number of arguments (-1 for unlimited)
    //   - argKinds:    OR-ed AttrArgKind flags (String, Int, Bool, Type, Any)
    //   - requiresConst: true if the attribute must be on a 'const' declaration
    //   - exclusiveWith: name of another attribute that cannot be used together
    //   - validator:   optional custom validation function
    // ─────────────────────────────────────────────────────────────────────────
AttributeRegistry::AttributeRegistry() {
}

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
    };

    const BuiltinAttribute kBuiltinAttrs[] = {
        { "extern",   AttributeContext::Func | AttributeContext::Var, true, 0, 1, AttrArgKind::String, false, "" },
        { "packed",   AttributeContext::Struct,                        false, 0, 0, AttrArgKind::None, false, "" },
        { "inline",   AttributeContext::Func,                          false, 0, 0, AttrArgKind::None, false, "" },
        { "noinline", AttributeContext::Func,                          false, 0, 0, AttrArgKind::None, false, "" },
        { "deprecated", AttributeContext::Func | AttributeContext::Var | AttributeContext::Struct | AttributeContext::Impl,
          true, 0, 1, AttrArgKind::String, false, "" },
    };
    const size_t kNumBuiltinAttrs = sizeof(kBuiltinAttrs) / sizeof(kBuiltinAttrs[0]);
}

void AttributeRegistry::setStringPool(StringPool& pool) {
    // Always rebuild from the static array (even if already set, to allow re‑init).
    stringPool = &pool;

    // Clear existing maps
    byId.clear();
    nameToId.clear();

    for (const auto& builtin : kBuiltinAttrs) {
        InternedString id = pool.intern(std::string(builtin.name));
        std::string_view nameView = pool.lookup(id);
        AttributeInfo info;
        info.id = id;
        info.name = nameView;
        info.validContexts = builtin.contexts;
        info.takesArgs = builtin.takesArgs;
        info.minArgs = builtin.minArgs;
        info.maxArgs = builtin.maxArgs;
        info.allowedArgKinds = builtin.argKinds;
        info.requiresConst = builtin.requiresConst;
        info.exclusiveWith = builtin.exclusiveWith.empty() ? InternedString() : pool.intern(std::string(builtin.exclusiveWith));
        // validator remains nullptr for now – add later if needed

        byId[id] = info;
        nameToId[std::string(builtin.name)] = id;
    }

    // Pre‑intern well‑known IDs (still needed for fast access)
    externId     = pool.intern("extern");
    packedId     = pool.intern("packed");
    inlineId     = pool.intern("inline");
    noinlineId   = pool.intern("noinline");
    deprecatedId = pool.intern("deprecated");
}

void AttributeRegistry::resetStringPool() {
    stringPool = nullptr;
    // Optionally clear interned caches (byId, nameToId) if they depend on pool strings.
    // For safety, clear everything to prevent dangling string_views.
    byId.clear();
    nameToId.clear();
    externId = InternedString();
    packedId = InternedString();
    inlineId = InternedString();
    noinlineId = InternedString();
    deprecatedId = InternedString();
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
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  attr.loc, DiagCode::E2003,
                  "unknown attribute '@" + std::string(name) + "'");
        return false;
    }

    bool valid = true;

    // Context check
    if (!hasFlag(info->validContexts, ctx)) {
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  attr.loc, DiagCode::E2010,
                  "@" + std::string(info->name) + " cannot be used on this declaration");
        valid = false;
    }

    // Arg count
    int argCount = static_cast<int>(attr.args.size());
    if (info->takesArgs) {
        if (argCount < info->minArgs) {
            dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                      attr.loc, DiagCode::E2009,
                      "@" + std::string(info->name) + " requires at least " +
                      std::to_string(info->minArgs) + " argument(s)");
            valid = false;
        }
        if (info->maxArgs != -1 && argCount > info->maxArgs) {
            dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                      attr.loc, DiagCode::E2009,
                      "@" + std::string(info->name) + " takes at most " +
                      std::to_string(info->maxArgs) + " argument(s)");
            valid = false;
        }
    } else if (argCount > 0) {
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  attr.loc, DiagCode::E2009,
                  "@" + std::string(info->name) + " does not take arguments");
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
            dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                      arg->loc, DiagCode::E2009,
                      "invalid argument type for @" + std::string(info->name));
            valid = false;
        }
    }

    // requiresConst
    if (info->requiresConst && declKw != DeclKeyword::Const) {
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  attr.loc, DiagCode::E3002,
                  "@" + std::string(info->name) + " requires 'const', not 'let'");
        valid = false;
    }

    // Custom validator
    if (info->validator && !info->validator(attr.args, declName, dc, attr.loc)) {
        valid = false;
    }

    return valid;
}

bool AttributeRegistry::isExternAttribute(InternedString id) const {
    return id == externId;
}

bool AttributeRegistry::isExternAttribute(const std::string& name) const {
    auto it = nameToId.find(name);
    return it != nameToId.end() && it->second == externId;
}

bool AttributeRegistry::isMainOnlyAttribute(InternedString id) const {
    // none currently
    return false;
}

bool AttributeRegistry::isMainOnlyAttribute(const std::string& name) const {
    return false;
}

bool AttributeRegistry::checkMutualExclusion(InternedString id1, InternedString id2,
                                             DiagnosticEngine& dc,
                                             const SourceLocation& loc) const {
    const AttributeInfo* a = lookup(id1);
    const AttributeInfo* b = lookup(id2);
    if (!a || !b) return true; // already reported elsewhere

    if (a->exclusiveWith.isValid() && a->exclusiveWith == id2) {
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  loc, DiagCode::E2007,
                  "@" + std::string(a->name) + " and @" + std::string(b->name) +
                  " cannot be used together");
        return false;
    }
    if (b->exclusiveWith.isValid() && b->exclusiveWith == id1) {
        dc.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax,
                  loc, DiagCode::E2007,
                  "@" + std::string(a->name) + " and @" + std::string(b->name) +
                  " cannot be used together");
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