/// @file AttributeRegistry.hpp
/// @brief Validates `@[...]` attributes against what's legal on declarations.
/// 
/// @architectural_note Stateless Design
///   Unlike IntrinsicRegistry which has state (maps of names to LLVM IDs),
///   AttributeRegistry is header-only and stateless. It only validates
///   attributes against declarations using pure functions.
/// 
/// @architectural_note Type Validation via resolveExprWithTarget
///   Instead of checking literal kinds directly, we use the type system
///   to validate attribute arguments. This is more consistent with the
///   rest of the semantic analysis.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaCompare.hpp"
#include "../types/SemaResolve.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "sema/Sema.hpp"

#include <string>

namespace sema {
namespace attr {

// ─── Attribute Name Constants ─────────────────────────────────────────────

inline InternedString kExport(SemaContext& ctx) {
    return ctx.pool.intern("export");
}

inline InternedString kForeign(SemaContext& ctx) {
    return ctx.pool.intern("foreign");
}

inline InternedString kLink(SemaContext& ctx) {
    return ctx.pool.intern("link");
}

inline InternedString kDeprecated(SemaContext& ctx) {
    return ctx.pool.intern("deprecated");
}

inline InternedString kInline(SemaContext& ctx) {
    return ctx.pool.intern("inline");
}

// ─── Attribute Query Functions ────────────────────────────────────────────

inline bool hasAttribute(ArenaSpan<AttributePtr> attrs, InternedString name) {
    for (const AttributeAST* attr : attrs) {
        if (attr->name == name) return true;
    }
    return false;
}

inline const AttributeAST* findAttribute(ArenaSpan<AttributePtr> attrs,
                                          InternedString name) {
    for (const AttributeAST* attr : attrs) {
        if (attr->name == name) return attr;
    }
    return nullptr;
}

// ─── Validators ─────────────────────────────────────────────────────────────

namespace detail {

inline bool isFunctionOwner(const DeclAST* owner) {
    return owner != nullptr && owner->isa<FuncDeclAST>();
}

inline bool isAtModuleLevel(const DeclAST* owner, SemaContext& ctx) {
    if (owner == nullptr) return true;
    return ctx.isAtModuleLevel();
}

/// @brief Validate that an attribute argument is a string literal.
/// Uses resolveExprWithTarget for type validation.
inline bool validateStringArg(const ExprAST* arg, SemaContext& ctx,
                               const std::string& argName) {
    TypeAST* result = resolveExprWithTarget(
        const_cast<ExprAST*>(arg), ctx.getStringType(), ctx
    );
    if (!result || result->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(ErrorCode::SemTypeMismatch, arg,
                              "argument '", argName, "' expects a string literal");
        return false;
    }
    return true;
}

inline void validateExport(const AttributeAST* attr,
                            const DeclAST* owner,
                            SemaContext& ctx) {
    if (!attr->args.empty()) {
        ctx.diagnostics.error(ErrorCode::SemAttributeArgCount, attr,
                              "attribute '@[export]' takes no arguments");
        return;
    }

    if (!isAtModuleLevel(owner, ctx)) {
        ctx.diagnostics.error(ErrorCode::SemAttributeInvalid, attr,
                              "attribute '@[export]' is only legal at module level");
    }
}

inline void validateForeign(const AttributeAST* attr,
                             const DeclAST* owner,
                             SemaContext& ctx) {
    if (!isFunctionOwner(owner)) {
        ctx.diagnostics.error(ErrorCode::SemAttributeInvalid, attr,
                              "attribute '@[foreign]' is only legal on function declarations");
        return;
    }

    if (attr->args.size() != 1) {
        ctx.diagnostics.error(ErrorCode::SemAttributeArgCount, attr,
                              "attribute '@[foreign]' expects exactly 1 argument (the ABI), got ",
                              attr->args.size());
        return;
    }

    // Validate the argument is a string literal
    if (!validateStringArg(attr->args[0], ctx, "ABI")) {
        return;
    }

    // Extract and validate the ABI string
    std::string abi = ctx.pool.lookup(attr->args[0]->as<LiteralExprAST>()->value);
    if (abi != "C") {
        ctx.diagnostics.error(ErrorCode::SemForeignABI, attr->args[0],
                              "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
    }
}

inline void validateLink(const AttributeAST* attr,
                          const DeclAST* owner,
                          SemaContext& ctx) {
    if (owner != nullptr && !isFunctionOwner(owner)) {
        ctx.diagnostics.error(ErrorCode::SemAttributeInvalid, attr,
                              "attribute '@[link]' is only legal at module level or on function declarations");
        return;
    }

    if (attr->args.empty()) {
        ctx.diagnostics.error(ErrorCode::SemAttributeArgCount, attr,
                              "attribute '@[link]' expects at least 1 argument (library name), got 0");
        return;
    }

    for (size_t i = 0; i < attr->args.size(); ++i) {
        if (!validateStringArg(attr->args[i], ctx, "library " + std::to_string(i + 1))) {
            return;
        }
    }
}

inline void validateDeprecated(const AttributeAST* attr,
                                const DeclAST* owner,
                                SemaContext& ctx) {
    (void)owner;
    if (attr->args.size() > 1) {
        ctx.diagnostics.error(ErrorCode::SemAttributeArgCount, attr,
                              "attribute '@[deprecated]' expects at most 1 argument (the message), got ",
                              attr->args.size());
        return;
    }

    if (!attr->args.empty()) {
        if (!validateStringArg(attr->args[0], ctx, "message")) {
            return;
        }
    }
}

inline void validateInline(const AttributeAST* attr,
                            const DeclAST* owner,
                            SemaContext& ctx) {
    if (!isFunctionOwner(owner)) {
        ctx.diagnostics.error(ErrorCode::SemAttributeInvalid, attr,
                              "attribute '@[inline]' is only legal on function declarations");
        return;
    }

    if (!attr->args.empty()) {
        ctx.diagnostics.error(ErrorCode::SemAttributeArgCount, attr,
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
            ctx.diagnostics.error(ErrorCode::SemUnknownAttribute, attr,
                                  "unknown attribute '@", ctx.pool.lookup(name), "'");
        }
    }
}

} // namespace attr
} // namespace sema