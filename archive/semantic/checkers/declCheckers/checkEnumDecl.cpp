#include "ast/ExprAST.hpp"
#include "ast/support/ArenaSpan.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// checkEnumDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local enums are private.
//   - Explicit integer values must be unique within the enum.
//   - Auto-assigned values are computed sequentially.
//   - No overflow checking (deferred to codegen/backing type selection).
//   - Enum variants are value-comparable (checked elsewhere).
// ─────────────────────────────────────────────────────────────────────────────
void checkEnumDecl(EnumDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkEnumDecl: name=" << ctx.pool.lookup(node.name)
                     << ", variants=" << node.variants.size()
                     << ", isLocal=" << isLocal);

    // ── Local enums cannot have visibility modifiers ─────────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local enum cannot have visibility modifier (pub/export)");
    }

    // ── Validate attributes ─────────────────────────────────────────────────
    // The registry will reject @extern, @inline, @packed, etc.
    // We don't need to capture extern metadata for enums
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::Enum),
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Check variant values for uniqueness ──────────────────────────────────
    std::unordered_set<int64_t> usedValues;
    int64_t nextAuto = 0;

    for (auto& variant : node.variants) {
        if (!variant) continue;

        std::string variantName = std::string(ctx.pool.lookup(variant->name));
        int64_t value;

        if (variant->explicitValue.has_value()) {
            value = variant->explicitValue.value();
            LUC_LOG_SEMANTIC_EXTREME("\tvariant '" << variantName
                                     << "' has explicit value " << value);
        } else {
            value = nextAuto;
            LUC_LOG_SEMANTIC_EXTREME("\tvariant '" << variantName
                                     << "' auto-assigned value " << value);
        }

        // Check for duplicate values
        if (!usedValues.insert(value).second) {
            ctx.error(variant->loc, DiagCode::E2005,
                      "duplicate enum value ", value, " for variant '", variantName,
                      "' in enum '", ctx.pool.lookup(node.name), "'");
        }

        // Update next auto value (even if duplicate was reported, continue)
        nextAuto = value + 1;
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkEnumDecl: complete for " << ctx.pool.lookup(node.name)
                             << " with " << usedValues.size() << " unique values");
}