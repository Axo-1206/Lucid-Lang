/// @file registry/IntrinsicValidator.cpp
/// @brief Implementation of semantic validation for intrinsics.

#include "IntrinsicValidator.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/Sema.hpp"

#include <unordered_set>

namespace sema {

// ─── Forward Declarations ──────────────────────────────────────────────────

static bool isArgumentCountValid(size_t count, const IntrinsicInfo* info);

// ─── Public API ────────────────────────────────────────────────────────────

bool validateIntrinsicCall(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);

    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) {
        ctx.diagnostics.error(DiagCode::Sem_UnknownIntrinsic, expr,
                              "unknown intrinsic '#", ctx.pool.lookup(expr->intrinsicName), "'");
        return false;
    }

    if (!validateIntrinsicArgCount(expr->intrinsicName, expr->args.size(), ctx)) {
        return false;
    }

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Dispatch to specific validator ────────────────────────────────────
    // Floating-Point Math
    static const std::unordered_set<std::string> FLOATING_POINT_OPS = {
        "sqrt", "abs", "fma", "ceil", "floor", "round", "pow", "min", "max"
    };
    if (FLOATING_POINT_OPS.find(name) != FLOATING_POINT_OPS.end()) {
        return validateFloatingPoint(expr, ctx);
    }

    // Memory Operations
    static const std::unordered_set<std::string> MEMORY_OPS = {
        "memcpy", "memmove", "memset"
    };
    if (MEMORY_OPS.find(name) != MEMORY_OPS.end()) {
        return validateMemoryOp(expr, ctx);
    }

    if (name == "fence") {
        return validateFence(expr, ctx);
    }

    // String Operations
    static const std::unordered_set<std::string> STRING_OPS = {
        "str_len", "str_ptr", "str_from_ptr", "str_concat",
        "str_eq", "str_slice", "str_byte_at"
    };
    if (STRING_OPS.find(name) != STRING_OPS.end()) {
        return validateStringOp(expr, ctx);
    }

    // Pointer Operations
    static const std::unordered_set<std::string> POINTER_OPS = {
        "addrof", "toRef", "toPtr", "ptrOffset", "ptrDiff"
    };
    if (POINTER_OPS.find(name) != POINTER_OPS.end()) {
        return validatePointerOp(expr, ctx);
    }

    if (name == "scope_exit") {
        return validateScopeExit(expr, ctx);
    }

    if (name.find("atomic_") == 0) {
        return validateAtomicOp(expr, ctx);
    }

    if (name.find("simd_") == 0) {
        return validateSIMD(expr, ctx);
    }

    // Memory Management
    static const std::unordered_set<std::string> MEMORY_MGMT_OPS = {
        "alloc", "free", "arena_create", "arena_alloc", "arena_reset", "arena_free"
    };
    if (MEMORY_MGMT_OPS.find(name) != MEMORY_MGMT_OPS.end()) {
        return validateMemoryManagement(expr, ctx);
    }

    // ─── Compiler-handled intrinsics with minimal validation ─────────────
    if (name == "clz" || name == "ctz" || name == "popcount" || name == "bswap") {
        if (!expr->args.empty() && !validateIntArg(expr->args[0], "value", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "prefetch" || name == "prefetch_r" || name == "prefetch_w") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "likely" || name == "unlikely") {
        if (!expr->args.empty() && !validateBoolArg(expr->args[0], "condition", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "pause") {
        return true;
    }

    // ─── Type & Value Inspection ──────────────────────────────────────────
    static const std::unordered_set<std::string> INSPECTION_OPS = {
        "sizeof", "alignof", "typeof", "nameof", "tostr", "ptrstr", "bitcast"
    };
    if (INSPECTION_OPS.find(name) != INSPECTION_OPS.end()) {
        return true;
    }

    return true;
}

bool validateIntrinsicArgCount(InternedString name, size_t count, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(name);
    if (!info) return false;
    return isArgumentCountValid(count, info);
}

const TypeAST* getIntrinsicReturnType(const IntrinsicCallExprAST* expr,
                                       const TypeAST* targetType,
                                       SemaContext& ctx) {
    if (!expr) return targetType;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Scope Exit returns void ──────────────────────────────────────────
    if (name == "scope_exit") {
        return nullptr;
    }

    // ─── Type/Value Inspection ─────────────────────────────────────────────
    if (name == "sizeof" || name == "alignof") {
        return ctx.getIntType();
    }

    if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr") {
        return ctx.getStringType();
    }

    if (name == "addrof") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            return ctx.arena.make<PtrTypeAST>(expr->args[0]->resolvedType);
        }
        return targetType;
    }

    if (name == "toRef") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            const TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<PtrTypeAST>()) {
                return ctx.arena.make<RefTypeAST>(
                    argType->as<PtrTypeAST>()->inner
                );
            }
        }
        return targetType;
    }

    if (name == "toPtr") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            const TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<RefTypeAST>()) {
                return ctx.arena.make<PtrTypeAST>(
                    argType->as<RefTypeAST>()->inner
                );
            }
        }
        return targetType;
    }

    if (name == "bitcast") {
        return targetType;
    }

    // ─── String Operations ──────────────────────────────────────────────────
    if (name == "str_len" || name == "str_from_ptr" || name == "str_concat" ||
        name == "str_slice") {
        return ctx.getStringType();
    }

    if (name == "str_ptr") {
        return ctx.arena.make<PtrTypeAST>(ctx.getIntType());
    }

    if (name == "str_eq") {
        return ctx.getBoolType();
    }

    if (name == "str_byte_at") {
        return ctx.getIntType();
    }

    // ─── Memory Management ──────────────────────────────────────────────────
    if (name == "alloc" || name == "arena_alloc") {
        return ctx.arena.make<PtrTypeAST>(ctx.getIntType());
    }

    if (name == "arena_create") {
        // Return ArenaDescriptor struct type
        return targetType;
    }

    // ─── SIMD ────────────────────────────────────────────────────────────────
    if (name.find("simd_") == 0) {
        return targetType;
    }

    return targetType;
}

ValueState getIntrinsicValueState(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return ValueState::Unknown;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Scope Exit produces no value ─────────────────────────────────────
    if (name == "scope_exit") {
        return ValueState::None;
    }

    if (name == "alloc" || name == "arena_alloc") {
        return ValueState::Unknown;
    }

    if (name == "toRef") {
        return ValueState::Definite;
    }

    if (name == "fence" || name == "pause") {
        return ValueState::Definite;
    }

    return ValueState::Definite;
}

// ─── Individual Validators ─────────────────────────────────────────────────

bool validateFloatingPoint(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    for (size_t i = 0; i < expr->args.size(); ++i) {
        if (!validateNumericArg(expr->args[i], "arg" + std::to_string(i + 1), ctx)) {
            return false;
        }
    }
    return true;
}

bool validateMemoryOp(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "memcpy" || name == "memmove") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
        if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "src", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
        return true;
    }

    if (name == "memset") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "dst", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "val", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "len", ctx)) return false;
        return true;
    }

    return true;
}

bool validateFence(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "fence requires an ordering argument");
        return false;
    }

    TypeAST* result = resolveExprWithTarget(
        const_cast<ExprAST*>(expr->args[0]), ctx.getStringType(), ctx
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

bool validateStringOp(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "str_len" || name == "str_ptr") {
        if (!expr->args.empty() && !validateStringArg(expr->args[0], "string", ctx)) return false;
        return true;
    }

    if (name == "str_from_ptr") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "len", ctx)) return false;
        return true;
    }

    if (name == "str_concat" || name == "str_eq") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "a", ctx)) return false;
        if (expr->args.size() >= 2 && !validateStringArg(expr->args[1], "b", ctx)) return false;
        return true;
    }

    if (name == "str_slice") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "from", ctx)) return false;
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "to", ctx)) return false;
        return true;
    }

    if (name == "str_byte_at") {
        if (expr->args.size() >= 1 && !validateStringArg(expr->args[0], "string", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "index", ctx)) return false;
        return true;
    }

    return true;
}

bool validatePointerOp(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "addrof") {
        return true;
    }

    if (name == "toRef") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "toPtr") {
        if (!expr->args.empty() && !validateRefArg(expr->args[0], "ref", ctx)) return false;
        return true;
    }

    if (name == "ptrOffset") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "offset", ctx)) return false;
        return true;
    }

    if (name == "ptrDiff") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "p1", ctx)) return false;
        if (expr->args.size() >= 2 && !validatePtrArg(expr->args[1], "p2", ctx)) return false;
        return true;
    }

    return true;
}

bool validateScopeExit(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    // ─── 1. Must be inside a function body ────────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, expr,
                              "#scope_exit is only valid inside a function body "
                              "(not at module scope or inside const initializers)");
        return false;
    }

    // ─── 2. Must have at least one argument (the function) ────────────────
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#scope_exit expects at least 1 argument (the function to call)");
        return false;
    }

    // ─── 3. Validate the first argument is a function value ───────────────
    const ExprAST* funcArg = expr->args[0];
    TypeAST* funcType = funcArg->resolvedType;
    if (!funcType || !funcType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit expects a function as the first argument, got ",
                              debug::typeToString(funcType, ctx.pool));
        return false;
    }

    const FuncTypeAST* func = funcType->as<FuncTypeAST>();

    // ─── 4. The function must have exactly one parameter group ─────────────
    if (func->isCurried()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must have exactly one parameter group "
                              "(curried functions are not allowed)");
        return false;
    }

    // ─── 5. The function must return void ──────────────────────────────────
    if (func->returnType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must return void "
                              "(cannot return a value during unwinding)");
        return false;
    }

    // ─── 6. Validate argument count against function parameters ───────────
    size_t callbackArgs = expr->args.size() - 1;
    size_t paramCount = func->params.size();

    bool hasVariadic = false;
    size_t variadicIndex = paramCount;
    for (size_t i = 0; i < paramCount; ++i) {
        if (func->params[i]->isVariadic) {
            hasVariadic = true;
            variadicIndex = i;
            break;
        }
    }

    if (hasVariadic) {
        if (callbackArgs < variadicIndex) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#scope_exit callback expects at least ", variadicIndex,
                                  " argument(s) before variadic, got ", callbackArgs);
            return false;
        }
    } else {
        if (callbackArgs != paramCount) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#scope_exit callback expects ", paramCount,
                                  " argument(s), got ", callbackArgs);
            return false;
        }
    }

    // ─── 7. Validate each argument type against callback parameters ───────
    for (size_t i = 0; i < callbackArgs; ++i) {
        const ExprAST* arg = expr->args[i + 1];
        const TypeAST* expectedType = nullptr;

        if (hasVariadic && i >= variadicIndex) {
            const ParamAST* variadicParam = func->params[variadicIndex];
            if (variadicParam->type->isa<ArrayTypeAST>()) {
                expectedType = variadicParam->type->as<ArrayTypeAST>()->element;
            } else {
                ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, arg,
                                      "variadic parameter has invalid type (expected array)");
                return false;
            }
        } else {
            expectedType = func->params[i]->type;
        }

        TypeAST* argType = resolveExprWithTarget(
            const_cast<ExprAST*>(arg), expectedType, ctx
        );
        if (!argType || argType->isa<UnknownTypeAST>()) {
            return false;
        }

        // ─── Use public SemaCompare functions ──────────────────────────────
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
    }

    return true;
}

bool validateAtomicOp(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;

    if (expr->args.size() >= 2) {
        const ExprAST* lastArg = expr->args[expr->args.size() - 1];
        TypeAST* result = resolveExprWithTarget(
            const_cast<ExprAST*>(lastArg), ctx.getStringType(), ctx
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

bool validateSIMD(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "simd_splat") {
        if (expr->args.size() >= 2) {
            TypeAST* nType = resolveExprWithTarget(
                const_cast<ExprAST*>(expr->args[1]), ctx.getIntType(), ctx
            );
            if (!nType || nType->isa<UnknownTypeAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[1],
                                      "argument 'N' must be a compile-time integer constant");
                return false;
            }

            if (!expr->args[1]->isa<LiteralExprAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->args[1],
                                      "argument 'N' must be a compile-time integer constant");
                return false;
            }
        }
        if (expr->args.size() >= 3 && !validateNumericArg(expr->args[2], "scalar", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "simd_load") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "simd_store") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    return true;
}

bool validateMemoryManagement(const IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "alloc") {
        if (expr->args.size() >= 2 && !validateIntArg(expr->args[1], "count", ctx)) {
            return false;
        }
        return true;
    }

    if (name == "free") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        return true;
    }

    if (name == "arena_create") {
        if (!expr->args.empty() && !validateIntArg(expr->args[0], "size", ctx)) return false;
        return true;
    }

    if (name == "arena_alloc") {
        if (expr->args.size() >= 3 && !validateIntArg(expr->args[2], "count", ctx)) {
            return false;
        }
        return true;
    }

    return true;
}

// ─── Argument Type Validators ─────────────────────────────────────────────

bool validatePtrArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !arg->resolvedType->isa<PtrTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects pointer type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateNumericArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isNumericType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects numeric type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateIntArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isIntegerType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects integer type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateStringArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isStringType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects string type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateBoolArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isBoolType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects boolean type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateRefArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !arg->resolvedType->isa<RefTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects reference type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

// ─── Internal Helpers ──────────────────────────────────────────────────────

static bool isArgumentCountValid(size_t count, const IntrinsicInfo* info) {
    if (info->isVarArg) {
        return count >= info->minArgs;
    }
    return count >= info->minArgs && count <= info->maxArgs;
}

} // namespace sema