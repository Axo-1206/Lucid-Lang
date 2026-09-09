/// @file registry/IntrinsicValidator.cpp
/// @brief Implementation of semantic validation for intrinsics.

#include "IntrinsicValidator.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include "core/ASTStrings.hpp"
#include "ArgTypeValidators.hpp"
#include "sema/Sema.hpp"
#include "sema/types/GenericHelpers.hpp"
#include "core/trace/Trace.hpp"

namespace sema {

// ─── Internal Helpers ──────────────────────────────────────────────────────

static bool isArgumentCountValid(size_t count, const IntrinsicInfo* info) {
    if (info->isVarArg) {
        return count >= info->minArgs;
    }
    return count >= info->minArgs && count <= info->maxArgs;
}

static bool isIntrinsicVoidInternal(InternedString name, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    return registry.isVoid(name);
}

/// @brief Resolve a type argument that may be a generic parameter.
/// 
/// This handles the common pattern for #sizeof(T), #alignof(T), #bitcast(T, x),
/// and #alloc(T, count) where the first argument is a type.
/// 
/// @param arg The expression to resolve as a type.
/// @param ctx The semantic context.
/// @return The resolved TypeAST, or nullptr on error.
static TypeAST* resolveTypeArgument(ExprAST* arg, SemaContext& ctx) {
    if (!arg) return nullptr;
    
    TypeAST* type = nullptr;
    
    if (arg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = arg->as<IdentifierExprAST>();
        if (id->isType) {
            type = id->resolvedTypeNode;
        } else {
            // ─── Check: Is this a generic parameter? ─────────────────────────
            // This must be checked BEFORE lookupType or isPrimitiveTypeName!
            if (ctx.isGenericParam(id->name)) {
                GenericParamDeclAST* param = ctx.lookupGenericParam(id->name);
                NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                namedType->resolvedDecl = param;
                type = namedType;
                id->isType = true;
                id->resolvedTypeNode = type;
                id->resolvedType = type;
            }
            // ─── Check: Is it a user-defined type? ──────────────────────────
            else {
                TypeDeclAST* typeDecl = ctx.lookupType(id->name);
                if (typeDecl) {
                    NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                    namedType->resolvedDecl = typeDecl;
                    namedType->genericArgs = id->genericArgs;
                    type = namedType;
                    id->isType = true;
                    id->resolvedTypeNode = type;
                    id->resolvedType = type;
                }
                // ─── Check: Is it a primitive type? ──────────────────────────
                else if (isPrimitiveTypeName(id->name, ctx.pool)) {
                    PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                    type = ctx.arena.make<PrimitiveTypeAST>(kind);
                    id->isType = true;
                    id->resolvedTypeNode = type;
                    id->resolvedType = type;
                }
            }
        }
    }
    
    return type;
}

// ─── Public API ────────────────────────────────────────────────────────────

bool validateIntrinsicCall(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);

    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) {
        ctx.diagnostics.error(DiagCode::Sem_UnknownIntrinsic, expr,
                              "unknown intrinsic '#", ctx.pool.lookup(expr->intrinsicName), "'");
        return false;
    }

    if (!validateIntrinsicArgCount(expr->intrinsicName, expr->args.size(), ctx)) {
        const std::string intrinsicName = ctx.pool.lookup(expr->intrinsicName);
        if (info->isVarArg || info->minArgs != info->maxArgs) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "intrinsic '#", intrinsicName, "' expects at least ",
                                  std::to_string(info->minArgs), " argument(s), got ",
                                  std::to_string(expr->args.size()));
        } else {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "intrinsic '#'", intrinsicName, "' expects ",
                                  std::to_string(info->minArgs), " argument(s), got ",
                                  std::to_string(expr->args.size()));
        }
        return false;
    }

    // ─── Dispatch by IntrinsicKind ─────────────────────────────────────────
    switch (info->kind) {
        // ─── Floating-Point Math ──────────────────────────────────────────
        case IntrinsicKind::Sqrt:
        case IntrinsicKind::Abs:
        case IntrinsicKind::Fma:
        case IntrinsicKind::Ceil:
        case IntrinsicKind::Floor:
        case IntrinsicKind::Round:
        case IntrinsicKind::Pow:
        case IntrinsicKind::Min:
        case IntrinsicKind::Max:
            return validateFloatingPoint(expr, ctx);

        // ─── Memory Operations ────────────────────────────────────────────
        case IntrinsicKind::Memcpy:
        case IntrinsicKind::Memmove:
        case IntrinsicKind::Memset:
            return validateMemoryOp(expr, ctx);

        // ─── CPU Hints ────────────────────────────────────────────────────
        case IntrinsicKind::Prefetch:
        case IntrinsicKind::PrefetchR:
        case IntrinsicKind::PrefetchW:
            return true;

        case IntrinsicKind::Fence:
            return validateFence(expr, ctx);

        case IntrinsicKind::Pause:
            return true;

        // ─── Atomics ──────────────────────────────────────────────────────
        case IntrinsicKind::AtomicLoad:
        case IntrinsicKind::AtomicStore:
        case IntrinsicKind::AtomicAdd:
        case IntrinsicKind::AtomicSub:
        case IntrinsicKind::AtomicAnd:
        case IntrinsicKind::AtomicOr:
        case IntrinsicKind::AtomicXor:
        case IntrinsicKind::AtomicCas:
            return validateAtomicOp(expr, ctx);

        // ─── Type & Value Inspection ──────────────────────────────────────
        case IntrinsicKind::Sizeof:
            return validateSizeof(expr, ctx);
        case IntrinsicKind::Alignof:
            return validateAlignof(expr, ctx);
        case IntrinsicKind::Typeof:
        case IntrinsicKind::Nameof:
        case IntrinsicKind::Ptrstr:
        case IntrinsicKind::Addrof:
            return true;

        case IntrinsicKind::Tostr:
            return validateTostr(expr, ctx);

        // ─── Pointer Operations ────────────────────────────────────────────
        case IntrinsicKind::PtrOffset:
        case IntrinsicKind::PtrDiff:
        case IntrinsicKind::ToRef:
        case IntrinsicKind::ToPtr:
            return validatePointerOp(expr, ctx);

        // ─── Bit Manipulation ─────────────────────────────────────────────
        case IntrinsicKind::Bitcast:
            return validateBitcast(expr, ctx);

        case IntrinsicKind::Clz:
        case IntrinsicKind::Ctz:
        case IntrinsicKind::Popcount:
        case IntrinsicKind::Bswap:
            if (!expr->args.empty() && !validateIntArg(expr->args[0], "value", ctx)) {
                return false;
            }
            return true;

        // ─── Branch Prediction ────────────────────────────────────────────
        case IntrinsicKind::Likely:
        case IntrinsicKind::Unlikely:
            if (!expr->args.empty() && !validateBoolArg(expr->args[0], "condition", ctx)) {
                return false;
            }
            return true;

        // ─── String Operations ─────────────────────────────────────────────
        case IntrinsicKind::StrLen:
        case IntrinsicKind::StrPtr:
        case IntrinsicKind::StrFromPtr:
        case IntrinsicKind::StrConcat:
        case IntrinsicKind::StrSlice:
        case IntrinsicKind::StrEq:
        case IntrinsicKind::StrByteAt:
            return validateStringOp(expr, ctx);

        // ─── Memory Management ─────────────────────────────────────────────
        case IntrinsicKind::Alloc:
        case IntrinsicKind::Free:
            return validateMemoryManagement(expr, ctx);

        // ─── Scope Exit ────────────────────────────────────────────────────
        case IntrinsicKind::ScopeExit:
            return validateScopeExit(expr, ctx);

        // ─── SIMD ──────────────────────────────────────────────────────────
        case IntrinsicKind::SimdAdd:
        case IntrinsicKind::SimdSub:
        case IntrinsicKind::SimdMul:
        case IntrinsicKind::SimdDiv:
        case IntrinsicKind::SimdFma:
        case IntrinsicKind::SimdMin:
        case IntrinsicKind::SimdMax:
        case IntrinsicKind::SimdLoad:
        case IntrinsicKind::SimdStore:
        case IntrinsicKind::SimdSplat:
        case IntrinsicKind::SimdExtract:
        case IntrinsicKind::SimdInsert:
            return validateSIMD(expr, ctx);

        // ─── Unknown kind ──────────────────────────────────────────────────
        default:
            ctx.diagnostics.error(DiagCode::Sem_UnknownIntrinsic, expr,
                                  "intrinsic '#'", ctx.pool.lookup(expr->intrinsicName),
                                  "' has unknown kind in validator");
            return false;
    }
}

bool validateIntrinsicArgCount(InternedString name, size_t count, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(name);
    if (!info) return false;
    return isArgumentCountValid(count, info);
}

bool isIntrinsicVoid(InternedString name, SemaContext& ctx) {
    return isIntrinsicVoidInternal(name, ctx);
}

// ─── getIntrinsicReturnType - FULL Implementation ─────────────────────────

TypeAST* getIntrinsicReturnType(IntrinsicCallExprAST* expr,
                                TypeAST* targetType,
                                SemaContext& ctx) {
    if (!expr) return targetType;

    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return targetType;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Void intrinsics return nothing ────────────────────────────────────
    if (isIntrinsicVoidInternal(expr->intrinsicName, ctx)) {
        return nullptr;
    }

    // ─── Dispatch by IntrinsicKind ─────────────────────────────────────────
    switch (info->kind) {
        // ─── Type/Value Inspection ────────────────────────────────────────
        case IntrinsicKind::Sizeof:
        case IntrinsicKind::Alignof:
            return ctx.getIntType();

        case IntrinsicKind::Typeof:
        case IntrinsicKind::Nameof:
        case IntrinsicKind::Tostr:
        case IntrinsicKind::Ptrstr:
            return ctx.getStringType();

        // ─── Pointer Operations ────────────────────────────────────────────
        case IntrinsicKind::Addrof: {
            if (!expr->args.empty() && expr->args[0]->resolvedType) {
                TypeAST* innerType = expr->args[0]->resolvedType;
                return ctx.getPtrType(innerType);
            }
            return targetType;
        }

        case IntrinsicKind::ToRef: {
            if (!expr->args.empty() && expr->args[0]->resolvedType) {
                TypeAST* argType = expr->args[0]->resolvedType;
                if (argType->isa<PtrTypeAST>()) {
                    TypeAST* inner = argType->as<PtrTypeAST>()->inner;
                    return ctx.getRefType(inner);
                }
                if (argType->isa<RefTypeAST>()) {
                    return argType;
                }
            }
            return targetType;
        }

        case IntrinsicKind::ToPtr: {
            if (!expr->args.empty() && expr->args[0]->resolvedType) {
                TypeAST* argType = expr->args[0]->resolvedType;
                if (argType->isa<RefTypeAST>()) {
                    TypeAST* inner = argType->as<RefTypeAST>()->inner;
                    return ctx.getPtrType(inner);
                }
                if (argType->isa<PtrTypeAST>()) {
                    return argType;
                }
            }
            return targetType;
        }

        case IntrinsicKind::PtrOffset: {
            if (!expr->args.empty() && expr->args[0]->resolvedType) {
                return expr->args[0]->resolvedType;
            }
            return targetType;
        }

        case IntrinsicKind::PtrDiff:
            return ctx.getIntType();

        // ─── Bitcast ──────────────────────────────────────────────────────
        case IntrinsicKind::Bitcast:
            // The return type is the target type T passed as a type argument
            if (expr->resolvedType) {
                return expr->resolvedType;
            }
            return targetType;

        // ─── String Operations ─────────────────────────────────────────────
        case IntrinsicKind::StrLen:
        case IntrinsicKind::StrFromPtr:
        case IntrinsicKind::StrConcat:
        case IntrinsicKind::StrSlice:
            return ctx.getStringType();

        case IntrinsicKind::StrPtr:
            return ctx.getPtrType(ctx.getIntType());

        case IntrinsicKind::StrEq:
            return ctx.getBoolType();

        case IntrinsicKind::StrByteAt:
            return ctx.getIntType();

        // ─── Memory Management ─────────────────────────────────────────────
        case IntrinsicKind::Alloc:
            if (expr->resolvedType) {
                return expr->resolvedType;
            }
            return ctx.getPtrType(ctx.getIntType());

        // ─── SIMD ──────────────────────────────────────────────────────────
        case IntrinsicKind::SimdLoad:
        case IntrinsicKind::SimdSplat:
        case IntrinsicKind::SimdAdd:
        case IntrinsicKind::SimdSub:
        case IntrinsicKind::SimdMul:
        case IntrinsicKind::SimdDiv:
        case IntrinsicKind::SimdFma:
        case IntrinsicKind::SimdMin:
        case IntrinsicKind::SimdMax:
        case IntrinsicKind::SimdExtract:
        case IntrinsicKind::SimdInsert:
            if (expr->resolvedType) {
                return expr->resolvedType;
            }
            return targetType;

        case IntrinsicKind::SimdStore:
            return nullptr;  // Void

        // ─── Scope Exit ────────────────────────────────────────────────────
        case IntrinsicKind::ScopeExit:
            return nullptr;  // Void

        // ─── Default ──────────────────────────────────────────────────────
        default:
            return targetType;
    }
}

ValueState getIntrinsicValueState(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return ValueState::Unknown;

    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return ValueState::Unknown;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Void intrinsics produce no value ──────────────────────────────────
    if (isIntrinsicVoidInternal(expr->intrinsicName, ctx)) {
        return ValueState::None;
    }

    switch (info->kind) {
        // ─── Memory allocations can fail ──────────────────────────────────
        case IntrinsicKind::Alloc:
            return ValueState::Unknown;

        // ─── toRef asserts non-null - always definite if it returns ──────
        case IntrinsicKind::ToRef:
            return ValueState::Definite;

        // ─── Fence and pause always succeed ──────────────────────────────
        case IntrinsicKind::Fence:
        case IntrinsicKind::Pause:
            return ValueState::Definite;

        // ─── String operations that can fail ─────────────────────────────
        case IntrinsicKind::StrFromPtr:
        case IntrinsicKind::StrConcat:
        case IntrinsicKind::StrSlice:
            return ValueState::Unknown;

        // ─── Everything else is definite ──────────────────────────────────
        default:
            return ValueState::Definite;
    }
}

// =============================================================================
// INDIVIDUAL VALIDATORS
// =============================================================================

// ─── validateFloatingPoint ──────────────────────────────────────────────────

bool validateFloatingPoint(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    for (size_t i = 0; i < expr->args.size(); ++i) {
        if (!validateNumericArg(expr->args[i], "arg" + std::to_string(i + 1), ctx)) {
            return false;
        }
    }
    return true;
}

// ─── validateMemoryOp ──────────────────────────────────────────────────────

bool validateMemoryOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    switch (info->kind) {
        case IntrinsicKind::Memcpy:
        case IntrinsicKind::Memmove:
            if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
            if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "src", ctx)) return false;
            if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
            return true;

        case IntrinsicKind::Memset:
            if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
            if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "val", ctx)) return false;
            if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
            return true;

        default:
            return true;
    }
}

// ─── validateFence ────────────────────────────────────────────────────────

bool validateFence(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "fence requires an ordering argument");
        return false;
    }

    TypeAST* result = resolveExprWithTarget(
        expr->args[0], ctx.getStringType(), ctx
    );
    if (!result || result->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "fence ordering expects a string literal");
        return false;
    }

    const LiteralExprAST* lit = expr->args[0]->as<LiteralExprAST>();
    if (!lit) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "fence ordering must be a string literal");
        return false;
    }

    std::string ordering = ctx.pool.lookup(lit->value);
    if (!IntrinsicRegistry::isValidFenceOrdering(ordering)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "invalid fence ordering — must be: relaxed, acquire, "
                              "release, acq_rel, or seq_cst");
        return false;
    }

    return true;
}

// ─── validateStringOp ──────────────────────────────────────────────────────

bool validateStringOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    switch (info->kind) {
        case IntrinsicKind::StrLen:
        case IntrinsicKind::StrPtr:
            if (!expr->args.empty() && !validateStringArg(expr->args[0], "string", ctx)) return false;
            return true;

        case IntrinsicKind::StrFromPtr:
            if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
            if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "len", ctx)) return false;
            return true;

        case IntrinsicKind::StrConcat:
        case IntrinsicKind::StrEq:
            if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "a", ctx)) return false;
            if (expr->args.size() >= 2 && !validateStringArg(expr->args[1], "b", ctx)) return false;
            return true;

        case IntrinsicKind::StrSlice:
            if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
            if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "from", ctx)) return false;
            if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "to", ctx)) return false;
            return true;

        case IntrinsicKind::StrByteAt:
            if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
            if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "index", ctx)) return false;
            return true;

        default:
            return true;
    }
}

// ─── validatePointerOp ─────────────────────────────────────────────────────

bool validatePointerOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    switch (info->kind) {
        case IntrinsicKind::Addrof:
            // addrof can take any expression - returns *T
            return true;

        case IntrinsicKind::ToRef:
            if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
            return true;

        case IntrinsicKind::ToPtr:
            if (!expr->args.empty() && !validateRefArg(expr->args[0], "ref", ctx)) return false;
            return true;

        case IntrinsicKind::PtrOffset:
            if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
            if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "offset", ctx)) return false;
            return true;

        case IntrinsicKind::PtrDiff:
            if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "p1", ctx)) return false;
            if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "p2", ctx)) return false;
            return true;

        default:
            return true;
    }
}

// ─── validateAtomicOp ─────────────────────────────────────────────────────

bool validateAtomicOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    // ─── All atomics require a pointer as the first argument ──────────────
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "atomic intrinsic requires a pointer argument");
        return false;
    }

    // ─── Validate the pointer argument ────────────────────────────────────
    if (!validatePtrArg(expr->args[0], "ptr", ctx)) {
        return false;
    }

    // ─── Validate ordering (last argument, if present) ────────────────────
    if (expr->args.size() >= 2) {
        ExprAST* lastArg = expr->args[expr->args.size() - 1];
        TypeAST* result = resolveExprWithTarget(
            lastArg, ctx.getStringType(), ctx
        );
        if (!result || result->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, lastArg,
                                  "atomic ordering expects a string literal");
            return false;
        }

        const LiteralExprAST* lit = lastArg->as<LiteralExprAST>();
        if (!lit) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, lastArg,
                                  "atomic ordering must be a string literal");
            return false;
        }

        std::string ordering = ctx.pool.lookup(lit->value);
        if (!IntrinsicRegistry::isValidFenceOrdering(ordering)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, lastArg,
                                  "invalid ordering — must be: relaxed, acquire, "
                                  "release, acq_rel, or seq_cst");
            return false;
        }
    }

    return true;
}

// ─── validateSIMD ──────────────────────────────────────────────────────────

bool validateSIMD(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;
    
    std::string_view name = lookupStringView(expr->intrinsicName);
    
    // ─── #simd_splat(type, lanes, scalar) ──────────────────────────────────
    if (name == "simd_splat") {
        if (expr->args.size() != 3) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                "#simd_splat expects 3 arguments: (type, lanes, scalar)");
            return false;
        }
        
        ExprAST* typeArg = expr->args[0];
        
        // Use the shared helper that handles generic parameters correctly
        TypeAST* elementType = resolveTypeArgument(typeArg, ctx);
        
        if (!elementType) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, typeArg,
                                "#simd_splat: first argument must be a type "
                                "(numeric primitive like int32, float64)");
            return false;
        }
        
        // ─── Validate SIMD element type is concrete ─────────────────────────────
        // This checks that the type is not a type-erased generic (@[erased]).
        // By default, generics are specialized, so Simd<T,N> works normally.
        if (!validateConcreteTypeForSimd(elementType, expr, ctx)) {
            return false;
        }
        
        if (!isValidSimdElementType(elementType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, typeArg,
                                "#simd_splat: type must be a numeric primitive "
                                "(int8, int16, int32, int64, uint8, uint16, uint32, "
                                "uint64, float32, or float64)");
            ctx.diagnostics.note(typeArg,
                                "Got: ", typeToString(elementType, ctx.pool));
            return false;
        }
        
        // Validate lanes
        ExprAST* lanesArg = expr->args[1];
        if (!lanesArg->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, lanesArg,
                                "#simd_splat: lanes must be an integer literal");
            return false;
        }
        
        int64_t laneCount = ctx.parseConstantInt(lanesArg);
        if (laneCount <= 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdLaneCount, lanesArg,
                                "#simd_splat: lane count must be > 0");
            return false;
        }
        
        // Validate scalar matches type
        ExprAST* scalar = expr->args[2];
        TypeAST* scalarType = resolveExpr(scalar, ctx);
        if (!scalarType || scalarType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, scalar,
                                "#simd_splat: scalar argument has unknown type");
            return false;
        }
        
        if (!typesEqual(elementType, scalarType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, scalar,
                                "#simd_splat: scalar type (", 
                                typeToString(scalarType, ctx.pool),
                                ") does not match specified type (",
                                typeToString(elementType, ctx.pool), ")");
            return false;
        }
        
        SimdTypeAST* simdType = ctx.arena.make<SimdTypeAST>(elementType, static_cast<uint64_t>(laneCount));
        simdType->loc = expr->loc;
        expr->resolvedType = simdType;
        
        return true;
    }
    
    // ─── #simd_load(ptr, lanes) ─────────────────────────────────────────
    if (name == "simd_load") {
        if (expr->args.size() != 2) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_load expects 2 arguments: (ptr, lanes)");
            return false;
        }
        
        ExprAST* ptr = expr->args[0];
        if (!ptr || !ptr->resolvedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "#simd_load: pointer argument has no type");
            return false;
        }
        
        if (!ptr->resolvedType->isa<PtrTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, ptr,
                                  "#simd_load: first argument must be a pointer");
            return false;
        }
        
        PtrTypeAST* ptrInner = ptr->resolvedType->as<PtrTypeAST>();
        
        // Validate SIMD element type is concrete and valid
        if (!validateConcreteTypeForSimd(ptrInner->inner, expr, ctx)) {
            return false;
        }
        
        if (!isValidSimdElementType(ptrInner->inner)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, ptr,
                                  "#simd_load: pointer must point to a numeric primitive");
            return false;
        }
        
        // Validate lanes
        ExprAST* lanesArg = expr->args[1];
        if (!lanesArg->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, lanesArg,
                                  "#simd_load: lanes must be an integer literal");
            return false;
        }
        
        int64_t laneCount = ctx.parseConstantInt(lanesArg);
        if (laneCount <= 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdLaneCount, lanesArg,
                                  "#simd_load: lane count must be > 0");
            return false;
        }
        
        SimdTypeAST* simdType = ctx.arena.make<SimdTypeAST>(
            ptrInner->inner, 
            static_cast<uint64_t>(laneCount)
        );
        simdType->loc = expr->loc;
        expr->resolvedType = simdType;
        
        return true;
    }
    
    // ─── #simd_store(ptr, simd_value) ────────────────────────────────────
    if (name == "simd_store") {
        if (expr->args.size() != 2) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_store expects 2 arguments: (ptr, simd_value)");
            return false;
        }
        
        ExprAST* ptr = expr->args[0];
        ExprAST* vec = expr->args[1];
        
        if (!ptr || !ptr->resolvedType || !vec || !vec->resolvedType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "#simd_store: arguments must have types");
            return false;
        }
        
        if (!ptr->resolvedType->isa<PtrTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, ptr,
                                  "#simd_store: first argument must be a pointer");
            return false;
        }
        
        if (!isSimdType(vec->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, vec,
                                  "#simd_store: second argument must be a Simd type");
            return false;
        }
        
        TypeAST* ptrElem = ptr->resolvedType->as<PtrTypeAST>()->inner;
        TypeAST* simdElem = getSimdElementType(vec->resolvedType);
        if (!typesEqual(ptrElem, simdElem)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_store: pointer element type and Simd element type must match");
            return false;
        }
        
        return true;
    }
    
    // ─── Binary SIMD ops ──────────────────────────────────────────────────
    if (name == "simd_add" || name == "simd_sub" || 
        name == "simd_mul" || name == "simd_div" ||
        name == "simd_min" || name == "simd_max") {
        
        if (expr->args.size() != 2) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#", name, " expects 2 arguments");
            return false;
        }
        
        ExprAST* a = expr->args[0];
        ExprAST* b = expr->args[1];
        
        if (!a || !a->resolvedType || !b || !b->resolvedType) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#", name, ": arguments must have types");
            return false;
        }
        
        if (!isSimdType(a->resolvedType) || !isSimdType(b->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#", name, ": both arguments must be Simd types");
            return false;
        }
        
        if (!typesEqual(a->resolvedType, b->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#", name, ": both Simd arguments must have identical types");
            return false;
        }
        
        return true;
    }
    
    // ─── #simd_fma(a, b, c) ────────────────────────────────────────────────
    if (name == "simd_fma") {
        if (expr->args.size() != 3) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_fma expects 3 arguments");
            return false;
        }
        
        ExprAST* a = expr->args[0];
        ExprAST* b = expr->args[1];
        ExprAST* c = expr->args[2];
        
        if (!a || !a->resolvedType || !b || !b->resolvedType || !c || !c->resolvedType) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_fma: arguments must have types");
            return false;
        }
        
        if (!isSimdType(a->resolvedType) || !isSimdType(b->resolvedType) || !isSimdType(c->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_fma: all arguments must be Simd types");
            return false;
        }
        
        if (!typesEqual(a->resolvedType, b->resolvedType) || !typesEqual(b->resolvedType, c->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_fma: all arguments must have identical Simd types");
            return false;
        }
        
        return true;
    }
    
    // ─── #simd_extract(v, index) ──────────────────────────────────────────
    if (name == "simd_extract") {
        if (expr->args.size() != 2) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_extract expects 2 arguments: (v, index)");
            return false;
        }
        
        ExprAST* vec = expr->args[0];
        if (!vec || !vec->resolvedType || !isSimdType(vec->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, vec,
                                  "#simd_extract: first argument must be a Simd type");
            return false;
        }
        
        ExprAST* idx = expr->args[1];
        if (!idx || !idx->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "#simd_extract: index must be an integer literal");
            return false;
        }
        
        int64_t index = ctx.parseConstantInt(idx);
        if (index < 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                                  "#simd_extract: index must be >= 0");
            return false;
        }
        
        return true;
    }
    
    // ─── #simd_insert(v, index, value) ────────────────────────────────────
    if (name == "simd_insert") {
        if (expr->args.size() != 3) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_insert expects 3 arguments: (v, index, value)");
            return false;
        }
        
        ExprAST* vec = expr->args[0];
        ExprAST* val = expr->args[2];
        
        if (!vec || !vec->resolvedType || !isSimdType(vec->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, vec,
                                  "#simd_insert: first argument must be a Simd type");
            return false;
        }
        
        if (!val || !val->resolvedType) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_insert: value argument must have a type");
            return false;
        }
        
        TypeAST* simdElem = getSimdElementType(vec->resolvedType);
        if (!typesEqual(simdElem, val->resolvedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#simd_insert: value type must match Simd element type");
            return false;
        }
        
        ExprAST* idx = expr->args[1];
        if (!idx || !idx->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "#simd_insert: index must be an integer literal");
            return false;
        }
        
        return true;
    }
    
    ctx.diagnostics.error(DiagCode::Sem_UnknownIntrinsic, expr,
                          "unknown SIMD intrinsic '#", name, "'");
    return false;
}

// ─── validateMemoryManagement ─────────────────────────────────────────────

bool validateMemoryManagement(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    switch (info->kind) {
        case IntrinsicKind::Alloc: {
            if (expr->args.empty()) {
                ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                    "#alloc expects 2 arguments: (type, count)");
                return false;
            }
            
            TypeAST* elementType = resolveTypeArgument(expr->args[0], ctx);
            
            if (!elementType) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                                    "#alloc expects a type as the first argument");
                return false;
            }
            
            // ─── Validate the element type is concrete for #alloc ──────────────
            // This checks that the type is not a type-erased generic (@[erased]).
            // By default, generics are specialized, so #alloc works normally.
            if (!validateConcreteTypeForAlloc(elementType, expr, ctx)) {
                return false;
            }
            
            if (expr->args.size() < 2) {
                ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                    "#alloc expects 2 arguments: (type, count)");
                return false;
            }
            
            if (!validateIntArg(expr->args[1], "count", ctx)) {
                return false;
            }
            
            PtrTypeAST* ptrType = ctx.getPtrType(elementType);
            expr->resolvedType = ptrType;
            
            return true;
        }

        case IntrinsicKind::Free:
            if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
            return true;

        default:
            return true;
    }
}

// ─── validateBitcast ──────────────────────────────────────────────────────

bool validateBitcast(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.size() != 2) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#bitcast expects 2 arguments: (type, value)");
        return false;
    }
    
    TypeAST* targetType = resolveTypeArgument(expr->args[0], ctx);
    
    if (!targetType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "#bitcast expects a type as the first argument");
        return false;
    }
    
    // ─── Validate the target type is concrete for #bitcast ───────────────────
    // This checks that the type is not a type-erased generic (@[erased]).
    // By default, generics are specialized, so #bitcast works normally.
    if (!validateConcreteTypeForBitcast(targetType, expr, ctx)) {
        return false;
    }
    
    targetType = resolveType(targetType, ctx);
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        return false;
    }
    
    ExprAST* valueArg = expr->args[1];
    TypeAST* valueType = resolveExpr(valueArg, ctx);
    if (!valueType || valueType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, valueArg,
                              "#bitcast: value argument has unknown type");
        return false;
    }
    
    expr->resolvedType = targetType;
    expr->valueState = ValueState::Definite;
    
    return true;
}

// ─── validateTostr ─────────────────────────────────────────────────────────

bool validateTostr(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;
    
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#tostr requires exactly 1 argument");
        return false;
    }
    
    if (expr->args.size() > 1) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#tostr expects exactly 1 argument, got ", 
                              expr->args.size());
        return false;
    }
    
    ExprAST* arg = expr->args[0];
    if (!arg->resolvedType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "#tostr argument has unknown type");
        return false;
    }
    
    TypeAST* argType = arg->resolvedType;
    
    // ─── Function types are ALLOWED ──────────────────────────────────────
    if (argType->isa<FuncTypeAST>()) {
        return true;
    }
    
    // ─── Validate the type is concrete for #tostr ────────────────────────────
    // This checks that the type is not a type-erased generic (@[erased]).
    // By default, generics are specialized, so #tostr works normally.
    if (!validateConcreteTypeForReflection(argType, expr, ctx, "tostr")) {
        return false;
    }
    
    // ─── Reject trait types ──────────────────────────────────────────────
    if (argType->isa<NamedTypeAST>()) {
        NamedTypeAST* named = argType->as<NamedTypeAST>();
        if (named->resolvedDecl && named->resolvedDecl->isa<TraitDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "#tostr cannot be used with trait type '", 
                                  ctx.pool.lookup(named->name), "'");
            ctx.diagnostics.note(expr,
                                 "Traits are field contracts, not concrete types");
            return false;
        }
    }
    
    return true;
}

// ─── validateSizeof ───────────────────────────────────────────────────────

bool validateSizeof(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#sizeof expects 1 argument");
        return false;
    }
    
    TypeAST* type = resolveTypeArgument(expr->args[0], ctx);
    
    if (!type) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "#sizeof expects a type, got an expression");
        return false;
    }
    
    // ─── Validate the type is concrete for #sizeof ──────────────────────────
    // This checks that the type is not a type-erased generic (@[erased]).
    // By default, generics are specialized, so #sizeof works normally.
    if (!validateConcreteTypeForReflection(type, expr, ctx, "sizeof")) {
        return false;
    }
    
    return true;
}

// ─── validateAlignof ──────────────────────────────────────────────────────

bool validateAlignof(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#alignof expects 1 argument");
        return false;
    }
    
    TypeAST* type = resolveTypeArgument(expr->args[0], ctx);
    
    if (!type) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[0],
                              "#alignof expects a type, got an expression");
        return false;
    }
    
    // ─── Validate the type is concrete for #alignof ─────────────────────────
    // This checks that the type is not a type-erased generic (@[erased]).
    // By default, generics are specialized, so #alignof works normally.
    if (!validateConcreteTypeForReflection(type, expr, ctx, "alignof")) {
        return false;
    }
    
    return true;
}

// ─── validateScopeExit ────────────────────────────────────────────────────

bool validateScopeExit(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, expr,
                              "#scope_exit is only valid inside a function body");
        return false;
    }

    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#scope_exit expects at least 1 argument");
        return false;
    }

    ExprAST* funcArg = expr->args[0];
    TypeAST* funcType = funcArg->resolvedType;
    
    if (!funcType || funcType->isa<UnknownTypeAST>()) {
        funcType = resolveExpr(funcArg, ctx);
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                                  "#scope_exit argument has unknown type");
            return false;
        }
    }

    if (!funcType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit expects a function as the first argument, got ",
                              typeToString(funcType, ctx.pool));
        return false;
    }

    FuncTypeAST* func = funcType->as<FuncTypeAST>();

    // ─── Handle generic function references ─────────────────────────────
    bool hasGenericArgs = false;
    FuncDeclAST* funcDecl = nullptr;
    
    if (funcArg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = funcArg->as<IdentifierExprAST>();
        hasGenericArgs = !id->genericArgs.empty();
        
        ValueDeclAST* decl = ctx.lookupValue(id->name);
        if (decl && decl->isa<FuncDeclAST>()) {
            funcDecl = decl->as<FuncDeclAST>();
        }
    } else if (funcArg->isa<ModuleAccessExprAST>()) {
        ModuleAccessExprAST* mod = funcArg->as<ModuleAccessExprAST>();
        hasGenericArgs = !mod->genericArgs.empty();
        
        ValueDeclAST* decl = ctx.lookupValueByAlias(mod->moduleName, mod->memberName);
        if (decl && decl->isa<FuncDeclAST>()) {
            funcDecl = decl->as<FuncDeclAST>();
        }
    }

    // ─── Validate generic instantiation ─────────────────────────────────
    if (funcDecl) {
        bool hasGenericParams = !funcDecl->genericParams.empty();
        
        if (hasGenericParams && !hasGenericArgs) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, funcArg,
                                  "#scope_exit callback '", ctx.pool.lookup(funcDecl->name),
                                  "' has generic parameters but no generic arguments");
            return false;
        }
        
        if (!hasGenericParams && hasGenericArgs) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, funcArg,
                                  "#scope_exit callback '", ctx.pool.lookup(funcDecl->name),
                                  "' is not generic but generic arguments were provided");
            return false;
        }
    }

    if (func->isCurried()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must have exactly one parameter group");
        return false;
    }

    if (func->returnType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must return void");
        return false;
    }

    for (ParamAST* param : func->params) {
        if (param->isVariadic) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "#scope_exit callback cannot have variadic parameters");
            return false;
        }
    }

    size_t callbackArgs = expr->args.size() - 1;
    size_t paramCount = func->params.size();

    if (callbackArgs != paramCount) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#scope_exit callback expects ", paramCount,
                              " argument(s), got ", callbackArgs);
        return false;
    }

    auto argsBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    for (size_t i = 0; i < callbackArgs; ++i) {
        ExprAST* arg = expr->args[i + 1];
        TypeAST* expectedType = func->params[i]->type;

        TypeAST* argType = resolveExprWithTarget(arg, expectedType, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            return false;
        }

        if (arg->valueState == ValueState::Nil && !isNullableType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                                  "cannot pass nil to non-nullable parameter in #scope_exit callback");
            return false;
        }

        if (arg->valueState == ValueState::Err && !isFallibleType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                                  "cannot pass err to non-fallible parameter in #scope_exit callback");
            return false;
        }

        argsBuilder.push_back(arg);
    }

    if (funcArg->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* field = funcArg->as<FieldAccessExprAST>();
        ctx.diagnostics.warning(DiagCode::Warn_UnsafeFFI, funcArg,
                                "function reference from struct field '",
                                ctx.pool.lookup(field->fieldName),
                                "' may capture the struct's lifetime");
    }

    BlockStmtAST* currentBlock = ctx.stack.currentBlock();
    if (!currentBlock) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, expr,
                              "#scope_exit must appear inside a block");
        return false;
    }

    ScopeExitRegistration* registration = ctx.arena.make<ScopeExitRegistration>();
    registration->callExpr = expr;
    registration->callback = funcDecl;
    registration->args = argsBuilder.build();

    auto exitsBuilder = ctx.arena.makeBuilder<ScopeExitRegistrationPtr>();
    for (ScopeExitRegistrationPtr existing : currentBlock->scopeExits) {
        exitsBuilder.push_back(existing);
    }
    exitsBuilder.push_back(registration);
    currentBlock->scopeExits = exitsBuilder.build();

    Trace::info("validateScopeExit: registered #scope_exit in block with ",
             currentBlock->scopeExits.size(), " total registrations");

    return true;
}

} // namespace sema