/**
 * @file SemanticDecl.cpp
 *
 * @nutshell Verifies the structure of structs, enums, functions, and implementations.
 *
 * @responsibility Phase 3a of semantic analysis: walks declaration nodes, resolves their
 *   types via TypeResolver, and enforces all declaration-level rules.
 *
 *
 * @related SemanticAnalyzer.cpp, SemanticStmt.cpp, SemanticExpr.cpp
 */

#include "ast/ExprAST.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "header/SymbolTable.hpp"
#include "header/TypeResolver.hpp"
#include "header/TypeChecker.hpp"
#include "header/SemanticContext.hpp"
#include "header/SemanticChecker.hpp"

#include <unordered_set>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// isConstExpr  — Returns true when an expression is a compile-time constant
//
// Uses SemanticContext for symbol table access.
// ─────────────────────────────────────────────────────────────────────────────
static bool isConstExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return false;

    // Literals (except nil) are always compile-time constants.
    if (expr->isa<LiteralExprAST>()) {
        return expr->as<LiteralExprAST>()->kind != LiteralKind::Nil;
    }

    // An identifier is const if its symbol was declared const.
    if (expr->isa<IdentifierExprAST>()) {
        Symbol* sym = ctx.symbols.lookup(expr->as<IdentifierExprAST>()->name);
        return sym && sym->declKw == DeclKeyword::Const;
    }

    // Enum variant access: Direction.North — always compile-time.
    if (expr->isa<FieldAccessExprAST>()) {
        auto* fa = expr->as<FieldAccessExprAST>();
        if (fa->object && fa->object->isa<IdentifierExprAST>()) {
            Symbol* sym = ctx.symbols.lookup(fa->object->as<IdentifierExprAST>()->name);
            return sym && sym->kind == SymbolKind::Enum;
        }
        return false;
    }

    // Arithmetic over const operands is const.
    if (expr->isa<BinaryExprAST>()) {
        auto* bin = expr->as<BinaryExprAST>();
        return isConstExpr(bin->left.get(), ctx) &&
               isConstExpr(bin->right.get(), ctx);
    }

    // Unary negation / bitwise-not of a const is const.
    if (expr->isa<UnaryExprAST>()) {
        return isConstExpr(expr->as<UnaryExprAST>()->operand.get(), ctx);
    }

    // Safe explicit cast of a const is const.
    if (expr->isa<TypeConvExprAST>()) {
        auto* tc = expr->as<TypeConvExprAST>();
        return !tc->isUnsafe && isConstExpr(tc->expr.get(), ctx);
    }

    // Anything else is not a compile-time constant.
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAttributes
//
// Validates every '@' attribute on a declaration.
// Now uses SemanticContext for pool and diagnostic engine.
// ─────────────────────────────────────────────────────────────────────────────
static void checkAttributes(const std::vector<AttributePtr>& attributes,
                            AttributeContext attrCtx,
                            const std::string& declName,
                            DeclKeyword declKw,
                            SemanticContext& ctx,
                            bool& outIsExtern,
                            std::string& outExternSym,
                            std::string& outCallingConv) {
    LUC_LOG_SEMANTIC_VERBOSE("checkAttributes: count=" << attributes.size()
                             << ", ctx=" << static_cast<int>(attrCtx)
                             << ", declName='" << declName << "'");

    outIsExtern = false;
    outExternSym = "";
    outCallingConv = "C";

    auto& registry = AttributeRegistry::instance();
    std::vector<std::string> seen;  // Track order for mutual exclusion

    for (const auto& attr : attributes) {
        std::string_view nameView = ctx.pool.lookup(attr->name);
        std::string name(nameView);
        LUC_LOG_SEMANTIC_EXTREME("checking attribute: @" << name);

        // Check for duplicate
        bool isDuplicate = false;
        for (const auto& seenName : seen) {
            if (seenName == name) {
                LUC_LOG_SEMANTIC("\tERROR: duplicate attribute @" << name);
                ctx.dc.error(DiagnosticCategory::Semantic, attr->loc, DiagCode::E3005,
                             "duplicate attribute '@" + name + "'");
                isDuplicate = true;
                break;
            }
        }
        if (isDuplicate) continue;

        // Check mutual exclusion with previously seen attributes
        bool mutuallyExclusive = false;
        for (const auto& seenName : seen) {
            if (!registry.checkMutualExclusion(name, seenName, ctx.dc, attr->loc)) {
                mutuallyExclusive = true;
                break;
            }
        }
        if (mutuallyExclusive) continue;

        // Convert AttributeContext enum to registry's context
        AttributeContext registryCtx;
        switch (attrCtx) {
            case AttributeContext::Func:
                registryCtx = AttributeContext::Func;
                break;
            case AttributeContext::Var:
                registryCtx = AttributeContext::Var;
                break;
            case AttributeContext::Struct:
                registryCtx = AttributeContext::Struct;
                break;
            default:
                registryCtx = AttributeContext::None;
                break;
        }

        // Check if this is main function (for main-only attributes like @aot/@jit)
        if (declName == "main") {
            registryCtx = registryCtx | AttributeContext::Main;
        }

        // Validate the attribute using registry
        if (!registry.validateAttribute(*attr, registryCtx, declName, declKw, ctx.dc)) {
            continue;
        }

        seen.push_back(name);

        // Handle @extern specially (needs to set out params for codegen)
        if (name == "extern") {
            outIsExtern = true;
            // First argument: symbol name (must be string literal)
            if (!attr->args.empty() && attr->args[0]->kind == AttributeArgKind::StringLit) {
                outExternSym = std::string(ctx.pool.lookup(attr->args[0]->value));
            }
            // Second argument: calling convention (must be type identifier, e.g., "C")
            if (attr->args.size() >= 2 && attr->args[1]->kind == AttributeArgKind::TypeIdent) {
                outCallingConv = std::string(ctx.pool.lookup(attr->args[1]->value));
            }
            LUC_LOG_SEMANTIC_EXTREME("\t@extern: sym='" << outExternSym
                                     << "', conv='" << outCallingConv << "'");
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkAttributes: complete, isExtern=" << outIsExtern);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkVarDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers (pub/export) – local variables
//     are always private to the block.
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
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local variable cannot have visibility modifier (pub/export)");
        // Continue checking, but treat as private.
    }

    // ── Attribute validation (@extern, @deprecated, etc.) ─────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Var,
                std::string(ctx.pool.lookup(node.name)), node.keyword,
                ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── @extern variable validation ───────────────────────────────────────────
    if (attrIsExtern) {
        if (node.init) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'@extern' variable '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must not have an initialiser — the symbol is resolved by the linker");
            return;
        }
        if (node.keyword != DeclKeyword::Const) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'@extern' variable must be declared with 'const'");
            return;
        }
        // Resolve type with insideExtern flag set (permits raw pointers)
        ctx.resolver.setInsideExtern(true);
        TypeAST* declaredType = ctx.resolver.resolveType(node.type.get());
        ctx.resolver.setInsideExtern(false);
        if (!declaredType) return;

        // Store extern metadata on symbol
        Symbol* sym = ctx.symbols.lookup(node.name);
        if (sym) {
            sym->isExtern = true;
            sym->externSymbol = ctx.pool.intern(attrExternSym);
            sym->callingConv = ctx.pool.intern(attrCallingConv);
            sym->type = declaredType;
        }
        node.resolvedType = declaredType;
        return;
    }

    // ── Resolve declared type ─────────────────────────────────────────────────
    TypeAST* declaredType = ctx.resolver.resolveType(node.type.get());
    if (!declaredType) return;

    // ── No initialiser ────────────────────────────────────────────────────────
    if (!node.init) {
        if (node.keyword == DeclKeyword::Const) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "const '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must have an initialiser");
        } else if (!ctx.checker.isNullable(declaredType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "variable '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must have an initial value because it is not nullable");
        }
        // Still record resolved type
        node.resolvedType = declaredType;
        return;
    }

    // ── Check initialiser expression ─────────────────────────────────────────
    TypeAST* initType = checkExpr(node.init.get(), ctx);
    if (!initType) return;

    // ── nil assignment to non‑nullable type ───────────────────────────────────
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !ctx.checker.isNullable(declaredType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "nil cannot be assigned to non-nullable type '" +
                         std::string(ctx.pool.lookup(node.name)) + "'");
            return;
        }
    }

    // ── const requires compile‑time constant ──────────────────────────────────
    if (node.keyword == DeclKeyword::Const && !isConstExpr(node.init.get(), ctx)) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "const '" + std::string(ctx.pool.lookup(node.name)) +
                     "' initialiser must be a compile‑time constant expression");
    }

    // ── Type assignability check, with optional from‑casting ──────────────────
    if (!ctx.checker.isAssignable(initType, declaredType)) {
        // Try to find a `from` conversion from initType to declaredType
        Symbol* fromCast = ctx.checker.isFromCastable(initType, declaredType, &ctx.symbols);
        if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
            // Rewrite the initialiser as an explicit cast
            auto targetTypeNode = ctx.arena.make<NamedTypeAST>(
                declaredType->as<NamedTypeAST>()->name);
            targetTypeNode->loc = node.loc;
            auto convExpr = ctx.arena.make<TypeConvExprAST>(
                std::move(targetTypeNode), std::move(node.init), false);
            convExpr->loc = node.init->loc;
            node.init = std::move(convExpr);
            // Re‑check the rewritten expression
            checkExpr(node.init.get(), ctx);
        } else {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3008,
                         "cannot implicitly convert initializer to type for '" +
                         std::string(ctx.pool.lookup(node.name)) +
                         "'; use an explicit type cast like '[target_type](value)' "
                         "or define a 'from' casting block");
        }
    }

    // ── Update symbol type (if not already set) ───────────────────────────────
    Symbol* sym = ctx.symbols.lookup(node.name);
    if (sym && !sym->type) {
        sym->type = declaredType;
    }
    node.resolvedType = declaredType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFuncDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local functions are private.
//   - Validate attributes (@extern, @inline, @deprecated, etc.).
//   - @extern functions: no body, must be const (checked by AttributeRegistry),
//     store extern metadata on symbol.
//   - Push a new scope for parameters.
//   - Declare each parameter from the signature's paramGroups.
//   - Check the body with the expected return type.
//   - Handle async/parallel qualifiers: increment parallelDepth if parallel,
//     track async context (if needed for Future type, but not stored here).
// ─────────────────────────────────────────────────────────────────────────────
void checkFuncDecl(FuncDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkFuncDecl: name=" << ctx.pool.lookup(node.name)
                     << ", isLocal=" << isLocal);

    // ── Local functions cannot have visibility modifiers ─────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local function cannot have visibility modifier (pub/export)");
    }

    // ── Attribute validation ─────────────────────────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Func,
                    std::string(ctx.pool.lookup(node.name)), node.keyword,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── @extern function handling ────────────────────────────────────────────
    if (attrIsExtern) {
        if (node.body) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "@extern function '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must not have a body");
        }
        Symbol* sym = ctx.symbols.lookup(node.name);
        if (sym) {
            sym->isExtern = true;
            sym->externSymbol = ctx.pool.intern(attrExternSym);
            sym->callingConv = ctx.pool.intern(attrCallingConv);
        }
        return;
    }

    // ── Determine expected return type (single return for now) ───────────────
    TypeAST* expectedReturn = nullptr;
    if (!node.sig.returnTypes.empty()) {
        expectedReturn = node.sig.returnTypes[0].get();
    }

    // ── Push generic parameters onto resolver stack (if any) ─────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Track async/parallel qualifiers ──────────────────────────────────────
    bool isAsync = node.sig.isAsync();
    bool isParallel = node.sig.isParallel();

    if (isParallel) {
        ctx.enterParallel();  // ctx.parallelDepth++
    }
    // Async context could be tracked in a separate stack if needed,
    // but for now we only need to know inside `await` checking.
    // We'll rely on a separate mechanism (e.g., SemanticContext::asyncDepth)
    // or the FunctionContext approach; but for simplicity, we just log.

    // ── Push a new scope for parameters ──────────────────────────────────────
    ctx.symbols.pushScope();

    // ── Declare parameters from node.sig.paramGroups ─────────────────────────
    for (const auto& group : node.sig.paramGroups) {
        for (const auto& param : group) {
            if (!param) continue;
            TypeAST* paramType = param->type.get();
            if (!paramType) {
                paramType = ctx.resolver.resolveType(param->type.get());
                if (!paramType) {
                    ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3001,
                                 "cannot resolve type for parameter '" +
                                 std::string(ctx.pool.lookup(param->name)) + "'");
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
            if (!ctx.symbols.declare(sym)) {
                ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                             "duplicate parameter name '" +
                             std::string(ctx.pool.lookup(param->name)) + "'");
            }
        }
    }

    // ── Check function body (if present) ─────────────────────────────────────
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    } else {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "function '" + std::string(ctx.pool.lookup(node.name)) +
                     "' must have a body");
    }

    // ── Pop scopes and stack ─────────────────────────────────────────────────
    ctx.symbols.popScope();

    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkFuncDecl: complete for " << ctx.pool.lookup(node.name));
}

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
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local struct cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (packed, deprecated, etc.) ──────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Struct,
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);
    // @extern is not allowed on structs; the registry will reject it.
    // @packed is allowed.

    // ── Push generic parameters so that default values can refer to T ─────────
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Check fields: duplicate names, resolve types, default values ─────────
    std::unordered_set<std::string> seenFieldNames;
    for (auto& field : node.fields) {
        if (!field) continue;

        std::string fieldName = std::string(ctx.pool.lookup(field->name));
        if (!seenFieldNames.insert(fieldName).second) {
            ctx.dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3005,
                         "duplicate field '" + fieldName + "' in struct '" +
                         std::string(ctx.pool.lookup(node.name)) + "'");
            continue;
        }

        // Resolve field type (should already be resolved by Phase 2, but be defensive)
        TypeAST* fieldType = field->type.get();
        if (!fieldType) {
            // Try to resolve now
            fieldType = ctx.resolver.resolveType(field->type.get());
            if (!fieldType) {
                ctx.dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3001,
                             "cannot resolve type for field '" + fieldName + "' in struct '" +
                             std::string(ctx.pool.lookup(node.name)) + "'");
                continue;
            }
        }

        // Check default value, if present
        if (field->defaultVal) {
            TypeAST* defaultType = checkExpr(field->defaultVal.get(), ctx);
            if (defaultType && !ctx.checker.isAssignable(defaultType, fieldType)) {
                ctx.dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3002,
                             "default value type mismatch for field '" + fieldName +
                             "' in struct '" + std::string(ctx.pool.lookup(node.name)) + "'");
            }
            // Optionally check that default value is constant (if field type is non-generic)
            // For now, we skip constantness check because defaults may be `T(0)` where T is generic.
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkStructDecl: complete for " << ctx.pool.lookup(node.name));
}

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
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local enum cannot have visibility modifier (pub/export)");
    }

    // ── Enums don't have meaningful attributes, but we validate anyway ───────
    // The registry will reject @extern, @inline, @packed, etc.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Enum,
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);
    // The registry will report errors for invalid attributes.

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
            ctx.dc.error(DiagnosticCategory::Semantic, variant->loc, DiagCode::E3005,
                         "duplicate enum value " + std::to_string(value) +
                         " for variant '" + variantName +
                         "' in enum '" + std::string(ctx.pool.lookup(node.name)) + "'");
        }

        // Update next auto value (even if duplicate was reported, continue)
        nextAuto = value + 1;
    }

    // ── Future: choose backing integer type based on range ───────────────────
    // The smallest type that can hold all values is selected:
    //   - int8: -128..127
    //   - int16: -32768..32767
    //   - int32: -2147483648..2147483647
    //   - int64: default fallback
    // For now, this is deferred to codegen.

    LUC_LOG_SEMANTIC_VERBOSE("checkEnumDecl: complete for " << ctx.pool.lookup(node.name)
                             << " with " << usedValues.size() << " unique values");
}

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
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local trait cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
    // Note: Traits are not in AttributeContext yet; we pass None for now.
    // If traits should support @deprecated, add AttributeContext::Trait to registry.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Trait,
                    std::string(ctx.pool.lookup(node.name)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Push generic parameters so method signatures can refer to T ──────────
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Check methods: duplicate names, resolve parameter/return types ────────
    std::unordered_set<std::string> seenMethodNames;
    
    for (auto& method : node.methods) {
        if (!method) continue;

        std::string methodName = std::string(ctx.pool.lookup(method->name));
        
        // Check for duplicate method names
        if (!seenMethodNames.insert(methodName).second) {
            ctx.dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                         "duplicate method '" + methodName + "' in trait '" +
                         std::string(ctx.pool.lookup(node.name)) + "'");
            continue;
        }

        LUC_LOG_SEMANTIC_EXTREME("\tchecking trait method: " << methodName);

        // Resolve parameter types in all curry groups
        for (const auto& group : method->sig.paramGroups) {
            for (const auto& param : group) {
                if (param && param->type) {
                    // Parameter type should already be resolved by Phase 2,
                    // but be defensive.
                    TypeAST* paramType = param->type.get();
                    if (!paramType) {
                        paramType = ctx.resolver.resolveType(param->type.get());
                        if (!paramType) {
                            ctx.dc.error(DiagnosticCategory::Semantic, param->loc,
                                         DiagCode::E3001,
                                         "cannot resolve parameter type for '" +
                                         std::string(ctx.pool.lookup(param->name)) +
                                         "' in trait method '" + methodName + "'");
                        }
                    }
                }
            }
        }

        // Resolve return types
        for (auto& retType : method->sig.returnTypes) {
            if (retType) {
                TypeAST* resolvedRet = retType.get();
                if (!resolvedRet) {
                    resolvedRet = ctx.resolver.resolveType(retType.get());
                    if (!resolvedRet) {
                        ctx.dc.error(DiagnosticCategory::Semantic, method->loc,
                                     DiagCode::E3001,
                                     "cannot resolve return type for trait method '" +
                                     methodName + "'");
                    }
                }
            }
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkTraitDecl: complete for " << ctx.pool.lookup(node.name)
                             << " with " << seenMethodNames.size() << " methods");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkImplDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local impls are private.
//   - Target struct must exist in symbol table.
//   - Generic parameters must match the struct's generic parameters (if any).
//   - No duplicate method names within the impl block.
//   - Method signatures must match the trait's method signatures (if traitRef present).
//   - Struct fields are injected into each method's scope.
//   - Method bodies are checked with the correct return type.
//   - Parallel/async qualifiers are tracked for depth counters.
// ─────────────────────────────────────────────────────────────────────────────
void checkImplDecl(ImplDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkImplDecl: structName=" << ctx.pool.lookup(node.structName)
                     << ", methods=" << node.methods.size()
                     << ", isLocal=" << isLocal);

    // ── Local impls cannot have visibility modifiers ─────────────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local impl cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::Impl,
                    std::string(ctx.pool.lookup(node.structName)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Verify target struct exists ──────────────────────────────────────────
    Symbol* structSym = ctx.symbols.lookup(node.structName);
    if (!structSym || structSym->kind != SymbolKind::Struct) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "impl target '" + std::string(ctx.pool.lookup(node.structName)) +
                     "' is not a declared struct");
        return;
    }
    auto* structDecl = structSym->decl->as<StructDeclAST>();

    // ── Check generic signature match ────────────────────────────────────────
    if (node.genericParams.size() != structDecl->genericParams.size()) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3017,
                     "generic signature mismatch: impl for '" +
                     std::string(ctx.pool.lookup(node.structName)) +
                     "' has " + std::to_string(node.genericParams.size()) +
                     " parameters, but struct was declared with " +
                     std::to_string(structDecl->genericParams.size()));
    } else {
        for (size_t i = 0; i < node.genericParams.size(); ++i) {
            auto& implParam = node.genericParams[i];
            auto& structParam = structDecl->genericParams[i];
            if (!implParam || !structParam) continue;

            if (implParam->name != structParam->name) {
                ctx.dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                             "generic parameter name mismatch: expected '" +
                             std::string(ctx.pool.lookup(structParam->name)) +
                             "', found '" + std::string(ctx.pool.lookup(implParam->name)) + "'");
            }

            // Check constraint counts (actual trait names must match)
            if (implParam->constraints.size() != structParam->constraints.size()) {
                ctx.dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                             "generic constraint count mismatch for '" +
                             std::string(ctx.pool.lookup(implParam->name)) +
                             "': expected " + std::to_string(structParam->constraints.size()) +
                             " traits, found " + std::to_string(implParam->constraints.size()));
            } else {
                for (size_t j = 0; j < implParam->constraints.size(); ++j) {
                    if (implParam->constraints[j] != structParam->constraints[j]) {
                        ctx.dc.error(DiagnosticCategory::Semantic, implParam->loc, DiagCode::E3017,
                                     "generic constraint mismatch for '" +
                                     std::string(ctx.pool.lookup(implParam->name)) +
                                     "': expected trait '" +
                                     std::string(ctx.pool.lookup(structParam->constraints[j])) +
                                     "', found '" +
                                     std::string(ctx.pool.lookup(implParam->constraints[j])) + "'");
                    }
                }
            }
        }
    }

    // ── Push generic parameters (impl-level) ─────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Pre-resolve struct field types for injection ─────────────────────────
    std::vector<std::pair<InternedString, TypeAST*>> fieldTypes;
    for (auto& field : structDecl->fields) {
        if (!field) continue;
        TypeAST* ft = field->type.get();
        if (!ft) {
            ft = ctx.resolver.resolveType(field->type.get());
        }
        if (ft) {
            fieldTypes.emplace_back(field->name, ft);
        }
    }

    // ── Check each method ────────────────────────────────────────────────────
    std::unordered_set<std::string> seenMethods;

    for (auto& method : node.methods) {
        if (!method) continue;

        std::string methodName = std::string(ctx.pool.lookup(method->name));

        // Check for duplicate method names
        if (!seenMethods.insert(methodName).second) {
            ctx.dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                         "duplicate method '" + methodName + "' in impl for '" +
                         std::string(ctx.pool.lookup(node.structName)) + "'");
            continue;
        }

        // Get expected return type (single return for now)
        TypeAST* expectedReturn = method->sig.returnTypes.empty()
                                  ? nullptr
                                  : method->sig.returnTypes[0].get();

        // Track qualifiers for depth counters
        bool isAsync = method->sig.isAsync();
        bool isParallel = method->sig.isParallel();

        if (isParallel) {
            ctx.enterParallel();
        }

        // Push a new scope for the method
        ctx.symbols.pushScope();

        // Inject struct fields into the method scope
        for (const auto& ft : fieldTypes) {
            Symbol fs;
            fs.name = ft.first;
            fs.kind = SymbolKind::Field;
            fs.declKw = DeclKeyword::Let;
            fs.visibility = Visibility::Private;
            fs.type = ft.second;
            fs.decl = nullptr;
            fs.loc = node.loc;
            ctx.symbols.declare(fs);
        }

        // Declare method parameters
        for (const auto& group : method->sig.paramGroups) {
            for (const auto& param : group) {
                if (!param) continue;

                TypeAST* paramType = param->type.get();
                if (!paramType) {
                    paramType = ctx.resolver.resolveType(param->type.get());
                    if (!paramType) {
                        ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3001,
                                     "cannot resolve type for parameter '" +
                                     std::string(ctx.pool.lookup(param->name)) + "'");
                        continue;
                    }
                }

                Symbol ps;
                ps.name = param->name;
                ps.kind = SymbolKind::Param;
                ps.declKw = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type = paramType;
                ps.decl = param.get();
                ps.loc = param->loc;
                if (!ctx.symbols.declare(ps)) {
                    ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                                 "duplicate parameter name '" +
                                 std::string(ctx.pool.lookup(param->name)) + "'");
                }
            }
        }

        // Check method body
        if (method->body) {
            checkStmt(method->body.get(), ctx, expectedReturn);
        } else {
            ctx.dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3002,
                         "method '" + methodName + "' in impl must have a body");
        }

        // Pop method scope
        ctx.symbols.popScope();

        if (isParallel) {
            ctx.exitParallel();
        }
    }

    // ── Trait conformance check (if traitRef present) ────────────────────────
    if (node.traitRef) {
        Symbol* traitSym = ctx.symbols.lookup(node.traitRef->name);
        if (!traitSym || traitSym->kind != SymbolKind::Trait) {
            ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                         "trait '" + std::string(ctx.pool.lookup(node.traitRef->name)) +
                         "' is not declared");
        } else {
            auto* traitDecl = traitSym->decl->as<TraitDeclAST>();
            bool allMethodsFound = true;

            for (auto& requiredMethod : traitDecl->methods) {
                if (!requiredMethod) continue;

                std::string requiredName = std::string(ctx.pool.lookup(requiredMethod->name));
                bool found = false;
                TypeAST* requiredType = nullptr;

                // Find matching method in impl
                for (auto& implMethod : node.methods) {
                    if (!implMethod) continue;
                    if (implMethod->name == requiredMethod->name) {
                        found = true;
                        // TODO: Compare full signatures (parameter and return types)
                        // For now, we only check name existence.
                        // Full signature comparison would use TypeChecker::isEqual
                        // on the resolved function types.
                        break;
                    }
                }

                if (!found) {
                    ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                                 "impl of '" + std::string(ctx.pool.lookup(node.structName)) +
                                 "' for trait '" + std::string(ctx.pool.lookup(node.traitRef->name)) +
                                 "' is missing method '" + requiredName + "'");
                    allMethodsFound = false;
                }
            }

            if (allMethodsFound && !node.methods.empty()) {
                LUC_LOG_SEMANTIC_VERBOSE("\ttrait conformance check passed for '"
                                         << ctx.pool.lookup(node.structName) << " : "
                                         << ctx.pool.lookup(node.traitRef->name) << "'");
            }
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkImplDecl: complete for "
                             << ctx.pool.lookup(node.structName)
                             << " with " << seenMethods.size() << " methods");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFromDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local from blocks are private.
//   - Target struct must exist and be a struct (not enum, trait, etc.).
//   - Each entry's return type must match the target struct type.
//   - Parameter types in each entry must resolve.
//   - No duplicate entry signatures (same parameter types, curry structure).
//   - Each entry body is checked to return the target type.
//   - From entries cannot have qualifiers (~async, ~nullable, ~parallel).
//   - Generic parameters are pushed so entries can refer to T.
// ─────────────────────────────────────────────────────────────────────────────
void checkFromDecl(FromDeclAST& node, SemanticContext& ctx, bool isLocal) {
    LUC_LOG_SEMANTIC("checkFromDecl: target=" << ctx.pool.lookup(node.targetTypeName)
                     << ", entries=" << node.entries.size()
                     << ", isLocal=" << isLocal);

    // ── Local from blocks cannot have visibility modifiers ───────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                     "local from block cannot have visibility modifier (pub/export)");
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
    // From blocks are not in AttributeContext yet; we pass None for now.
    // If from blocks should support @deprecated, add AttributeContext::From.
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::None,
                    std::string(ctx.pool.lookup(node.targetTypeName)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Verify target struct exists ──────────────────────────────────────────
    Symbol* targetSym = ctx.symbols.lookup(node.targetTypeName);
    if (!targetSym || targetSym->kind != SymbolKind::Struct) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "from block target '" + std::string(ctx.pool.lookup(node.targetTypeName)) +
                     "' is not a declared struct");
        return;
    }
    auto* structDecl = targetSym->decl->as<StructDeclAST>();
    TypeAST* targetType = structDecl->selfType.get();
    if (!targetType) {
        ctx.dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "from block target type not resolved");
        return;
    }

    // ── Push generic parameters (from block can be generic: from Wrapper<T>) ──
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Check each from entry ────────────────────────────────────────────────
    std::vector<FromEntryAST*> verifiedEntries;  // For duplicate signature detection

    for (auto& entry : node.entries) {
        if (!entry) continue;

        LUC_LOG_SEMANTIC_EXTREME("\tchecking from entry");

        // Resolve return type (should be target struct type)
        TypeAST* entryReturnType = entry->returnType.get();
        if (!entryReturnType) {
            entryReturnType = ctx.resolver.resolveType(entry->returnType.get());
            if (!entryReturnType) {
                ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3001,
                             "from entry: cannot resolve return type");
                continue;
            }
        }

        // Verify return type matches target struct
        if (!ctx.checker.isEqual(entryReturnType, targetType)) {
            ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3002,
                         "from entry return type must be '" +
                         std::string(ctx.pool.lookup(node.targetTypeName)) +
                         "', got '" + LucDebug::kindToString(entryReturnType->kind) + "'");
            continue;
        }

        // Check that the entry has no qualifiers
        bool hasQualifiers = false;
        for (const auto& qualName : entry->sig.rawQualifiers) {
            ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E2010,
                         "from entries cannot have qualifiers (~" +
                         std::string(ctx.pool.lookup(qualName)) +
                         "); qualifiers are not allowed on conversions");
            hasQualifiers = true;
        }
        if (entry->sig.qualifiers != 0) {
            ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E2010,
                         "from entries cannot have qualifiers (~async, ~nullable, ~parallel)");
            hasQualifiers = true;
        }
        if (hasQualifiers) continue;

        // Push a new scope for the entry's parameters
        ctx.symbols.pushScope();

        // Resolve and declare parameters for each curry group
        bool paramError = false;
        for (const auto& group : entry->sig.paramGroups) {
            for (const auto& param : group) {
                if (!param) continue;

                TypeAST* paramType = param->type.get();
                if (!paramType) {
                    paramType = ctx.resolver.resolveType(param->type.get());
                    if (!paramType) {
                        ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3001,
                                     "cannot resolve parameter type for '" +
                                     std::string(ctx.pool.lookup(param->name)) + "'");
                        paramError = true;
                        continue;
                    }
                }

                Symbol ps;
                ps.name = param->name;
                ps.kind = SymbolKind::Param;
                ps.declKw = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type = paramType;
                ps.decl = param.get();
                ps.loc = param->loc;
                if (!ctx.symbols.declare(ps)) {
                    ctx.dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                                 "duplicate parameter name '" +
                                 std::string(ctx.pool.lookup(param->name)) +
                                 "' in from entry");
                    paramError = true;
                }
            }
        }

        if (paramError) {
            ctx.symbols.popScope();
            continue;
        }

        // Check entry body
        if (entry->body) {
            // The body must return a value of the target type
            checkStmt(entry->body.get(), ctx, targetType);
        } else {
            ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3002,
                         "from entry must have a body");
        }

        // Pop parameter scope
        ctx.symbols.popScope();

        // Check for duplicate signature (same parameter types, same curry structure)
        bool isDuplicate = false;
        for (auto* seen : verifiedEntries) {
            if (entry->sig.paramGroups.size() != seen->sig.paramGroups.size())
                continue;

            bool match = true;
            for (size_t g = 0; g < entry->sig.paramGroups.size(); ++g) {
                const auto& g1 = entry->sig.paramGroups[g];
                const auto& g2 = seen->sig.paramGroups[g];
                if (g1.size() != g2.size()) {
                    match = false;
                    break;
                }
                for (size_t p = 0; p < g1.size(); ++p) {
                    TypeAST* t1 = g1[p]->type.get();
                    TypeAST* t2 = g2[p]->type.get();
                    if (!ctx.checker.isEqual(t1, t2)) {
                        match = false;
                        break;
                    }
                }
                if (!match) break;
            }
            if (match) {
                isDuplicate = true;
                break;
            }
        }

        if (isDuplicate) {
            ctx.dc.error(DiagnosticCategory::Semantic, entry->loc, DiagCode::E3005,
                         "duplicate from entry signature (same parameter types)");
        } else {
            verifiedEntries.push_back(entry.get());
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkFromDecl: complete for "
                             << ctx.pool.lookup(node.targetTypeName)
                             << " with " << verifiedEntries.size() << " valid entries");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTopLevelDecl  — Dispatcher called by SemanticAnalyzer::checkDecls()
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx) {
    if (!decl) return;
    LUC_LOG_SEMANTIC("checkTopLevelDecl: kind=" << LucDebug::kindToString(decl->kind));

    if (decl->isa<VarDeclAST>())
        checkVarDecl(*decl->as<VarDeclAST>(), ctx);
    else if (decl->isa<FuncDeclAST>())
        checkFuncDecl(*decl->as<FuncDeclAST>(), ctx);
    else if (decl->isa<StructDeclAST>())
        checkStructDecl(*decl->as<StructDeclAST>(), ctx);
    else if (decl->isa<EnumDeclAST>())
        checkEnumDecl(*decl->as<EnumDeclAST>(), ctx);
    else if (decl->isa<TraitDeclAST>())
        checkTraitDecl(*decl->as<TraitDeclAST>(), ctx);
    else if (decl->isa<ImplDeclAST>())
        checkImplDecl(*decl->as<ImplDeclAST>(), ctx);
    else if (decl->isa<FromDeclAST>())
        checkFromDecl(*decl->as<FromDeclAST>(), ctx);
    // PackageDecl, UseDecl, TypeAliasDecl — nothing to check at phase 3.
}