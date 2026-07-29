/// @file AttributeRegistry.hpp
/// @brief Validates `@[...]` attributes against what's legal on declarations.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "../support/LiteralHelpers.hpp"

namespace sema {
namespace attr {

// ─── Attribute Name Constants ─────────────────────────────────────────────

inline InternedString kExport(SemaContext& ctx) {
    return ctx.pool().intern("export");
}

inline InternedString kForeign(SemaContext& ctx) {
    return ctx.pool().intern("foreign");
}

inline InternedString kLink(SemaContext& ctx) {
    return ctx.pool().intern("link");
}

inline InternedString kDeprecated(SemaContext& ctx) {
    return ctx.pool().intern("deprecated");
}

inline InternedString kInline(SemaContext& ctx) {
    return ctx.pool().intern("inline");
}

// ─── Validators ─────────────────────────────────────────────────────────────

namespace detail {

inline bool isFunctionOwner(const DeclAST* owner) {
    return owner != nullptr && owner->isa<FuncDeclAST>();
}

inline bool isAtModuleLevel(const DeclAST* owner, SemaContext& ctx) {
    if (owner == nullptr) return true;
    return ctx.symbols.isAtModuleLevel();
}

inline void validateExport(const AttributeAST* attr,
                            const DeclAST* owner,
                            SemaContext& ctx) {
    if (!attr->args.empty()) {
        ctx.error(attr, DiagCode::E4002,
                  "attribute '@[export]' takes no arguments");
        return;
    }

    if (!isAtModuleLevel(owner, ctx)) {
        ctx.error(attr, DiagCode::E4001,
                  "attribute '@[export]' is only legal at module level");
    }
}

inline void validateForeign(const AttributeAST* attr,
                             const DeclAST* owner,
                             SemaContext& ctx) {
    if (!isFunctionOwner(owner)) {
        ctx.error(attr, DiagCode::E4001,
                  "attribute '@[foreign]' is only legal on function declarations");
        return;
    }

    if (attr->args.size() != 1) {
        ctx.error(attr, DiagCode::E4002,
                  "attribute '@[foreign]' expects exactly 1 argument (the ABI), got ",
                  std::to_string(attr->args.size()));
        return;
    }

    // Use unified literal helpers
    auto abi = literal::extractString(attr->args[0], ctx.pool());
    if (!abi) {
        ctx.error(attr->args[0], DiagCode::E3003,
                  "attribute '@[foreign]' expects a string literal");
        return;
    }
    if (*abi != "C") {
        ctx.error(attr->args[0], DiagCode::E4101,
                  "unsupported foreign ABI '", *abi, "' — only \"C\" is supported");
    }
}

inline void validateLink(const AttributeAST* attr,
                          const DeclAST* owner,
                          SemaContext& ctx) {
    if (owner != nullptr && !isFunctionOwner(owner)) {
        ctx.error(attr, DiagCode::E4001,
                  "attribute '@[link]' is only legal at module level or on function declarations");
        return;
    }

    if (attr->args.empty()) {
        ctx.error(attr, DiagCode::E4002,
                  "attribute '@[link]' expects at least 1 argument (library name), got 0");
        return;
    }

    for (size_t i = 0; i < attr->args.size(); ++i) {
        if (!literal::isStringLiteral(attr->args[i])) {
            ctx.error(attr->args[i], DiagCode::E3003,
                      "argument ", std::to_string(i + 1),
                      " of '@[link]' expects a string literal");
            return;
        }
    }
}

inline void validateDeprecated(const AttributeAST* attr,
                                const DeclAST* owner,
                                SemaContext& ctx) {
    if (attr->args.size() > 1) {
        ctx.error(attr, DiagCode::E4002,
                  "attribute '@[deprecated]' expects at most 1 argument (the message), got ",
                  std::to_string(attr->args.size()));
        return;
    }

    if (!attr->args.empty()) {
        if (!literal::validateStringLiteral(attr->args[0], ctx, "message")) {
            return;
        }
    }
}

inline void validateInline(const AttributeAST* attr,
                            const DeclAST* owner,
                            SemaContext& ctx) {
    if (!isFunctionOwner(owner)) {
        ctx.error(attr, DiagCode::E4001,
                  "attribute '@[inline]' is only legal on function declarations");
        return;
    }

    if (!attr->args.empty()) {
        ctx.error(attr, DiagCode::E4002,
                  "attribute '@[inline]' takes no arguments");
    }
}

} // namespace detail

// ─── Public API ────────────────────────────────────────────────────────────

inline void validateAttributes(const DeclAST* owner, SemaContext& ctx) {
    for (const AttributeAST* attr : owner->attributes) {
        if (!attr) continue;

        const InternedString name = attr->name;

        if (name == kExport(ctx)) {
            detail::validateExport(attr, owner, ctx);
        } else if (name == kForeign(ctx)) {
            detail::validateForeign(attr, owner, ctx);
        } else if (name == kLink(ctx)) {
            detail::validateLink(attr, owner, ctx);
        } else if (name == kDeprecated(ctx)) {
            detail::validateDeprecated(attr, owner, ctx);
        } else if (name == kInline(ctx)) {
            detail::validateInline(attr, owner, ctx);
        } else {
            ctx.error(attr, DiagCode::E4003,
                      "unknown attribute '@", ctx.pool().lookup(name), "'");
        }
    }
}

} // namespace attr
} // namespace sema