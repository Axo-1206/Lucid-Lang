#include "ast/ExprAST.hpp"
#include "ast/support/ArenaSpan.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// checkStructDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local structs are private.
//   - No duplicate field names.
//   - Each field type must resolve (or already resolved from Phase 2).
//   - Default value expressions (if present) must be assignable to the field type.
//   - Default values must be compile‑time constants (future extension).
//   - Generic parameters are pushed to allow field type resolution if needed.
// ─────────────────────────────────────────────────────────────────────────────
void checkStructDecl(StructDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkStructDecl: name=" << ctx.pool.lookup(node.name)
                     << ", isLocal=" << isLocal);

    // ── Local structs cannot have visibility modifiers ───────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local struct cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (packed, deprecated, etc.) ──────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::Struct),
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Push generic parameters so that default values can refer to T ─────────
    if (!node.genericParams.empty() && ctx.resolver) {
        ctx.resolver->pushGenericParams(&node.genericParams);
    }

    // ── Check fields: duplicate names, resolve types, default values ─────────
    std::unordered_set<std::string> seenFieldNames;
    for (auto& field : node.fields) {
        if (!field) continue;

        std::string fieldName = std::string(ctx.pool.lookup(field->name));
        if (!seenFieldNames.insert(fieldName).second) {
            ctx.error(field->loc, DiagCode::E2005,
                      "duplicate field '", fieldName, "' in struct '", ctx.pool.lookup(node.name), "'");
            continue;
        }

        // Resolve field type (should already be resolved by Phase 2, but be defensive)
        TypeAST* fieldType = field->type.get();
        if (!fieldType && ctx.resolver) {
            fieldType = ctx.resolver->resolveType(field->type.get());
            if (!fieldType) {
                ctx.error(field->loc, DiagCode::E2001,
                          "cannot resolve type for field '", fieldName, "' in struct '", ctx.pool.lookup(node.name), "'");
                continue;
            }
        }

        // Check default value, if present
        if (field->defaultVal) {
            TypeAST* defaultType = checkExpr(field->defaultVal.get(), ctx);
            if (defaultType && !TypeChecker::isAssignable(defaultType, fieldType, ctx)) {
                ctx.error(field->loc, DiagCode::E2002,
                          "default value type mismatch for field '", fieldName,
                          "' in struct '", ctx.pool.lookup(node.name), "'");
            }
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty() && ctx.resolver) {
        ctx.resolver->popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkStructDecl: complete for " << ctx.pool.lookup(node.name));
}