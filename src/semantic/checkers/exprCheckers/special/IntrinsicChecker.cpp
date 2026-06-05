/**
 * @file IntrinsicChecker.cpp
 * @brief Semantic checking for intrinsic calls (#intrinsic).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "registry/IntrinsicRegistry.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIntrinsicCallExpr: name=" << ctx.pool.lookup(node.intrinsicName));
    
    // Look up intrinsic in registry
    const IntrinsicEntry* entry = intrinsic::lookup(node.intrinsicName);
    if (!entry) {
        ctx.error(node.loc, DiagCode::E4001,
                  "unknown intrinsic '#", ctx.pool.lookup(node.intrinsicName), "'");
        return nullptr;
    }
    
    // Check type argument if required
    if (!entry->argKinds.empty() && entry->argKinds[0] == IntrinsicArgKind::TypeArg) {
        if (!node.typeArg) {
            ctx.error(node.loc, DiagCode::E4002,
                      "intrinsic '#", ctx.pool.lookup(node.intrinsicName),
                      "' requires a type argument");
            return nullptr;
        }
        if (ctx.dispatcher) {
            TypeAST* resolvedType = ctx.dispatcher->resolveType(node.typeArg.get());
            if (!resolvedType) {
                ctx.error(node.loc, DiagCode::E4003,
                          "invalid type argument for intrinsic '#",
                          ctx.pool.lookup(node.intrinsicName), "'");
                return nullptr;
            }
        }
    }
    
    // Check argument count
    size_t expectedMin = static_cast<size_t>(entry->minArgs);
    size_t expectedMax = (entry->maxArgs == -1) ? SIZE_MAX : static_cast<size_t>(entry->maxArgs);
    
    if (node.args.size() < expectedMin || node.args.size() > expectedMax) {
        ctx.error(node.loc, DiagCode::E4002,
                  "intrinsic '#", ctx.pool.lookup(node.intrinsicName),
                  "' expects ", expectedMin, " to ",
                  (entry->maxArgs == -1 ? "unlimited" : std::to_string(entry->maxArgs)),
                  " arguments, got ", node.args.size());
        return nullptr;
    }
    
    // Check argument types
    for (size_t i = 0; i < node.args.size(); ++i) {
        TypeAST* argType = checkExpr(node.args[i].get(), ctx);
        if (!argType) return nullptr;
        
        // Type-specific validation
        size_t argIdx = (entry->argKinds[0] == IntrinsicArgKind::TypeArg) ? i + 1 : i;
        if (argIdx < entry->argKinds.size()) {
            IntrinsicArgKind expectedKind = entry->argKinds[argIdx];
            switch (expectedKind) {
                case IntrinsicArgKind::IntValue:
                    if (!TypeChecker::isIntegerType(argType, ctx)) {
                        ctx.error(node.args[i]->loc, DiagCode::E4003,
                                  "argument ", i + 1, " of '#", ctx.pool.lookup(node.intrinsicName),
                                  "' must be an integer");
                        return nullptr;
                    }
                    break;
                case IntrinsicArgKind::FloatValue:
                    if (!argType->isa<PrimitiveTypeAST>() ||
                        (argType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Float &&
                         argType->as<PrimitiveTypeAST>()->primitiveKind != PrimitiveKind::Double)) {
                        ctx.error(node.args[i]->loc, DiagCode::E4003,
                                  "argument ", i + 1, " of '#", ctx.pool.lookup(node.intrinsicName),
                                  "' must be a float or double");
                        return nullptr;
                    }
                    break;
                case IntrinsicArgKind::PtrValue:
                    if (!argType->isa<PtrTypeAST>()) {
                        ctx.error(node.args[i]->loc, DiagCode::E4003,
                                  "argument ", i + 1, " of '#", ctx.pool.lookup(node.intrinsicName),
                                  "' must be a raw pointer (*T)");
                        return nullptr;
                    }
                    break;
                case IntrinsicArgKind::SizeValue:
                    if (!TypeChecker::isIntegerType(argType, ctx)) {
                        ctx.error(node.args[i]->loc, DiagCode::E4003,
                                  "argument ", i + 1, " of '#", ctx.pool.lookup(node.intrinsicName),
                                  "' must be a size (integer)");
                        return nullptr;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    
    // Determine return type
    TypeAST* returnType = nullptr;
    switch (entry->returnKind) {
        case IntrinsicReturnKind::Void:
            returnType = nullptr;
            break;
        case IntrinsicReturnKind::Uint64:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint64).release();
            break;
        case IntrinsicReturnKind::Int64:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int64).release();
            break;
        case IntrinsicReturnKind::Float32:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Float).release();
            break;
        case IntrinsicReturnKind::Float64:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Double).release();
            break;
        case IntrinsicReturnKind::SameAsArg0:
            if (!node.args.empty()) {
                returnType = node.args[0]->resolvedType;
            }
            break;
        case IntrinsicReturnKind::SameAsArg1:
            if (node.args.size() >= 2) {
                returnType = node.args[1]->resolvedType;
            }
            break;
        case IntrinsicReturnKind::RefOfTypeArg0:
            if (node.typeArg) {
                returnType = ctx.arena.make<RefTypeAST>(TypePtr(node.typeArg.get())).release();
            }
            break;
        default:
            returnType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Any).release();
            break;
    }
    
    node.resolvedType = returnType;
    node.isConst = true; // Intrinsics are compile-time evaluable where applicable
    return returnType;
}