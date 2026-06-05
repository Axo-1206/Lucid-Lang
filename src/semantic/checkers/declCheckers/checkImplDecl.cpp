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

// ─────────────────────────────────────────────────────────────────────────────
// checkImplDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local impls are private.
//   - Target struct must exist in symbol table (can be from any visible scope).
//   - Generic parameters must match the struct's generic parameters (if any).
//   - No duplicate method names within the impl block (merged across scopes).
//   - Method signatures must match the trait's method signatures (if traitRef present).
//   - Struct fields are injected into each method's scope.
//   - Method bodies are checked with the correct return type.
//   - Parallel/async qualifiers are tracked for depth counters.
//   - Impl blocks can appear in any scope and are visible in that scope and
//     any nested scope (standard lexical scoping).
// ─────────────────────────────────────────────────────────────────────────────
void checkImplDecl(ImplDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkImplDecl: isLocal=" << isLocal);

    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local impl cannot have visibility modifier (pub/export)");
    }

    // Ensure the target was resolved
    if (!node.resolvedSelfType) {
        ctx.error(node.loc, DiagCode::E2001, "impl target could not be resolved");
        return;
    }

    // Validate generic parameters against the target
    if (node.resolvedTargetGenericParams) {
        checkImplGenericParams(node, node.resolvedTargetGenericParams, ctx);
    } else if (!node.genericParams.empty()) {
        ctx.error(node.loc, DiagCode::E2015, "non‑generic target cannot have generic parameters");
    }

    // Push substitution map (if any) so that generic parameters inside method bodies are replaced
    if (!node.resolvedSubstitutionMap.empty() && ctx.dispatcher) {
        ctx.dispatcher->pushSubstitutionMap(&node.resolvedSubstitutionMap);
    }

    // Check each method
    std::unordered_set<std::string> seenMethods;
    for (auto& method : node.methods) {
        if (!method) continue;
        std::string mname = std::string(ctx.pool.lookup(method->name));
        if (!seenMethods.insert(mname).second) {
            ctx.error(method->loc, DiagCode::E2005, "duplicate method '", mname, "' in impl");
            continue;
        }

        TypeAST* expectedReturn = nullptr;
        if (method->funcType && !method->funcType->sig.returnTypes.empty()) {
            expectedReturn = method->funcType->sig.returnTypes[0].get();
        }

        bool wasParallel = method->funcType && method->funcType->isParallel();
        if (wasParallel) ctx.enterParallel();
        checkImplMethod(node, *method, expectedReturn, ctx);
        if (wasParallel) ctx.exitParallel();
    }

    // Pop substitution map
    if (!node.resolvedSubstitutionMap.empty() && ctx.dispatcher) {
        ctx.dispatcher->popSubstitutionMap();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkImplDecl: complete");
}