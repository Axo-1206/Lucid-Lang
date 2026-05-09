#include "AttributeRegistry.hpp"
#include "ast/DeclAST.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <sstream>

AttributeRegistry& AttributeRegistry::instance() {
    static AttributeRegistry registry;
    return registry;
}

AttributeRegistry::AttributeRegistry() {
    // All attributes are registered with their names – they will be interned later
    // when setStringPool is called. Until then, the registry is not usable.
    // We'll just store the raw strings; setStringPool will intern them.
    // For now, we store the registration data in a temporary vector, then in setStringPool we actually create the entries.
    // Simpler: register inline but without interning; we can still store the string name and later set the ID.
    // We'll do the registration in setStringPool after the pool is known.
    // But the registry is a singleton; we cannot postpone registration because the constructor runs before setStringPool.
    // So we must store the registration info as strings, then later intern.
    // We'll store a list of pending registrations, then process them in setStringPool.
}

// We'll use a static vector of pending registrations
namespace {
    struct PendingAttr {
        std::string name;
        AttributeContext contexts;
        bool takesArgs;
        int minArgs;
        int maxArgs;
        AttrArgKind argKinds;
        bool requiresConst;
        std::string exclusiveWith;
        bool (*validator)(const std::vector<ASTPtr<AttributeArgAST>>&,
                          const std::string&, DiagnosticEngine&,
                          const SourceLocation&) = nullptr;
    };
    std::vector<PendingAttr> pending;
}

void AttributeRegistry::registerAttribute(const std::string& name,
                                          AttributeContext contexts,
                                          bool takesArgs, int minArgs, int maxArgs,
                                          AttrArgKind argKinds,
                                          bool requiresConst,
                                          const std::string& exclusiveWith,
                                          bool (*validator)(const std::vector<ASTPtr<AttributeArgAST>>&,
                                                            const std::string&, DiagnosticEngine&,
                                                            const SourceLocation&)) {
    pending.push_back({name, contexts, takesArgs, minArgs, maxArgs, argKinds, requiresConst, exclusiveWith, validator});
}

void AttributeRegistry::setStringPool(StringPool& pool) {
    if (stringPool) return; // already set
    stringPool = &pool;

    // Process pending registrations
    for (const auto& p : pending) {
        InternedString id = pool.intern(p.name);
        std::string_view nameView = pool.lookup(id);
        AttributeInfo info;
        info.id = id;
        info.name = nameView;
        info.validContexts = p.contexts;
        info.takesArgs = p.takesArgs;
        info.minArgs = p.minArgs;
        info.maxArgs = p.maxArgs;
        info.allowedArgKinds = p.argKinds;
        info.requiresConst = p.requiresConst;
        info.exclusiveWith = p.exclusiveWith.empty() ? InternedString() : pool.intern(p.exclusiveWith);
        info.validator = p.validator;

        byId[id] = info;
        nameToId[p.name] = id;
    }
    pending.clear();

    // Pre‑intern well‑known IDs
    externId     = pool.intern("extern");
    packedId     = pool.intern("packed");
    inlineId     = pool.intern("inline");
    noinlineId   = pool.intern("noinline");
    deprecatedId = pool.intern("deprecated");
}

const AttributeInfo* AttributeRegistry::lookup(InternedString id) const {
    auto it = byId.find(id);
    return (it != byId.end()) ? &it->second : nullptr;
}

const AttributeInfo* AttributeRegistry::lookup(const std::string& name) const {
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
    if (!stringPool) return false; // not initialised

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