/// @file registry/AttributeValidator.cpp
/// @brief Implementation of pure attribute validation functions.

#include "AttributeValidator.hpp"
#include "core/registry/AttributeRegistry.hpp"
#include "../types/ArgumentTypeValidators.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/Sema.hpp"

#include <unordered_set>

namespace sema {

bool validateAllAttributes(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return true;

    // ─── Validate each attribute ──────────────────────────────────────────
    // The registry's ATTRIBUTE_TABLE defines which declaration kinds each
    // attribute can attach to. validateAttribute() checks this via
    // isAllowedOnDecl().
    bool allValid = true;
    for (AttributeAST* attr : decl->attributes) {
        if (!attr) continue;
        if (!validateAttribute(attr, decl, ctx)) {
            allValid = false;
        }
    }

    // ─── Check for duplicate attributes ─────────────────────────────────
    std::unordered_set<InternedString> seen;
    for (AttributeAST* attr : decl->attributes) {
        if (seen.find(attr->name) != seen.end()) {
            ctx.diagnostics.error(DiagCode::Sem_AttributeDuplicate, attr,
                                  "duplicate attribute '@", ctx.pool.lookup(attr->name),
                                  "' on declaration '", ctx.pool.lookup(decl->name), "'");
            allValid = false;
        }
        seen.insert(attr->name);
    }

    return allValid;
}

bool validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    if (!attr) return false;

    const AttributeInfo* info = AttributeRegistry::getInstance(ctx.pool).getInfo(attr->name);
    if (!info) {
        ctx.diagnostics.error(DiagCode::Sem_UnknownAttribute, attr,
                              "unknown attribute '@", ctx.pool.lookup(attr->name), "'");
        return false;
    }

    // ─── 1. Check: Is this attribute allowed on this declaration kind? ────
    // This uses the ATTRIBUTE_TABLE's allowedKinds list.
    if (!AttributeRegistry::getInstance(ctx.pool).isAllowedOnDecl(attr->name, owner->kind)) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeNotApplicable, attr,
                              "attribute '@", ctx.pool.lookup(attr->name),
                              "' cannot be applied to '", 
                              debug::kindToString(owner->kind), "'");
        return false;
    }

    // ─── 2. Check: Is this attribute only for generic declarations? ──────
    if (info->appliesToGenericOnly) {
        bool isGeneric = false;
        if (owner->isa<FuncDeclAST>()) {
            isGeneric = !owner->as<FuncDeclAST>()->genericParams.empty();
        } else if (owner->isa<StructDeclAST>()) {
            isGeneric = !owner->as<StructDeclAST>()->genericParams.empty();
        }
        
        if (!isGeneric) {
            ctx.diagnostics.error(DiagCode::Sem_AttributeNotApplicable, attr,
                                  "attribute '@", ctx.pool.lookup(attr->name),
                                  "' can only be applied to generic declarations");
            ctx.diagnostics.note(attr,
                                 "'", ctx.pool.lookup(owner->name),
                                 "' has no generic parameters. Remove '@", 
                                 ctx.pool.lookup(attr->name), "'.");
            return false;
        }
    }

    std::string name = ctx.pool.lookup(attr->name);

    // ─── Dispatch to specific validator ────────────────────────────────────
    if (name == "export") {
        return validateExport(attr, owner, ctx);
    }
    if (name == "foreign") {
        return validateForeign(attr, owner, ctx);
    }
    if (name == "link") {
        return validateLink(attr, owner, ctx);
    }
    if (name == "deprecated") {
        return validateDeprecated(attr, owner, ctx);
    }
    if (name == "inline" || name == "noinline") {
        return validateInlineHint(attr, owner, ctx);
    }
    if (name == "specialize") {
        return validateSpecialize(attr, owner, ctx);
    }

    // ─── Generic validation for unknown attributes ─────────────────────────
    if (!validateArgCount(attr, info->minArgs, info->maxArgs, ctx)) {
        return false;
    }

    if (info->requiresStringArgs) {
        for (size_t i = 0; i < attr->args.size(); ++i) {
            if (!validateStringArg(attr->args[i], "argument " + std::to_string(i + 1), ctx)) {
                return false;
            }
        }
    }

    return true;
}

// ─── Individual Attribute Validators ──────────────────────────────────────

bool validateExport(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // ─── 1. Validate argument count ──────────────────────────────────────
    if (!attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr,
                              "attribute '@[export]' takes no arguments");
        return false;
    }

    // ─── 2. Validate placement: only at module level ──────────────────────
    if (!isModuleLevelDeclaration(owner, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeInvalid, attr,
                              "attribute '@[export]' is only legal at module level");
        return false;
    }

    return true;
}

bool validateForeign(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // ─── 1. Validate owner: only on functions ─────────────────────────────
    if (!owner || !owner->isa<FuncDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeInvalid, attr,
                              "attribute '@[foreign]' is only legal on function declarations");
        return false;
    }

    // ─── 2. Validate argument count ──────────────────────────────────────
    if (!validateArgCount(attr, 1, 1, ctx)) {
        return false;
    }

    // ─── 3. Validate ABI string ────────────────────────────────────────────
    if (!validateStringArg(attr->args[0], "ABI", ctx)) {
        return false;
    }

    const LiteralExprAST* lit = attr->args[0]->as<LiteralExprAST>();
    if (!lit) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, attr->args[0],
                              "@[foreign] argument must be a string literal");
        return false;
    }

    std::string abi = ctx.pool.lookup(lit->value);
    if (abi != "C") {
        ctx.diagnostics.error(DiagCode::Sem_ForeignABI, attr->args[0],
                              "unsupported foreign ABI '", abi, "' — only \"C\" is supported");
        return false;
    }

    // ─── 4. Warn if function has a body ──────────────────────────────────
    FuncDeclAST* func = owner->as<FuncDeclAST>();
    if (func->body) {
        ctx.diagnostics.warning(DiagCode::Warn_ForeignBody, attr,
                                "foreign function '", ctx.pool.lookup(owner->name),
                                "' has a body; it will be ignored");
    }

    return true;
}

bool validateLink(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // ─── 1. Validate placement ──────────────────────────────────────────────
    bool atModuleLevel = isModuleLevelDeclaration(owner, ctx);
    bool onFunction = owner && owner->isa<FuncDeclAST>();

    if (!atModuleLevel && !onFunction) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeInvalid, attr,
                              "attribute '@[link]' is only legal at module level or on function declarations");
        return false;
    }

    // ─── 2. Validate argument count ──────────────────────────────────────────
    if (attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr,
                              "attribute '@[link]' expects at least 1 argument (library name or file path), got 0");
        return false;
    }

    // ─── 3. Validate each argument ──────────────────────────────────────────
    bool allValid = true;
    for (size_t i = 0; i < attr->args.size(); ++i) {
        ExprAST* arg = attr->args[i];

        if (!arg || arg->kind != ASTKind::LiteralExpr) {
            ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                                  "@[link] argument ", i + 1,
                                  " must be a string literal, got non-literal expression");
            allValid = false;
            continue;
        }

        const LiteralExprAST* lit = arg->as<LiteralExprAST>();

        if (lit->kind != LiteralKind::String && lit->kind != LiteralKind::RawString) {
            ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                                  "@[link] argument ", i + 1,
                                  " must be a string literal, got ",
                                  debug::literalKindToString(lit->kind));
            allValid = false;
            continue;
        }

        std::string value = ctx.pool.lookup(lit->value);
        if (value.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                                  "@[link] argument ", i + 1, " cannot be an empty string");
            allValid = false;
            continue;
        }

        // ─── Warnings about common mistakes ──────────────────────────────
        if (value.find(' ') != std::string::npos) {
            ctx.diagnostics.warning(DiagCode::Warn_UnsafeFFI, arg,
                                    "@[link] argument '", value,
                                    "' contains a space — library names and file paths should not contain spaces");
        }

        if (value.find("./") == 0 || value.find(".\\") == 0) {
            ctx.diagnostics.warning(DiagCode::Warn_UnsafeFFI, arg,
                                    "@[link] argument '", value,
                                    "' uses './' — prefer absolute or package-relative paths");
        }

        if (value.find('.') == std::string::npos) {
            ctx.diagnostics.warning(DiagCode::Warn_UnsafeFFI, arg,
                                    "@[link] argument '", value,
                                    "' has no file extension — on Windows, prefer '.lib' or '.dll'");
        }
    }

    return allValid;
}

bool validateDeprecated(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    (void)owner;

    // ─── 1. Validate argument count ──────────────────────────────────────────
    if (attr->args.size() > 1) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr,
                              "attribute '@[deprecated]' expects at most 1 argument (the message), got ",
                              attr->args.size());
        return false;
    }

    // ─── 2. Validate optional message argument ──────────────────────────────
    if (!attr->args.empty()) {
        if (!validateStringArg(attr->args[0], "message", ctx)) {
            return false;
        }
    }

    return true;
}

/// @brief Validate @[inline] and @[noinline] attributes.
bool validateInlineHint(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // ─── 1. Validate owner: only on functions ─────────────────────────────
    if (!owner || !owner->isa<FuncDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeInvalid, attr,
                              "attribute '@", ctx.pool.lookup(attr->name), 
                              "' is only legal on function declarations");
        return false;
    }

    // ─── 2. Validate argument count ──────────────────────────────────────────
    if (!attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr,
                              "attribute '@", ctx.pool.lookup(attr->name), 
                              "' takes no arguments");
        return false;
    }

    // ─── 3. Warn if used on foreign functions ───────────────────────────────
    for (AttributeAST* existing : owner->attributes) {
        if (ctx.pool.lookup(existing->name) == "foreign") {
            const char* hint = "will be ignored";
            if (ctx.pool.lookup(attr->name) == "noinline") {
                hint = "foreign functions are not inlined by default, '@[noinline]' is redundant";
            }
            ctx.diagnostics.warning(DiagCode::Warn_ForeignInline, attr,
                                    "foreign function '", ctx.pool.lookup(owner->name),
                                    "' cannot be inlined (", hint, ")");
            break;
        }
    }

    // ─── 4. Store the appropriate flag on the function ─────────────────────
    FuncDeclAST* func = const_cast<FuncDeclAST*>(owner->as<FuncDeclAST>());
    if (ctx.pool.lookup(attr->name) == "inline") {
        func->isInline = true;
    } else {
        func->isNoInline = true;
    }

    return true;
}

bool validateSpecialize(AttributeAST* attr, DeclAST* owner, SemaContext& ctx) {
    // ─── 1. The registry already verified this is on FuncDecl or StructDecl ──
    // So we just need to verify it's generic
    
    bool isGeneric = false;
    if (owner->isa<FuncDeclAST>()) {
        isGeneric = !owner->as<FuncDeclAST>()->genericParams.empty();
    } else if (owner->isa<StructDeclAST>()) {
        isGeneric = !owner->as<StructDeclAST>()->genericParams.empty();
    }
    
    if (!isGeneric) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeNotApplicable, attr,
                              "attribute '@[specialize]' can only be applied to generic declarations");
        return false;
    }

    // ─── 2. Validate argument count ──────────────────────────────────────────
    if (!attr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr,
                              "attribute '@[specialize]' takes no arguments");
        return false;
    }

    // ─── 3. Mark as needing specialization ──────────────────────────────────
    if (owner->isa<FuncDeclAST>()) {
        const_cast<FuncDeclAST*>(owner->as<FuncDeclAST>())->shouldSpecialize = true;
    } else if (owner->isa<StructDeclAST>()) {
        const_cast<StructDeclAST*>(owner->as<StructDeclAST>())->shouldSpecialize = true;
    }

    return true;
}

// ─── Helpers ──────────────────────────────────────────────────────────────

bool validateStringArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, nullptr,
                              "argument '", argName, "' is null");
        return false;
    }

    TypeAST* result = resolveExprWithTarget(
        const_cast<ExprAST*>(arg), ctx.getStringType(), ctx
    );
    if (!result || result->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects a string literal");
        return false;
    }

    if (!arg->isa<LiteralExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                              "argument '", argName, "' must be a string literal (not an expression)");
        return false;
    }

    const LiteralExprAST* lit = arg->as<LiteralExprAST>();
    if (lit->kind != LiteralKind::String && lit->kind != LiteralKind::RawString) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                              "argument '", argName, "' must be a string literal");
        return false;
    }

    return true;
}

bool validateArgCount(AttributeAST* attr, size_t min, size_t max, SemaContext& ctx) {
    size_t actual = attr->args.size();
    if (actual < min || (max > 0 && actual > max)) {
        std::string msg = "attribute '@" + ctx.pool.lookup(attr->name) +
                          "' expects ";
        if (min == max) {
            msg += std::to_string(min) + " argument(s)";
        } else if (max == 0) {
            msg += "at least " + std::to_string(min) + " argument(s)";
        } else {
            msg += "between " + std::to_string(min) + " and " + std::to_string(max) + " arguments";
        }
        msg += ", got " + std::to_string(actual);
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgCount, attr, msg);
        return false;
    }
    return true;
}

bool supportsAttributes(DeclAST* decl) {
    if (!decl) return false;

    switch (decl->kind) {
        case ASTKind::ImportDecl:
        case ASTKind::VarDecl:
        case ASTKind::FuncDecl:
        case ASTKind::StructDecl:
        case ASTKind::EnumDecl:
        case ASTKind::TraitDecl:
        case ASTKind::FieldDecl:
        case ASTKind::TraitFieldDecl:
        case ASTKind::EnumVariant:
            return true;

        case ASTKind::Param:
        case ASTKind::GenericParamDecl:
        case ASTKind::UnknownDecl:
            return false;

        default:
            return false;
    }
}

/// @brief Check if a declaration is at module level (top-level).
/// This checks the declaration's actual position in the AST, not the current scope.
/// @note Different from ctx.isAtModuleLevel() which checks if the current scope is module-level.
bool isModuleLevelDeclaration(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return false;

    // Check if the declaration is in the current module's decl list
    if (ctx.currentModule) {
        for (DeclAST* d : ctx.currentModule->decls) {
            if (d == decl) return true;
        }
    }

    // Also check if it's in the module table
    if (ctx.currentModuleTable) {
        for (const auto& [name, value] : ctx.currentModuleTable->values) {
            if (static_cast<DeclAST*>(value) == decl) return true;
        }
        for (const auto& [name, type] : ctx.currentModuleTable->types) {
            if (static_cast<DeclAST*>(type) == decl) return true;
        }
    }

    return false;
}

} // namespace sema