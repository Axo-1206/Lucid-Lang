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
// checkTraitDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local traits are private.
//   - No duplicate method names within the trait.
//   - Each method's parameter types and return type must resolve.
//   - Generic type parameters (e.g., T in Trait<T>) are pushed onto the stack
//     so that method signatures can refer to them.
//   - Traits cannot have fields or default implementations (bodies are empty).
//   - Async/parallel qualifiers on trait methods are allowed but not checked here.
// ─────────────────────────────────────────────────────────────────────────────
void checkTraitDecl(TraitDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkTraitDecl: name=" << ctx.pool.lookup(node.name)
                     << ", methods=" << node.methods.size()
                     << ", isLocal=" << isLocal);

    // ── Local traits cannot have visibility modifiers ────────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local trait cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::Trait),
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Push generic parameters so method signatures can refer to T ──────────
    if (!node.genericParams.empty() && ctx.dispatcher) {
        ctx.dispatcher->pushGenericParams(&node.genericParams);
    }

    // ── Check methods: duplicate names, resolve parameter/return types ────────
    std::unordered_set<std::string> seenMethodNames;

    for (auto& method : node.methods) {
        if (!method || !method->funcType) continue;

        std::string methodName = std::string(ctx.pool.lookup(method->name));

        // Check for duplicate method names
        if (!seenMethodNames.insert(methodName).second) {
            ctx.error(method->loc, DiagCode::E2005,
                      "duplicate method '", methodName, "' in trait '", ctx.pool.lookup(node.name), "'");
            continue;
        }

        LUC_LOG_SEMANTIC_EXTREME("\tchecking trait method: " << methodName);

        const FuncSignature& sig = method->funcType->sig;

        // Resolve parameter types (flattened)
        for (const auto& param : sig.allParams) {
            if (!param) continue;
            if (param->type) {
                TypeAST* paramType = ctx.dispatcher ? ctx.dispatcher->resolveType(param->type.get()) : param->type.get();
                if (!paramType) {
                    ctx.error(param->loc, DiagCode::E2001,
                              "cannot resolve type for parameter '", ctx.pool.lookup(param->name),
                              "' in trait method '", methodName, "'");
                }
            }
        }

        // Resolve return types
        for (auto& retType : sig.returnTypes) {
            if (retType) {
                TypeAST* resolvedRet = ctx.dispatcher ? ctx.dispatcher->resolveType(retType.get()) : retType.get();
                if (!resolvedRet) {
                    ctx.error(method->loc, DiagCode::E2001,
                              "cannot resolve return type for trait method '", methodName, "'");
                }
            }
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty() && ctx.dispatcher) {
        ctx.dispatcher->popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkTraitDecl: complete for " << ctx.pool.lookup(node.name)
                             << " with " << seenMethodNames.size() << " methods");
}