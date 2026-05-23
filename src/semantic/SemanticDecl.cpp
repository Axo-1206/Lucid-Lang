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
#include "ast/support/ArenaSpan.hpp"
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
static void checkAttributes(const ArenaSpan<AttributePtr>& attributes,
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
                ctx.error(attr->loc, DiagCode::E3005,
                        {"duplicate attribute '@" + name + "'"});
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

// Helper to format a TypeAST as a string for diagnostics
static std::string formatType(TypeAST* type, StringPool& pool) {
    if (!type) return "<null>";
    
    if (type->isa<PrimitiveTypeAST>()) {
        auto* prim = type->as<PrimitiveTypeAST>();
        switch (prim->primitiveKind) {
            case PrimitiveKind::Bool:   return "bool";
            case PrimitiveKind::Byte:   return "byte";
            case PrimitiveKind::Short:  return "short";
            case PrimitiveKind::Int:    return "int";
            case PrimitiveKind::Long:   return "long";
            case PrimitiveKind::Ubyte:  return "ubyte";
            case PrimitiveKind::Ushort: return "ushort";
            case PrimitiveKind::Uint:   return "uint";
            case PrimitiveKind::Ulong:  return "ulong";
            case PrimitiveKind::Int8:   return "int8";
            case PrimitiveKind::Int16:  return "int16";
            case PrimitiveKind::Int32:  return "int32";
            case PrimitiveKind::Int64:  return "int64";
            case PrimitiveKind::Uint8:  return "uint8";
            case PrimitiveKind::Uint16: return "uint16";
            case PrimitiveKind::Uint32: return "uint32";
            case PrimitiveKind::Uint64: return "uint64";
            case PrimitiveKind::Float:  return "float";
            case PrimitiveKind::Double: return "double";
            case PrimitiveKind::Decimal:return "decimal";
            case PrimitiveKind::String: return "string";
            case PrimitiveKind::Char:   return "char";
            case PrimitiveKind::Any:    return "any";
            
            default: return "primitive";
        }
    }
    
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        std::string result = std::string(pool.lookup(named->name));
        if (!named->genericArgs.empty()) {
            result += "<";
            for (size_t i = 0; i < named->genericArgs.size(); ++i) {
                if (i > 0) result += ", ";
                result += formatType(named->genericArgs[i].get(), pool);
            }
            result += ">";
        }
        return result;
    }
    
    return LucDebug::kindToString(type->kind);
}

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
        ctx.error(node.loc, DiagCode::E3005, {"local variable cannot have visibility modifier (pub/export)"});
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
            ctx.error(node.loc, DiagCode::E3002,
                {"'@extern' variable '" + std::string(ctx.pool.lookup(node.name)) +
                "' must not have an initialiser — the symbol is resolved by the linker"});
            return;
        }
        if (node.keyword != DeclKeyword::Const) {
            ctx.error(node.loc, DiagCode::E3002, { "'@extern' variable must be declared with 'const'"});
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
        // VarDeclAST has no resolvedType field; the symbol holds the type.
        return;
    }

    // ── Resolve declared type ─────────────────────────────────────────────────
    TypeAST* declaredType = ctx.resolver.resolveType(node.type.get());
    if (!declaredType) return;

    // ── No initialiser ────────────────────────────────────────────────────────
    if (!node.init) {
        if (node.keyword == DeclKeyword::Const) {
            ctx.error(node.loc, DiagCode::E3002,
                {"const '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must have an initialiser"});
        } else if (!ctx.checker.isNullable(declaredType)) {
            ctx.error(node.loc, DiagCode::E3002,
                { "variable '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must have an initial value because it is not nullable"});
        }
        // Update symbol type if not already set
        Symbol* sym = ctx.symbols.lookup(node.name);
        if (sym && !sym->type) sym->type = declaredType;
        return;
    }

    // ── Check initialiser expression ─────────────────────────────────────────
    TypeAST* initType = checkExpr(node.init.get(), ctx);
    if (!initType) return;

    // ── nil assignment to non‑nullable type ───────────────────────────────────
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !ctx.checker.isNullable(declaredType)) {
            ctx.error(node.loc, DiagCode::E3002,
                {"nil cannot be assigned to non-nullable type '" +
                         formatType(declaredType, ctx.pool) + "'"});
            return;
        }
    }

    // ── const requires compile‑time constant ──────────────────────────────────
    if (node.keyword == DeclKeyword::Const && !isConstExpr(node.init.get(), ctx)) {
        ctx.error(node.loc, DiagCode::E3002,
                {"const '" + std::string(ctx.pool.lookup(node.name)) +
                     "' initialiser must be a compile‑time constant expression"});
    }

    // ── Type assignability check, with optional from‑casting ──────────────────
    if (!ctx.checker.isAssignable(initType, declaredType)) {
        // Try to find a `from` conversion from initType to declaredType
        Symbol* fromCast = ctx.checker.isFromCastable(initType, declaredType, &ctx.symbols);
        if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
            // Rewrite the initialiser as an explicit cast
            // Create a NamedTypeAST for the target type (simplified; for better fidelity, use clone)
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
            ctx.error(node.loc, DiagCode::E3008,
                    {"cannot implicitly convert initializer to type '" +
                         formatType(declaredType, ctx.pool) +
                         "' for variable '" + std::string(ctx.pool.lookup(node.name)) +
                         "'; use an explicit type cast like '[target_type](value)' "
                         "or define a 'from' casting block"});
        }
    }

    // ── Update symbol type (if not already set) ───────────────────────────────
    Symbol* sym = ctx.symbols.lookup(node.name);
    if (sym && !sym->type) {
        sym->type = declaredType;
    }
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
        ctx.error( node.loc, DiagCode::E3005,
                     {"local function cannot have visibility modifier (pub/export)"});
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
            ctx.error(node.loc, DiagCode::E3002,
                    {"@extern function '" + std::string(ctx.pool.lookup(node.name)) +
                         "' must not have a body"});
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

    // ── Declare parameters using flattened allParams ─────────────────────────
    // Parameters are already flattened; we still need to respect curry groups
    // for scoping? In Luc, all parameters are in the same scope regardless of curry.
    // So we just iterate over allParams.
    for (const auto& param : node.sig.allParams) {
        if (!param) continue;

        // Parameter type may already be resolved; if not, resolve now.
        TypeAST* paramType = param->type.get();
        if (!paramType) {
            paramType = ctx.resolver.resolveType(param->type.get());
            if (!paramType) {
                ctx.error(param->loc, DiagCode::E3001,
                        {"cannot resolve type for parameter '" +
                             std::string(ctx.pool.lookup(param->name)) + "'"});
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
            ctx.error(param->loc, DiagCode::E3005,
                    {"duplicate parameter name '" +
                         std::string(ctx.pool.lookup(param->name)) + "'"});
        }
    }

    // ── Check function body (if present) ─────────────────────────────────────
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    } else {
        ctx.error(node.loc, DiagCode::E3002,
                {"function '" + std::string(ctx.pool.lookup(node.name)) +
                     "' must have a body"});
    }

    // ── Pop scopes and stack ─────────────────────────────────────────────────
    ctx.symbols.popScope();

    if (isParallel) {
        ctx.exitParallel();
    }

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
        ctx.error(node.loc, DiagCode::E3005, {"local struct cannot have visibility modifier (pub/export)"});
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
            ctx.error(field->loc, DiagCode::E3005,
                {"duplicate field '" + fieldName + "' in struct '" +
                         std::string(ctx.pool.lookup(node.name)) + "'"});
            continue;
        }

        // Resolve field type (should already be resolved by Phase 2, but be defensive)
        TypeAST* fieldType = field->type.get();
        if (!fieldType) {
            // Try to resolve now
            fieldType = ctx.resolver.resolveType(field->type.get());
            if (!fieldType) {
                ctx.error(field->loc, DiagCode::E3001,
                    {"cannot resolve type for field '" + fieldName + "' in struct '" +
                             std::string(ctx.pool.lookup(node.name)) + "'"});
                continue;
            }
        }

        // Check default value, if present
        if (field->defaultVal) {
            TypeAST* defaultType = checkExpr(field->defaultVal.get(), ctx);
            if (defaultType && !ctx.checker.isAssignable(defaultType, fieldType)) {
                ctx.error(field->loc, DiagCode::E3002,
                    {"default value type mismatch for field '" + fieldName +
                             "' in struct '" + std::string(ctx.pool.lookup(node.name)) + "'"});
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
        ctx.error(node.loc, DiagCode::E3005, {"local enum cannot have visibility modifier (pub/export)"});
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
            ctx.error(variant->loc, DiagCode::E3005,
                {"duplicate enum value " + std::to_string(value) +
                         " for variant '" + variantName +
                         "' in enum '" + std::string(ctx.pool.lookup(node.name)) + "'"});
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
        ctx.error(node.loc, DiagCode::E3005,
                  {"local trait cannot have visibility modifier (pub/export)"});
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
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
            ctx.error(method->loc, DiagCode::E3005,
                      {"duplicate method '" + methodName + "' in trait '" +
                       std::string(ctx.pool.lookup(node.name)) + "'"});
            continue;
        }

        LUC_LOG_SEMANTIC_EXTREME("\tchecking trait method: " << methodName);

        // Resolve parameter types (flattened)
        for (const auto& param : method->sig.allParams) {
            if (!param) continue;
            if (param->type) {
                // Parameter type should already be resolved by Phase 2, but be defensive
                TypeAST* paramType = param->type.get();
                if (!paramType) {
                    paramType = ctx.resolver.resolveType(param->type.get());
                    if (!paramType) {
                        ctx.error(param->loc, DiagCode::E3001,
                                  {"cannot resolve type for parameter '" +
                                   std::string(ctx.pool.lookup(param->name)) +
                                   "' in trait method '" + methodName + "'"});
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
                        ctx.error(method->loc, DiagCode::E3001,
                                  {"cannot resolve return type for trait method '" +
                                   methodName + "'"});
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

// ============================================================================
// Helpers for impl block checking
// ============================================================================

// Helper: inject receiver symbol into the current scope
static void injectReceiverSymbol(const ImplDeclAST& node, SemanticContext& ctx) {
    InternedString recName = node.receiverAlias.isValid() ? node.receiverAlias
                           : ctx.pool.intern("self");
    Symbol rec;
    rec.name = recName;
    rec.kind = SymbolKind::Param;   // read‑only reference to the instance
    rec.declKw = DeclKeyword::Let;
    rec.visibility = Visibility::Private;
    rec.type = node.resolvedSelfType;
    rec.decl = nullptr;
    rec.loc = node.loc;
    if (!ctx.symbols.declare(rec)) {
        ctx.error(node.loc, DiagCode::E3005,
            {"receiver name '" + std::string(ctx.pool.lookup(recName)) +
                    "' conflicts with existing symbol"});
    }
}

// Helper: check that impl generic parameters match target's generic parameters
static void checkImplGenericParams(const ImplDeclAST& node,
                                   const ArenaSpan<GenericParamPtr>* targetParams,
                                   SemanticContext& ctx) {
    const auto& implParams = node.genericParams;

    if (targetParams == nullptr) {
        // Target is non‑generic (e.g., enum, non‑generic struct, or alias to such)
        if (!implParams.empty()) {
            ctx.error(node.loc, DiagCode::E3017, {"impl target is not generic, so impl cannot have generic parameters"});
        }
        return;
    }

    // Target is generic – compare counts and names
    if (implParams.size() != targetParams->size()) {
        ctx.error(node.loc, DiagCode::E3017,
                {"generic parameter count mismatch: impl has " +
                     std::to_string(implParams.size()) +
                     ", target has " + std::to_string(targetParams->size())});
        return;
    }
    for (size_t i = 0; i < implParams.size(); ++i) {
        auto* ip = implParams[i].get();
        auto* tp = (*targetParams)[i].get();
        if (!ip || !tp) continue;
        if (ip->name != tp->name) {
            ctx.error(ip->loc, DiagCode::E3017,
                {"generic parameter name mismatch: expected '" +
                         std::string(ctx.pool.lookup(tp->name)) +
                         "', got '" + std::string(ctx.pool.lookup(ip->name)) + "'"});
        }
        // Optionally compare constraints (same order, same names)
        if (ip->constraints.size() != tp->constraints.size()) {
            ctx.error(ip->loc, DiagCode::E3017,
                {"constraint count mismatch for parameter '" +
                         std::string(ctx.pool.lookup(ip->name)) + "'"});
        } else {
            for (size_t j = 0; j < ip->constraints.size(); ++j) {
                if (ip->constraints[j] != tp->constraints[j]) {
                    ctx.error(ip->loc, DiagCode::E3017,
                    {"constraint mismatch: expected '" +
                                 std::string(ctx.pool.lookup(tp->constraints[j])) +
                                 "', got '" +
                                 std::string(ctx.pool.lookup(ip->constraints[j])) + "'"});
                }
            }
        }
    }
}

// Helper: check a single method inside the impl block
static void checkImplMethod(const ImplDeclAST& node, MethodDeclAST& method,
                            TypeAST* expectedReturn, SemanticContext& ctx) {
    ctx.symbols.pushScope();

    // 1. Inject receiver
    injectReceiverSymbol(node, ctx);

    // 2. Declare parameters using flattened allParams
    for (const auto& param : method.sig.allParams) {
        if (!param) continue;

        TypeAST* paramType = param->type.get();
        if (!paramType) {
            paramType = ctx.resolver.resolveType(param->type.get());
            if (!paramType) {
                ctx.error(param->loc, DiagCode::E3001,
                          {"cannot resolve parameter type for '" +
                           std::string(ctx.pool.lookup(param->name)) + "'"});
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
            ctx.error(param->loc, DiagCode::E3005,
                      {"duplicate parameter name '" +
                       std::string(ctx.pool.lookup(param->name)) + "'"});
        }
    }

    // 3. Check body
    if (method.body) {
        checkStmt(method.body.get(), ctx, expectedReturn);
    } else {
        ctx.error(method.loc, DiagCode::E3002,
                  {"impl method '" + std::string(ctx.pool.lookup(method.name)) +
                   "' must have a body"});
    }

    ctx.symbols.popScope();
}

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
        ctx.error(node.loc, DiagCode::E3005, {"local impl cannot have visibility modifier (pub/export)"});
    }

    // Ensure the target was resolved
    if (!node.resolvedSelfType) {
        ctx.error(node.loc, DiagCode::E3001, {"impl target could not be resolved"});
        return;
    }

    // Validate generic parameters against the target
    if (node.resolvedTargetGenericParams) {
        checkImplGenericParams(node, node.resolvedTargetGenericParams, ctx);
    } else if (!node.genericParams.empty()) {
        ctx.error(node.loc, DiagCode::E3017, {"non‑generic target cannot have generic parameters"});
    }

    // Push substitution map (if any) so that generic parameters inside method bodies are replaced
    if (!node.resolvedSubstitutionMap.empty()) {
        ctx.resolver.pushSubstitutionMap(&node.resolvedSubstitutionMap);
    }

    // Check each method
    std::unordered_set<std::string> seenMethods;
    for (auto& method : node.methods) {
        if (!method) continue;
        std::string mname = std::string(ctx.pool.lookup(method->name));
        if (!seenMethods.insert(mname).second) {
            ctx.error(method->loc, DiagCode::E3005, {"duplicate method '" + mname + "' in impl"});
            continue;
        }
        TypeAST* expectedReturn = method->sig.returnTypes.empty()
                                  ? nullptr
                                  : method->sig.returnTypes[0].get();
        bool wasParallel = method->sig.isParallel();
        if (wasParallel) ctx.enterParallel();
        checkImplMethod(node, *method, expectedReturn, ctx);
        if (wasParallel) ctx.exitParallel();
    }

    // Pop substitution map
    if (!node.resolvedSubstitutionMap.empty()) {
        ctx.resolver.popSubstitutionMap();
    }

    // Trait conformance (optional, can be added later)
    // if (node.traitRef) { ... }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFromDecl
//
// Rules enforced:
//   - If isLocal, reject visibility modifiers – local from blocks are private.
//   - Target struct must exist in the symbol table (can be from any visible scope).
//   - Each entry's return type must match the target struct type.
//   - Parameter types in each entry must resolve.
//   - No duplicate entry signatures within the same from block (multiple blocks
//     for the same target in different scopes are allowed).
//   - Each entry body is checked to return the target type.
//   - From entries cannot have qualifiers (~async, ~nullable, ~parallel).
//   - Generic parameters are pushed so entries can refer to T.
//   - From blocks can appear in any scope and are visible in that scope and
//     any nested scope (standard lexical scoping).
// ─────────────────────────────────────────────────────────────────────────────
void checkFromDecl(FromDeclAST& node, SemanticContext& ctx, bool isLocal) {
    // Extract target type name from targetType (must be NamedTypeAST)
    InternedString targetTypeName;
    if (node.targetType && node.targetType->isa<NamedTypeAST>()) {
        targetTypeName = node.targetType->as<NamedTypeAST>()->name;
    } else {
        ctx.error(node.loc, DiagCode::E3001,
                  {"from block target must be a named type (struct)"});
        return;
    }

    LUC_LOG_SEMANTIC("checkFromDecl: target=" << ctx.pool.lookup(targetTypeName)
                     << ", entries=" << node.entries.size()
                     << ", isLocal=" << isLocal);

    // ── Local from blocks cannot have visibility modifiers ───────────────────
    if (isLocal && node.visibility != Visibility::Private) {
        ctx.error(node.loc, DiagCode::E3005,
                  {"local from block cannot have visibility modifier (pub/export)"});
    }

    // ── Validate @attributes (deprecated, etc.) ──────────────────────────────
    bool attrIsExtern = false;
    std::string attrExternSym, attrCallingConv;
    checkAttributes(node.attributes, AttributeContext::None,
                    std::string(ctx.pool.lookup(targetTypeName)), DeclKeyword::Let,
                    ctx, attrIsExtern, attrExternSym, attrCallingConv);

    // ── Verify target struct exists (lookup in symbol table) ─────────────────
    Symbol* targetSym = ctx.symbols.lookup(targetTypeName);
    if (!targetSym || targetSym->kind != SymbolKind::Struct) {
        ctx.error(node.loc, DiagCode::E3001,
                  {"from block target '" + std::string(ctx.pool.lookup(targetTypeName)) +
                   "' is not a declared struct in the current scope"});
        return;
    }
    auto* structDecl = targetSym->decl->as<StructDeclAST>();
    TypeAST* targetType = structDecl->selfType.get();
    if (!targetType) {
        ctx.error(node.loc, DiagCode::E3001,
                  {"from block target type not resolved"});
        return;
    }

    // ── Push generic parameters (from block can be generic: from Wrapper<T>) ──
    if (!node.genericParams.empty()) {
        ctx.resolver.pushGenericParams(&node.genericParams);
    }

    // ── Check each from entry ────────────────────────────────────────────────
    std::vector<FromEntryAST*> verifiedEntries;

    for (auto& entry : node.entries) {
        if (!entry) continue;

        LUC_LOG_SEMANTIC_EXTREME("\tchecking from entry");

        // Resolve return type (should be target struct type)
        TypeAST* entryReturnType = entry->returnType.get();
        if (!entryReturnType) {
            entryReturnType = ctx.resolver.resolveType(entry->returnType.get());
            if (!entryReturnType) {
                ctx.error(entry->loc, DiagCode::E3001,
                          {"from entry: cannot resolve return type"});
                continue;
            }
        }

        // Verify return type matches target struct
        if (!ctx.checker.isEqual(entryReturnType, targetType)) {
            ctx.error(entry->loc, DiagCode::E3002,
                      {"from entry return type must be '" +
                       std::string(ctx.pool.lookup(targetTypeName)) +
                       "', got '" + LucDebug::kindToString(entryReturnType->kind) + "'"});
            continue;
        }

        // Check that the entry has no qualifiers
        bool hasQualifiers = false;
        for (const auto& qualName : entry->sig.rawQualifiers) {
            ctx.error(entry->loc, DiagCode::E2010,
                      {"from entries cannot have qualifiers (~" +
                       std::string(ctx.pool.lookup(qualName)) +
                       "); qualifiers are not allowed on conversions"});
            hasQualifiers = true;
        }
        if (entry->sig.qualifiers != 0) {
            ctx.error(entry->loc, DiagCode::E2010,
                      {"from entries cannot have qualifiers (~async, ~nullable, ~parallel)"});
            hasQualifiers = true;
        }
        if (hasQualifiers) continue;

        // Push a new scope for the entry's parameters
        ctx.symbols.pushScope();

        // Resolve and declare parameters (flattened allParams)
        bool paramError = false;
        for (const auto& param : entry->sig.allParams) {
            if (!param) continue;

            TypeAST* paramType = param->type.get();
            if (!paramType) {
                paramType = ctx.resolver.resolveType(param->type.get());
                if (!paramType) {
                    ctx.error(param->loc, DiagCode::E3001,
                              {"cannot resolve parameter type for '" +
                               std::string(ctx.pool.lookup(param->name)) + "'"});
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
                ctx.error(param->loc, DiagCode::E3005,
                          {"duplicate parameter name '" +
                           std::string(ctx.pool.lookup(param->name)) +
                           "' in from entry"});
                paramError = true;
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
            ctx.error(entry->loc, DiagCode::E3002,
                      {"from entry must have a body"});
        }

        // Pop parameter scope
        ctx.symbols.popScope();

        // Check for duplicate signature within this from block only
        // Compare parameter groups (preserving curry structure)
        bool isDuplicate = false;
        for (auto* seen : verifiedEntries) {
            if (entry->sig.groupCount() != seen->sig.groupCount())
                continue;

            bool match = true;
            for (size_t g = 0; g < entry->sig.groupCount(); ++g) {
                auto group1 = entry->sig.getGroup(g);
                auto group2 = seen->sig.getGroup(g);
                if (group1.size() != group2.size()) {
                    match = false;
                    break;
                }
                for (size_t p = 0; p < group1.size(); ++p) {
                    TypeAST* t1 = group1[p]->type.get();
                    TypeAST* t2 = group2[p]->type.get();
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
            ctx.error(entry->loc, DiagCode::E3005,
                      {"duplicate from entry signature (same parameter types) within the same from block"});
        } else {
            verifiedEntries.push_back(entry.get());
        }
    }

    // ── Pop generic parameters ───────────────────────────────────────────────
    if (!node.genericParams.empty()) {
        ctx.resolver.popGenericParams();
    }

    LUC_LOG_SEMANTIC_VERBOSE("checkFromDecl: complete for "
                             << ctx.pool.lookup(targetTypeName)
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