/**
 * @file SemanticHelpers.hpp
 * @brief Shared helper functions for semantic checking (Phase 3).
 *
 * This header contains static helper functions used across multiple semantic
 * checking files, including attribute validation, constant expression detection,
 * type formatting, and impl block helpers.
 */

#pragma once

#include "ast/ExprAST.hpp"
#include "ast/support/ArenaSpan.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"

#include <string>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// isConstExpr  — Returns true when an expression is a compile-time constant
// ─────────────────────────────────────────────────────────────────────────────
static inline bool isConstExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return false;

    if (expr->isa<LiteralExprAST>()) {
        return expr->as<LiteralExprAST>()->kind != LiteralKind::Nil;
    }

    if (expr->isa<IdentifierExprAST>()) {
        Symbol* sym = ctx.symbols->lookup(expr->as<IdentifierExprAST>()->name);
        return sym && sym->declKw == DeclKeyword::Const;
    }

    if (expr->isa<FieldAccessExprAST>()) {
        auto* fa = expr->as<FieldAccessExprAST>();
        if (fa->object && fa->object->isa<IdentifierExprAST>()) {
            Symbol* sym = ctx.symbols->lookup(fa->object->as<IdentifierExprAST>()->name);
            return sym && sym->kind == SymbolKind::Enum;
        }
        return false;
    }

    if (expr->isa<BinaryExprAST>()) {
        auto* bin = expr->as<BinaryExprAST>();
        return isConstExpr(bin->left.get(), ctx) &&
               isConstExpr(bin->right.get(), ctx);
    }

    if (expr->isa<UnaryExprAST>()) {
        return isConstExpr(expr->as<UnaryExprAST>()->operand.get(), ctx);
    }

    if (expr->isa<TypeConvExprAST>()) {
        auto* tc = expr->as<TypeConvExprAST>();
        return isConstExpr(tc->expr.get(), ctx);
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAttributes  — Validates every '@' attribute on a declaration
// ─────────────────────────────────────────────────────────────────────────────
static inline void checkAttributes(const ArenaSpan<AttributePtr>& attributes,
                                   uint32_t attrCtx,
                                   const std::string& declName,
                                   DeclKeyword declKw,
                                   SemanticContext& ctx,
                                   bool& outIsExtern,
                                   std::string& outExternSym,
                                   std::string& outCallingConv) {
    LUC_LOG_SEMANTIC_VERBOSE("checkAttributes: count=" << attributes.size()
                             << ", ctx=0x" << std::hex << attrCtx
                             << ", declName='" << declName << "'");

    outIsExtern = false;
    outExternSym = "";
    outCallingConv = "C";

    std::vector<std::string> seen;

    for (const auto& attr : attributes) {
        std::string_view nameView = ctx.pool.lookup(attr->name);
        std::string name(nameView);
        LUC_LOG_SEMANTIC_EXTREME("checking attribute: @" << name);

        // Check for duplicate
        bool isDuplicate = false;
        for (const auto& seenName : seen) {
            if (seenName == name) {
                ctx.error(attr->loc, DiagCode::E3002, "duplicate attribute '@", name, "'");
                isDuplicate = true;
                break;
            }
        }
        if (isDuplicate) continue;

        // Check mutual exclusion with previously seen attributes
        bool mutuallyExclusive = false;
        for (const auto& seenName : seen) {
            if (!attribute::checkMutualExclusion(name, seenName, attr->loc)) {
                mutuallyExclusive = true;
                break;
            }
        }
        if (mutuallyExclusive) continue;

        // Look up the attribute entry from the registry
        const AttributeEntry* entry = attribute::lookup(name);
        if (!entry) {
            ctx.error(attr->loc, DiagCode::E3001, "unknown attribute '@", name, "'");
            continue;
        }

        // Add main context if this is the main function
        uint32_t registryCtx = attrCtx;
        if (declName == "main") {
            registryCtx |= static_cast<uint32_t>(AttributeContext::Main);
        }

        // Validate the attribute using registry
        if (!attribute::validateAttribute(*entry, attr->args,
                                          static_cast<AttributeContext>(registryCtx),
                                          declName, declKw,
                                          ctx.currentFile, attr->loc)) {
            continue;
        }

        seen.push_back(name);

        // Handle @extern specially
        if (name == "extern") {
            outIsExtern = true;
            if (!attr->args.empty() && attr->args[0] &&
                attr->args[0]->kind == AttributeArgKind::StringLit) {
                outExternSym = std::string(ctx.pool.lookup(attr->args[0]->value));
            }
            if (attr->args.size() >= 2 && attr->args[1] &&
                attr->args[1]->kind == AttributeArgKind::TypeIdent) {
                outCallingConv = std::string(ctx.pool.lookup(attr->args[1]->value));
            }
            LUC_LOG_SEMANTIC_EXTREME("\t@extern: sym='" << outExternSym
                                     << "', conv='" << outCallingConv << "'");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// formatType  — Helper to convert a TypeAST to a readable string for diagnostics
// ─────────────────────────────────────────────────────────────────────────────
static inline std::string formatType(TypeAST* type, const StringPool& pool) {
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
// Impl Block Helpers
// ─────────────────────────────────────────────────────────────────────────────

static inline void injectReceiverSymbol(const ImplDeclAST& node, SemanticContext& ctx) {
    InternedString recName = node.receiverAlias.isValid() ? node.receiverAlias
                           : ctx.pool.intern("self");
    Symbol rec;
    rec.name = recName;
    rec.kind = SymbolKind::Param;
    rec.declKw = DeclKeyword::Let;
    rec.visibility = Visibility::Private;
    rec.type = node.resolvedSelfType;
    rec.decl = nullptr;
    rec.loc = node.loc;
    if (!ctx.symbols->declare(rec)) {
        ctx.error(node.loc, DiagCode::E2005, "receiver name '", ctx.pool.lookup(recName),
                  "' conflicts with existing symbol");
    }
}

static inline void checkImplGenericParams(const ImplDeclAST& node,
                                          const ArenaSpan<GenericParamPtr>* targetParams,
                                          SemanticContext& ctx) {
    const auto& implParams = node.genericParams;

    if (targetParams == nullptr) {
        if (!implParams.empty()) {
            ctx.error(node.loc, DiagCode::E2015, "impl target is not generic, so impl cannot have generic parameters");
        }
        return;
    }

    if (implParams.size() != targetParams->size()) {
        ctx.error(node.loc, DiagCode::E2017,
                  "generic parameter count mismatch: impl has ", implParams.size(),
                  ", target has ", targetParams->size());
        return;
    }
    
    for (size_t i = 0; i < implParams.size(); ++i) {
        auto* ip = implParams[i].get();
        auto* tp = (*targetParams)[i].get();
        if (!ip || !tp) continue;
        if (ip->name != tp->name) {
            ctx.error(ip->loc, DiagCode::E2015,
                      "generic parameter name mismatch: expected '", ctx.pool.lookup(tp->name),
                      "', got '", ctx.pool.lookup(ip->name), "'");
        }
        if (ip->constraints.size() != tp->constraints.size()) {
            ctx.error(ip->loc, DiagCode::E2015,
                      "constraint count mismatch for parameter '", ctx.pool.lookup(ip->name), "'");
        } else {
            for (size_t j = 0; j < ip->constraints.size(); ++j) {
                if (ip->constraints[j] != tp->constraints[j]) {
                    ctx.error(ip->loc, DiagCode::E2015,
                              "constraint mismatch: expected '", ctx.pool.lookup(tp->constraints[j]),
                              "', got '", ctx.pool.lookup(ip->constraints[j]), "'");
                }
            }
        }
    }
}

static inline void checkImplMethod(const ImplDeclAST& node, MethodDeclAST& method,
                                   TypeAST* expectedReturn, SemanticContext& ctx) {
    ctx.symbols->pushScope();

    injectReceiverSymbol(node, ctx);

    if (method.funcType) {
        const FuncSignature& sig = method.funcType->sig;
        for (const auto& param : sig.allParams) {
            if (!param) continue;

            TypeAST* paramType = param->type.get();
            if (!paramType && ctx.resolver) {
                paramType = ctx.resolver->resolveType(param->type.get());
                if (!paramType) {
                    ctx.error(param->loc, DiagCode::E2001,
                              "cannot resolve parameter type for '", ctx.pool.lookup(param->name), "'");
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
            if (!ctx.symbols->declare(ps)) {
                ctx.error(param->loc, DiagCode::E2005,
                          "duplicate parameter name '", ctx.pool.lookup(param->name), "'");
            }
        }
    }

    if (method.body) {
        checkStmt(method.body.get(), ctx, expectedReturn);
    } else {
        ctx.error(method.loc, DiagCode::E2003,
                  "impl method '", ctx.pool.lookup(method.name), "' must have a body");
    }

    ctx.symbols->popScope();
}