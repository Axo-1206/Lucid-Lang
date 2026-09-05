/// @file registry/IntrinsicValidator.cpp
/// @brief Implementation of semantic validation for intrinsics.

#include "IntrinsicValidator.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include "core/ASTStrings.hpp"
#include "ArgTypeValidators.hpp"
#include "sema/Sema.hpp"

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

// ─── Helper: Check if a type is a generic parameter ──────────────────────

static bool isGenericParameterType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    return ctx.isGenericParam(named->name);
}

// ─── Helper: Check if a type contains a generic parameter ────────────────

static bool containsGenericParameter(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (ctx.isGenericParam(named->name)) {
            return true;
        }
        // Check generic arguments of a named type (e.g., Box<T>)
        for (TypeAST* arg : named->genericArgs) {
            if (containsGenericParameter(arg, ctx)) {
                return true;
            }
        }
        return false;
    }
    
    if (type->isa<ArrayTypeAST>()) {
        ArrayTypeAST* array = type->as<ArrayTypeAST>();
        return containsGenericParameter(array->element, ctx);
    }
    
    if (type->isa<NullableTypeAST>()) {
        NullableTypeAST* nullable = type->as<NullableTypeAST>();
        return containsGenericParameter(nullable->inner, ctx);
    }
    
    if (type->isa<FallibleTypeAST>()) {
        FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
        return containsGenericParameter(fallible->inner, ctx);
    }
    
    if (type->isa<CombinedTypeAST>()) {
        CombinedTypeAST* combined = type->as<CombinedTypeAST>();
        return containsGenericParameter(combined->inner, ctx);
    }
    
    if (type->isa<PtrTypeAST>()) {
        PtrTypeAST* ptr = type->as<PtrTypeAST>();
        return containsGenericParameter(ptr->inner, ctx);
    }
    
    if (type->isa<RefTypeAST>()) {
        RefTypeAST* ref = type->as<RefTypeAST>();
        return containsGenericParameter(ref->inner, ctx);
    }
    
    if (type->isa<FuncTypeAST>()) {
        FuncTypeAST* func = type->as<FuncTypeAST>();
        for (ParamAST* param : func->params) {
            if (containsGenericParameter(param->type, ctx)) {
                return true;
            }
        }
        if (func->returnType && containsGenericParameter(func->returnType, ctx)) {
            return true;
        }
        return false;
    }
    
    return false;  // Primitive types don't contain generic parameters
}

// ─── Helper: Check if we're inside a generic function ─────────────────────

static bool isInsideGenericFunction(SemaContext& ctx) {
    FuncDeclAST* func = ctx.stack.getInnermostFunction();
    return func && !func->genericParams.empty();
}

// ─── Helper: Check if we're inside a specialized function ─────────────────

static bool isInsideSpecializedFunction(SemaContext& ctx) {
    FuncDeclAST* func = ctx.stack.getInnermostFunction();
    return func && func->shouldSpecialize;
}

// ─── Simd Helpers ────────────────────────────────────────────────────────

/// @brief Parse a compile-time integer constant.
static int64_t parseConstantInt(ExprAST* expr, SemaContext& ctx) {
    if (!expr->isa<LiteralExprAST>()) return 0;
    LiteralExprAST* lit = expr->as<LiteralExprAST>();
    try {
        std::string valStr = ctx.pool.lookup(lit->value);
        return std::stoll(valStr, nullptr, 0);
    } catch (const std::exception& e) {
        return 0;
    }
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

    // ─── Dispatch by IntrinsicKind ─────────────────────────────────────────
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

// ─── Individual Validators ─────────────────────────────────────────────────

bool validateFloatingPoint(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    for (size_t i = 0; i < expr->args.size(); ++i) {
        if (!validateNumericArg(expr->args[i], "arg" + std::to_string(i + 1), ctx)) {
            return false;
        }
    }
    return true;
}

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

bool validateScopeExit(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    // ─── 1. Must be inside a function body ────────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, expr,
                              "#scope_exit is only valid inside a function body "
                              "(not at module scope or inside const initializers)");
        return false;
    }

    // ─── 2. Must have at least one argument (the function to call) ────────
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#scope_exit expects at least 1 argument (the function to call)");
        return false;
    }

    // ─── 3. Resolve and validate the first argument ────────────────────────
    ExprAST* funcArg = expr->args[0];
    TypeAST* funcType = funcArg->resolvedType;
    
    // First, resolve the argument if not already resolved
    if (!funcType || funcType->isa<UnknownTypeAST>()) {
        funcType = resolveExpr(funcArg, ctx);
        if (!funcType || funcType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                                  "#scope_exit argument has unknown type");
            return false;
        }
    }

    // ─── 4. Verify it's a function type ────────────────────────────────────
    if (!funcType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit expects a function as the first argument, got ",
                              typeToString(funcType, ctx.pool));
        return false;
    }

    FuncTypeAST* func = funcType->as<FuncTypeAST>();

    // ─── 5. Handle generic function references ─────────────────────────────
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
    } else if (funcArg->isa<AnonFuncExprAST>()) {
        // Anonymous functions are always fully resolved
        // No generic handling needed
    }
    
    // ─── 6. Validate generic instantiation ─────────────────────────────────
    if (funcDecl) {
        bool hasGenericParams = !funcDecl->genericParams.empty();
        
        if (hasGenericParams && !hasGenericArgs) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, funcArg,
                                  "#scope_exit callback '", ctx.pool.lookup(funcDecl->name),
                                  "' has generic parameters but no generic arguments were provided");
            ctx.diagnostics.note(funcArg,
                                 "Instantiate the generic function: '#scope_exit(",
                                 ctx.pool.lookup(funcDecl->name), "<T>)'");
            return false;
        }
        
        if (!hasGenericParams && hasGenericArgs) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, funcArg,
                                  "#scope_exit callback '", ctx.pool.lookup(funcDecl->name),
                                  "' is not generic but generic arguments were provided");
            return false;
        }
    }

    // ─── 7. The function must have exactly one parameter group ─────────────
    if (func->isCurried()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must have exactly one parameter group "
                              "(curried functions are not allowed)");
        ctx.diagnostics.note(funcArg,
                             "Use a wrapper closure: '#scope_exit(() -> () { setup(5)() })' "
                             "to call a curried function");
        return false;
    }

    // ─── 8. The function must return void ──────────────────────────────────
    if (func->returnType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, funcArg,
                              "#scope_exit callback must return void "
                              "(cannot return a value during unwinding)");
        ctx.diagnostics.note(funcArg,
                             "The callback is called during unwinding - there is no "
                             "caller to receive a return value or handle an error");
        return false;
    }

    // ─── 9. No variadic parameters ─────────────────────────────────────────
    for (ParamAST* param : func->params) {
        if (param->isVariadic) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "#scope_exit callback cannot have variadic parameters");
            ctx.diagnostics.note(param,
                                 "Variadic parameters are not supported in cleanup callbacks");
            return false;
        }
    }

    // ─── 10. Validate argument count against function parameters ──────────
    size_t callbackArgs = expr->args.size() - 1;
    size_t paramCount = func->params.size();

    if (callbackArgs != paramCount) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#scope_exit callback expects ", paramCount,
                              " argument(s), got ", callbackArgs);
        ctx.diagnostics.note(expr,
                             "All parameters of the callback must be supplied at the #scope_exit call site");
        return false;
    }

    // ─── 11. Validate each argument type against callback parameters ──────
    auto argsBuilder = ctx.arena.makeBuilder<ExprAST*>();
    
    for (size_t i = 0; i < callbackArgs; ++i) {
        ExprAST* arg = expr->args[i + 1];
        TypeAST* expectedType = func->params[i]->type;

        TypeAST* argType = resolveExprWithTarget(
            arg, expectedType, ctx
        );
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

        // Store the resolved argument
        argsBuilder.push_back(arg);
    }

    // ─── 12. Check for field access capture issues ─────────────────────────
    if (funcArg->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* field = funcArg->as<FieldAccessExprAST>();
        ctx.diagnostics.warning(DiagCode::Warn_UnsafeFFI, funcArg,
                                "function reference from struct field '",
                                ctx.pool.lookup(field->fieldName),
                                "' may capture the struct's lifetime");
        ctx.diagnostics.note(funcArg,
                             "Ensure the struct outlives the scope where #scope_exit is registered");
    }

    // ─── 13. Get the current block for registration ──────────────────────
    BlockStmtAST* currentBlock = ctx.stack.currentBlock();
    if (!currentBlock) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, expr,
                              "#scope_exit must appear inside a block");
        return false;
    }

    // ─── 14. Create the registration ──────────────────────────────────────
    ScopeExitRegistration* registration = ctx.arena.make<ScopeExitRegistration>();
    registration->callExpr = expr;
    registration->callback = funcDecl;
    registration->args = argsBuilder.build();

    // ─── 15. Append to the current block's scopeExits ────────────────────
    auto exitsBuilder = ctx.arena.makeBuilder<ScopeExitRegistrationPtr>();
    
    // Copy existing registrations (preserving registration order)
    for (ScopeExitRegistrationPtr existing : currentBlock->scopeExits) {
        exitsBuilder.push_back(existing);
    }
    // Add the new one at the end
    exitsBuilder.push_back(registration);
    
    currentBlock->scopeExits = exitsBuilder.build();

    Trace::info("validateScopeExit: registered #scope_exit in block with ",
             currentBlock->scopeExits.size(), " total registrations");

    return true;
}

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

    // ─── Special cases ─────────────────────────────────────────────────────
    switch (info->kind) {
        case IntrinsicKind::AtomicStore:
            // atomic_store(ptr, val, ordering) - val can be any type
            // Ordering is validated below
            break;

        case IntrinsicKind::AtomicCas:
            // atomic_cas(ptr, expected, desired, ordering)
            if (expr->args.size() >= 3) {
                // expected and desired are validated by type system
                // They must match the pointee type
            }
            break;

        default:
            // atomic_load, atomic_add, atomic_sub, atomic_and, atomic_or, atomic_xor
            // These take ptr, [val], ordering
            break;
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

bool validateSIMD(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;
    
    std::string_view name = lookupStringView(expr->intrinsicName);
    
    // ─── #simd_splat(type, lanes, scalar) ──────────────────────────────────
    //   #simd_splat(float, 4, 3.14)
    //   #simd_splat(int, 8, 0)
    //
    //   where:
    //     type is a numeric primitive type (int8, int16, ..., float64)
    //     lanes is an integer literal (1, 2, 4, 8, 16, 32, 64)
    //     scalar is a value of the type specified by 'type'
    //
    if (name == "simd_splat") {
        if (expr->args.size() != 3) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "#simd_splat expects 3 arguments: (type, lanes, scalar)");
            return false;
        }
        
        // ─── 1. Validate first argument is a TYPE ────────────────────────────
        ExprAST* typeArg = expr->args[0];
        TypeAST* elementType = nullptr;
        
        if (typeArg->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = typeArg->as<IdentifierExprAST>();
            if (id->isType) {
                elementType = id->resolvedTypeNode;
            } else {
                // Try to resolve as a type (fallback)
                if (isPrimitiveTypeName(id->name, ctx.pool)) {
                    PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                    elementType = ctx.arena.make<PrimitiveTypeAST>(kind);
                    id->isType = true;
                    id->resolvedTypeNode = elementType;
                } else {
                    TypeDeclAST* typeDecl = ctx.lookupType(id->name);
                    if (typeDecl) {
                        NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                        namedType->resolvedDecl = typeDecl;
                        elementType = namedType;
                        id->isType = true;
                        id->resolvedTypeNode = elementType;
                    }
                }
            }
        }
        
        if (!elementType) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, typeArg,
                                  "#simd_splat: first argument must be a type "
                                  "(numeric primitive like int32, float64, etc.)");
            return false;
        }
        
        // ─── 2. Validate element type is a valid SIMD element type ───────────
        if (!isValidSimdElementType(elementType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, typeArg,
                                  "#simd_splat: type must be a numeric primitive "
                                  "(int8, int16, int32, int64, uint8, uint16, uint32, "
                                  "uint64, float32, or float64)");
            ctx.diagnostics.note(typeArg,
                                 "Got: ", typeToString(elementType, ctx.pool));
            return false;
        }
        
        // ─── 3. Validate second argument is an integer literal (lanes) ──────
        ExprAST* lanesArg = expr->args[1];
        if (!lanesArg->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, lanesArg,
                                  "#simd_splat: lanes must be an integer literal");
            return false;
        }
        
        LiteralExprAST* lanesLit = lanesArg->as<LiteralExprAST>();
        if (lanesLit->kind != LiteralKind::Int && 
            lanesLit->kind != LiteralKind::Hex &&
            lanesLit->kind != LiteralKind::Binary) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, lanesArg,
                                  "#simd_splat: lanes must be an integer literal");
            return false;
        }
        
        int64_t laneCount = parseConstantInt(lanesArg, ctx);
        if (laneCount <= 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdLaneCount, lanesArg,
                                  "#simd_splat: lane count must be > 0");
            return false;
        }
        
        // ─── 4. Validate scalar matches the specified type ──────────────────
        ExprAST* scalar = expr->args[2];
        TypeAST* scalarType = resolveExpr(scalar, ctx);
        if (!scalarType || scalarType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, scalar,
                                  "#simd_splat: scalar argument has unknown type");
            return false;
        }
        
        // Scalar must match the element type
        if (!typesEqual(elementType, scalarType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, scalar,
                                  "#simd_splat: scalar type (", 
                                  typeToString(scalarType, ctx.pool),
                                  ") does not match specified type (",
                                  typeToString(elementType, ctx.pool), ")");
            return false;
        }
        
        // ─── 5. Resolve the return type ──────────────────────────────────────
        // Simd<T, N> where T = elementType, N = laneCount
        SimdTypeAST* simdType = ctx.arena.make<SimdTypeAST>(elementType, static_cast<uint64_t>(laneCount));
        simdType->loc = expr->loc;
        expr->resolvedType = simdType;
        
        return true;
    }
    
    // ─── #simd_load(ptr, lanes) ─────────────────────────────────────────
    // Updated: lanes is an integer literal, not an enum
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
        if (!isValidSimdElementType(ptrInner->inner)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdElementType, ptr,
                                  "#simd_load: pointer must point to a numeric primitive");
            return false;
        }
        
        // Validate lanes (second argument) - must be integer literal
        ExprAST* lanesArg = expr->args[1];
        if (!lanesArg->isa<LiteralExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, lanesArg,
                                  "#simd_load: lanes must be an integer literal");
            return false;
        }
        
        int64_t laneCount = parseConstantInt(lanesArg, ctx);
        if (laneCount <= 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSimdLaneCount, lanesArg,
                                  "#simd_load: lane count must be > 0");
            return false;
        }
        
        // Resolve the return type: Simd<T, N>
        SimdTypeAST* simdType = ctx.arena.make<SimdTypeAST>(
            ptrInner->inner, 
            static_cast<uint64_t>(laneCount)
        );
        simdType->loc = expr->loc;
        expr->resolvedType = simdType;
        
        return true;
    }
    
    // ─── #simd_store(ptr, simd_value) ────────────────────────────────────
    // No changes needed - already validates correctly
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
    
    // ─── Binary SIMD ops: #simd_add, #simd_sub, #simd_mul, #simd_div ────
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
    // No changes needed - already validates correctly
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
    // No changes needed - already validates correctly
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
        
        LiteralExprAST* lit = idx->as<LiteralExprAST>();
        if (lit->kind != LiteralKind::Int) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "#simd_extract: index must be an integer literal");
            return false;
        }
        
        int64_t index = parseConstantInt(idx, ctx);
        if (index < 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                                  "#simd_extract: index must be >= 0");
            return false;
        }
        
        // Validate index < lane count (requires const eval of lane count)
        // This would be done in a separate const evaluation pass
        return true;
    }
    
    // ─── #simd_insert(v, index, value) ────────────────────────────────────
    // No changes needed - already validates correctly
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
    
    // Unknown SIMD intrinsic
    ctx.diagnostics.error(DiagCode::Sem_UnknownIntrinsic, expr,
                          "unknown SIMD intrinsic '#", name, "'");
    return false;
}

bool validateMemoryManagement(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (!info) return false;

    switch (info->kind) {
        case IntrinsicKind::Alloc: {
            // #alloc(T, count) - T is a type, count is a value
            // Grammar: #alloc(T, count) -> *T
            
            // ─── 1. Validate first argument is a TYPE ────────────────────────
            if (expr->args.empty()) {
                ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                      "#alloc expects 2 arguments: (type, count)");
                return false;
            }
            
            ExprAST* typeArg = expr->args[0];
            TypeAST* elementType = nullptr;
            
            if (typeArg->isa<IdentifierExprAST>()) {
                IdentifierExprAST* id = typeArg->as<IdentifierExprAST>();
                if (id->isType) {
                    elementType = id->resolvedTypeNode;
                } else {
                    // Try to resolve as a type (fallback)
                    TypeDeclAST* typeDecl = ctx.lookupType(id->name);
                    if (typeDecl) {
                        NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                        namedType->resolvedDecl = typeDecl;
                        namedType->genericArgs = id->genericArgs;
                        elementType = namedType;
                        id->isType = true;
                        id->resolvedTypeNode = elementType;
                        id->resolvedType = elementType;
                    } else if (isPrimitiveTypeName(id->name, ctx.pool)) {
                        PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                        elementType = ctx.arena.make<PrimitiveTypeAST>(kind);
                        id->isType = true;
                        id->resolvedTypeNode = elementType;
                        id->resolvedType = elementType;
                    }
                }
            }
            
            if (!elementType) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, typeArg,
                                      "#alloc expects a type as the first argument");
                ctx.diagnostics.note(typeArg,
                                     "Usage: #alloc(T, count) where T is a type");
                return false;
            }
            
            // ─── 2. Validate count argument ──────────────────────────────────
            if (expr->args.size() < 2) {
                ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                      "#alloc expects 2 arguments: (type, count)");
                return false;
            }
            
            if (!validateIntArg(expr->args[1], "count", ctx)) {
                return false;
            }
            
            // ─── 3. Resolve the return type: *T ──────────────────────────────
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

/// @brief Validate #bitcast(T, x) intrinsic.
/// 
/// Grammar: #bitcast(T, x) -> T
/// where T is a type, and x is an expression that can be bitcast to T.
/// Both types must have the same size.
bool validateBitcast(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.size() != 2) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#bitcast expects 2 arguments: (type, value)");
        return false;
    }
    
    // ─── 1. Validate first argument is a TYPE ──────────────────────────────
    ExprAST* typeArg = expr->args[0];
    TypeAST* targetType = nullptr;
    
    if (typeArg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = typeArg->as<IdentifierExprAST>();
        if (id->isType) {
            targetType = id->resolvedTypeNode;
        } else {
            // Try to resolve as a type (fallback)
            TypeDeclAST* typeDecl = ctx.lookupType(id->name);
            if (typeDecl) {
                NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                namedType->resolvedDecl = typeDecl;
                namedType->genericArgs = id->genericArgs;
                targetType = namedType;
                id->isType = true;
                id->resolvedTypeNode = targetType;
                id->resolvedType = targetType;
            } else if (isPrimitiveTypeName(id->name, ctx.pool)) {
                PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                targetType = ctx.arena.make<PrimitiveTypeAST>(kind);
                id->isType = true;
                id->resolvedTypeNode = targetType;
                id->resolvedType = targetType;
            }
        }
    }
    
    if (!targetType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, typeArg,
                              "#bitcast expects a type as the first argument");
        ctx.diagnostics.note(typeArg,
                             "Usage: #bitcast(T, x) where T is a type");
        return false;
    }
    
    // ─── 2. Validate the value argument ─────────────────────────────────────
    ExprAST* valueArg = expr->args[1];
    TypeAST* valueType = resolveExpr(valueArg, ctx);
    if (!valueType || valueType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, valueArg,
                              "#bitcast: value argument has unknown type");
        return false;
    }
    
    // ─── 3. Both types must be sized ────────────────────────────────────────
    // We can't check sizes at compile time without CodeGen info,
    // but Sema can check that both types are valid.
    
    // ─── 4. Resolve the return type ─────────────────────────────────────────
    expr->resolvedType = targetType;
    expr->valueState = ValueState::Definite;
    
    return true;
}

bool validateTostr(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;
    
    // ─── 1. Must have exactly one argument ─────────────────────────────────
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
    
    // ─── 2. Function types are ALLOWED ────────────────────────────────────
    // #tostr on functions returns the function's declared name
    if (argType->isa<FuncTypeAST>()) {
        // No validation needed - functions are always valid
        return true;
    }
    
    // ─── 3. Reject generic parameters ──────────────────────────────────────
    if (isGenericParameterType(argType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                              "#tostr cannot be used with generic type '", 
                              ctx.pool.lookup(argType->as<NamedTypeAST>()->name), 
                              "' - the concrete type is not known at compile time");
        ctx.diagnostics.note(expr,
                             "Only concrete types can be converted to strings");
        return false;
    }
    
    // ─── 4. Reject types containing generic parameters ─────────────────────
    if (containsGenericParameter(argType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                              "#tostr cannot be used with type '", 
                              typeToString(argType, ctx.pool),
                              "' which contains generic parameters");
        ctx.diagnostics.note(expr,
                             "Only fully concrete types can be converted to strings");
        return false;
    }
    
    // ─── 5. Reject trait types ─────────────────────────────────────────────
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
    
    // ─── 6. All checks passed ──────────────────────────────────────────────
    return true;
}

/// @brief Validate #sizeof(T) intrinsic.
bool validateSizeof(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#sizeof expects 1 argument");
        return false;
    }
    
    ExprAST* arg = expr->args[0];
    
    // ─── Check if the argument is a type reference ──────────────────────
    TypeAST* type = nullptr;
    
    if (arg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = arg->as<IdentifierExprAST>();
        if (id->isType) {
            // It was parsed as a type - use the resolved type node
            type = id->resolvedTypeNode;
        } else {
            // It was parsed as a value - try to resolve as a type
            // This handles the fallback case where the parser couldn't tell
            TypeDeclAST* typeDecl = ctx.lookupType(id->name);
            if (typeDecl) {
                // It's a user-defined type!
                NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                namedType->resolvedDecl = typeDecl;
                namedType->genericArgs = id->genericArgs;
                type = namedType;
            } else if (isPrimitiveTypeName(id->name, ctx.pool)) {
                // It's a primitive type
                PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                type = ctx.arena.make<PrimitiveTypeAST>(kind);
            }
            
            if (type) {
                // Update the identifier to mark it as a type
                id->isType = true;
                id->resolvedTypeNode = type;
                id->resolvedType = type;
            }
        }
    }
    
    if (!type) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "#sizeof expects a type, got an expression");
        return false;
    }
    
    // ─── Type must be concrete (no generic parameters) ──────────────────
    if (containsGenericParameter(type, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, arg,
                              "#sizeof cannot be used with generic type '",
                              typeToString(type, ctx.pool), "'");
        ctx.diagnostics.note(arg,
                             "Only concrete types have a known size at compile time");
        return false;
    }
    
    // ─── Type must be sized ──────────────────────────────────────────────
    // All types are sized in Lucid, except maybe future/thread which are handles
    // This is a placeholder for future validation
    
    return true;
}

/// @brief Validate #alignof(T) intrinsic.
bool validateAlignof(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (expr->args.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                              "#alignof expects 1 argument");
        return false;
    }
    
    ExprAST* arg = expr->args[0];
    
    // ─── Same logic as sizeof ──────────────────────────────────────────────
    TypeAST* type = nullptr;
    
    if (arg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = arg->as<IdentifierExprAST>();
        if (id->isType) {
            type = id->resolvedTypeNode;
        } else {
            TypeDeclAST* typeDecl = ctx.lookupType(id->name);
            if (typeDecl) {
                NamedTypeAST* namedType = ctx.arena.make<NamedTypeAST>(id->name);
                namedType->resolvedDecl = typeDecl;
                namedType->genericArgs = id->genericArgs;
                type = namedType;
            } else if (isPrimitiveTypeName(id->name, ctx.pool)) {
                PrimitiveKind kind = primitiveKindFromName(id->name, ctx.pool);
                type = ctx.arena.make<PrimitiveTypeAST>(kind);
            }
            
            if (type) {
                id->isType = true;
                id->resolvedTypeNode = type;
                id->resolvedType = type;
            }
        }
    }
    
    if (!type) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "#alignof expects a type, got an expression");
        return false;
    }
    
    if (containsGenericParameter(type, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, arg,
                              "#alignof cannot be used with generic type '",
                              typeToString(type, ctx.pool), "'");
        return false;
    }
    
    return true;
}

} // namespace sema