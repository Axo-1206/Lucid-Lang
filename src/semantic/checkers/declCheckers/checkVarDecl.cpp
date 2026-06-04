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
// checkVarDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local variables are private.
//   - Resolve type annotation; error if unresolved.
//   - const requires an initialiser that is a compile-time constant expression.
//   - If an initialiser is present, its type must be assignable to the annotation.
//   - nil literal is only assignable to nullable types.
//   - const does not allow nil (nil is never a compile-time constant).
//   - @extern variable: no initialiser, must be const, type must be resolved.
// ─────────────────────────────────────────────────────────────────────────────
void checkVarDecl(VarDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkVarDecl: name=" << ctx.pool.lookup(node.name)
                     << " kw=" << static_cast<int>(node.keyword)
                     << ", isLocal=" << isLocal);

    // ── Local declarations cannot have visibility modifiers ───────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E2005, "local variable cannot have visibility modifier (pub/export)");
    }

    // ── Attribute validation (@extern, @deprecated, etc.) ─────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, static_cast<uint32_t>(AttributeContext::Var),
                    std::string(ctx.pool.lookup(node.name)), node.keyword,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── @extern variable validation ───────────────────────────────────────────
    if (attrIsExtern) {
        if (node.init) {
            ctx.error(node.loc, DiagCode::E2002,
                      "@extern variable '", ctx.pool.lookup(node.name),
                      "' must not have an initialiser — the symbol is resolved by the linker");
            return;
        }
        if (node.keyword != DeclKeyword::Const) {
            ctx.error(node.loc, DiagCode::E2002, "@extern variable must be declared with 'const'");
            return;
        }
        
        // Set insideExtern flag for type resolution (permits raw pointers)
        ctx.enterExtern();
        TypeAST* declaredType = ctx.resolver ? ctx.resolver->resolveType(node.type.get()) : nullptr;
        ctx.exitExtern();
        
        if (!declaredType) return;

        // Store extern metadata on symbol
        Symbol* sym = ctx.symbols->lookup(node.name);
        if (sym) {
            sym->isExtern = true;
            sym->externSymbol = ctx.pool.intern(attrExternSym);
            sym->callingConv = ctx.pool.intern(attrCallingConv);
            sym->type = declaredType;
        }
        return;
    }

    // ── Resolve declared type ─────────────────────────────────────────────────
    TypeAST* declaredType = ctx.resolver ? ctx.resolver->resolveType(node.type.get()) : nullptr;
    if (!declaredType) return;

    // ── No initialiser ────────────────────────────────────────────────────────
    if (!node.init) {
        if (node.keyword == DeclKeyword::Const) {
            ctx.error(node.loc, DiagCode::E2002,
                      "const '", ctx.pool.lookup(node.name), "' must have an initialiser");
        } else if (!TypeChecker::isNullable(declaredType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002,
                      "variable '", ctx.pool.lookup(node.name),
                      "' must have an initial value because it is not nullable");
        }
        Symbol* sym = ctx.symbols->lookup(node.name);
        if (sym && !sym->type) sym->type = declaredType;
        return;
    }

    // ── Check initialiser expression ─────────────────────────────────────────
    TypeAST* initType = checkExpr(node.init.get(), ctx);
    if (!initType) return;

    // ── nil assignment to non‑nullable type ───────────────────────────────────
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !TypeChecker::isNullable(declaredType, ctx)) {
            ctx.error(node.loc, DiagCode::E2002,
                      "nil cannot be assigned to non-nullable type '",
                      LucDebug::formatType(declaredType, ctx.pool), "'");
            return;
        }
    }

    // ── const requires compile‑time constant ──────────────────────────────────
    if (node.keyword == DeclKeyword::Const && !isConstExpr(node.init.get(), ctx)) {
        ctx.error(node.loc, DiagCode::E2002,
                  "const '", ctx.pool.lookup(node.name),
                  "' initialiser must be a compile‑time constant expression");
    }

    // ── Type assignability check, with optional from‑casting ──────────────────
    if (!TypeChecker::isAssignable(initType, declaredType, ctx)) {
        // Try to find a `from` conversion from initType to declaredType
        Symbol* fromCast = TypeChecker::isFromCastable(initType, declaredType, ctx);
        if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
            // Rewrite the initialiser as an explicit cast
            auto targetTypeNode = ctx.arena.make<NamedTypeAST>(
                declaredType->as<NamedTypeAST>()->name);
            targetTypeNode->loc = node.loc;
            auto convExpr = ctx.arena.make<TypeConvExprAST>(
                std::move(targetTypeNode), std::move(node.init), false);
            convExpr->loc = node.init->loc;
            node.init = std::move(convExpr);
            checkExpr(node.init.get(), ctx);
        } else {
            ctx.error(node.loc, DiagCode::E2008,
                      "cannot implicitly convert initializer to type '",
                      LucDebug::formatType(declaredType, ctx.pool),
                      "' for variable '", ctx.pool.lookup(node.name),
                      "'; use an explicit type cast like '[target_type](value)' "
                      "or define a 'from' casting block");
        }
    }

    // ── Update symbol type (if not already set) ───────────────────────────────
    Symbol* sym = ctx.symbols->lookup(node.name);
    if (sym && !sym->type) {
        sym->type = declaredType;
    }
}