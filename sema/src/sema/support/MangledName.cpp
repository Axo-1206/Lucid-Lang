/// @file MangledName.cpp
/// @brief Implementation of mangled name generation.

#include "MangledName.hpp"

#include "sema/context/SemaContext.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <cctype>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Sanitize a string for use in a symbol name.
///
/// Replaces every character that is not a letter, a digit, or an
/// underscore with an underscore. The result is a valid C identifier
/// fragment. Case is preserved; the mangled name is case-sensitive.
std::string sanitizeForMangledName(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(c);
        } else {
            result.push_back('_');
        }
    }
    return result;
}

/// The current module's path, sanitized for use in a mangled name.
///
/// The path is the module's file path relative to the package root
/// (grammar §3.1). Path separators and dots are folded to underscores so
/// the result is a valid identifier fragment.
std::string mangledModulePath(SemaContext& ctx) {
    if (!ctx.currentModule) return "global";
    return sanitizeForMangledName(
        ctx.pool.lookupView(ctx.currentModule->filePath));
}

/// Build a mangled name from a module path and a sanitized declaration
/// name. The `_L` prefix marks the name as a Lucid-mangled symbol; the
/// `_` separates the module path from the declaration name.
InternedString buildMangled(const std::string& modulePath,
                            std::string_view name,
                            SemaContext& ctx) {
    std::string result;
    result.reserve(2 + modulePath.size() + 1 + name.size());
    result += "_L";
    result += modulePath;
    result += "_";
    result += name;
    return ctx.pool.intern(result);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// generateMangledName — FuncDeclAST
// ─────────────────────────────────────────────────────────────────────────────

InternedString generateMangledName(FnDeclAST* decl, SemaContext& ctx) {
    if (!decl) return InternedString{};

    // ─── @export: the symbol is the source name ─────────────────────────
    //
    // An `@export`ed function's symbol is exactly the name the user
    // wrote. No prefix, no module path. The host looks up `onTick` by
    // the name `onTick`, and the bytecode module's symbol table must
    // resolve that name to this function.
    //
    // A host-bound function without `@export` uses the same mangled
    // shape as a Lucid-bodied function. The `hostName` field is the
    // name in the *native* symbol table; the mangled name is the name
    // in the bytecode module's symbol table. The two are independent.
    if (decl->isExported) {
        return decl->name;
    }

    // ─── Non-exported: `_L<module-path>_<name>` ─────────────────────────
    std::string name = sanitizeForMangledName(ctx.pool.lookupView(decl->name));
    return buildMangled(mangledModulePath(ctx), name, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// generateMangledName — VarDeclAST
// ─────────────────────────────────────────────────────────────────────────────

InternedString generateMangledName(VarDeclAST* decl, SemaContext& ctx) {
    if (!decl) return InternedString{};

    if (decl->isExported) {
        return decl->name;
    }

    std::string name = sanitizeForMangledName(ctx.pool.lookupView(decl->name));
    return buildMangled(mangledModulePath(ctx), name, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// generateMangledName — TableDeclAST
// ─────────────────────────────────────────────────────────────────────────────

InternedString generateMangledName(TableDeclAST* decl, SemaContext& ctx) {
    if (!decl) return InternedString{};

    // A table's `@export` semantics differ slightly from a function's.
    //
    // For a function, `@export` means "the host can look this function
    // up by the name written in source". The mangled name is the source
    // name and the host's lookup is a plain string match.
    //
    // For a table, `@export` means "other modules (Tier 2 in particular)
    // can see this table". The `@export`-based visibility is what
    // `resolveModuleMemberAccess` checks, not the mangled name. But the
    // mangled name still has to exist and still has to be unique across
    // the loaded module set, because the bytecode module's table
    // registry keys on it.
    //
    // The short-circuit here uses the source name for consistency with
    // functions and variables. If a future design wants tables to have
    // module-qualified symbols even when exported (to disambiguate two
    // exported tables of the same name in different modules), the
    // short-circuit can be removed without touching anything else.
    if (decl->isExported) {
        return decl->name;
    }

    std::string name = sanitizeForMangledName(ctx.pool.lookupView(decl->name));
    return buildMangled(mangledModulePath(ctx), name, ctx);
}

} // namespace lucid::sema