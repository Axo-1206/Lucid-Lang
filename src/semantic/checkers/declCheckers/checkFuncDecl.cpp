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

// ─────────────────────────────────────────────────────────────────────────────
// checkFuncDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local functions are private.
//   - Validate attributes (@extern, @inline, @deprecated, etc.).
//   - @extern functions: no body, must be const (checked by AttributeRegistry),
//     store extern metadata on symbol.
//   - Push a new scope for parameters.
//   - Declare each parameter from the flattened allParams, respecting group sizes.
//   - Check the body with the expected return type.
//   - Handle async/parallel qualifiers: increment parallelDepth if parallel,
//     track async context (if needed for Future type, but not stored here).
// ─────────────────────────────────────────────────────────────────────────────
void checkFuncDecl(FuncDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkFuncDecl: name=" << ctx.pool.lookup(node.name)
                     << ", isLocal=" << isLocal);

    // ── Local functions cannot have visibility modifiers ─────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local function cannot have visibility modifier (pub/export)");
    }

    // ── Attribute validation ─────────────────────────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::Func),
                    std::string(ctx.pool.lookup(node.name)), node.keyword,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── @extern function handling ────────────────────────────────────────────
    if (attrIsExtern) {
        if (node.body) {
            ctx.error(node.loc, DiagCode::E2002,
                      "@extern function '", ctx.pool.lookup(node.name), "' must not have a body");
        }
        Symbol* sym = ctx.symbols->lookup(node.name);
        if (sym) {
            sym->isExtern = true;
            sym->externSymbol = ctx.pool.intern(attrExternSym);
            sym->callingConv = ctx.pool.intern(attrCallingConv);
        }
        return;
    }

    // ── Determine expected return type (single return for now) ───────────────
    TypeAST* expectedReturn = nullptr;
    if (node.funcType && !node.funcType->sig.returnTypes.empty()) {
        expectedReturn = node.funcType->sig.returnTypes[0].get();
    }

    // ── Push generic parameters onto resolver stack (if any) ─────────────────
    if (!node.genericParams.empty() && ctx.resolver) {
        ctx.resolver->pushGenericParams(&node.genericParams);
    }

    // ── Track async/parallel qualifiers ──────────────────────────────────────
    bool isAsync = node.funcType && node.funcType->isAsync();
    bool isParallel = node.funcType && node.funcType->isParallel();

    if (isParallel) {
        ctx.enterParallel();
    }

    // ── Push a new scope for parameters ──────────────────────────────────────
    ctx.symbols->pushScope();

    // ── Declare parameters using flattened allParams ─────────────────────────
    if (node.funcType) {
        const FuncSignature& sig = node.funcType->sig;
        for (const auto& param : sig.allParams) {
            if (!param) continue;

            TypeAST* paramType = param->type.get();
            if (!paramType && ctx.resolver) {
                paramType = ctx.resolver->resolveType(param->type.get());
                if (!paramType) {
                    ctx.error(param->loc, DiagCode::E2001,
                              "cannot resolve type for parameter '", ctx.pool.lookup(param->name), "'");
                    continue;
                }
            }

            Symbol sym;
            sym.name = param->name;
            sym.kind = SymbolKind::Param;
            sym.declKw = DeclKeyword::Let;
            sym.visibility = Visibility::Private;
            sym.type = paramType;
            sym.decl = param.get();
            sym.loc = param->loc;
            if (!ctx.symbols->declare(sym)) {
                ctx.error(param->loc, DiagCode::E2005,
                          "duplicate parameter name '", ctx.pool.lookup(param->name), "'");
            }
        }
    }

    // ── Check function body (if present) ─────────────────────────────────────
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    } else {
        ctx.error(node.loc, DiagCode::E2003,
                  "function '", ctx.pool.lookup(node.name), "' must have a body");
    }

    // ── Pop scopes and stack ─────────────────────────────────────────────────
    ctx.symbols->popScope();

    if (isParallel) {
        ctx.exitParallel();
    }

    if (!node.genericParams.empty() && ctx.resolver) {
        ctx.resolver->popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkFuncDecl: complete for " << ctx.pool.lookup(node.name));
}