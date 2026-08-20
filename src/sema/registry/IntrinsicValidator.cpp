/// @file registry/IntrinsicValidator.cpp
/// @brief Implementation of semantic validation for intrinsics.

#include "IntrinsicValidator.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include "../types/ArgTypeValidators.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/Sema.hpp"

#include <unordered_set>
#include <unordered_map>

namespace sema {

// ─── Internal Helpers ──────────────────────────────────────────────────────

static bool isArgumentCountValid(size_t count, const IntrinsicInfo* info) {
    if (info->isVarArg) {
        return count >= info->minArgs;
    }
    return count >= info->minArgs && count <= info->maxArgs;
}

static bool isIntrinsicVoidInternal(InternedString name, SemaContext& ctx) {
    std::string nameStr = ctx.pool.lookup(name);
    return VOID_INTRINSICS.find(nameStr) != VOID_INTRINSICS.end();
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
                                  "intrinsic '#", intrinsicName, "' expects ",
                                  std::to_string(info->minArgs), " argument(s), got ",
                                  std::to_string(expr->args.size()));
        }
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

bool isIntrinsicVoid(InternedString name, SemaContext& ctx) {
    return isIntrinsicVoidInternal(name, ctx);
}

// ─── getIntrinsicReturnType - FULL Implementation ─────────────────────────

TypeAST* getIntrinsicReturnType(IntrinsicCallExprAST* expr,
                                       TypeAST* targetType,
                                       SemaContext& ctx) {
    if (!expr) return targetType;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Void intrinsics return nothing ────────────────────────────────────
    if (isIntrinsicVoidInternal(expr->intrinsicName, ctx)) {
        return nullptr;
    }

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

    // ─── Pointer Operations (using type cache) ────────────────────────────
    if (name == "addrof") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            // addrof(x) returns *T where T is the type of x
            TypeAST* innerType = expr->args[0]->resolvedType;
            // Use the cached pointer type - ensures canonicalization
            return ctx.getPtrType(innerType);
        }
        return targetType;
    }

    if (name == "toRef") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<PtrTypeAST>()) {
                TypeAST* inner = argType->as<PtrTypeAST>()->inner;
                // Use the cached reference type
                return ctx.getRefType(inner);
            }
            // If it's already a reference, return it
            if (argType->isa<RefTypeAST>()) {
                return argType;
            }
        }
        return targetType;
    }

    if (name == "toPtr") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            TypeAST* argType = expr->args[0]->resolvedType;
            if (argType->isa<RefTypeAST>()) {
                TypeAST* inner = argType->as<RefTypeAST>()->inner;
                // Use the cached pointer type
                return ctx.getPtrType(inner);
            }
            // If it's already a pointer, return it
            if (argType->isa<PtrTypeAST>()) {
                return argType;
            }
        }
        return targetType;
    }

    // ─── ptrOffset(ptr, n) returns the same pointer type ──────────────────
    if (name == "ptrOffset") {
        if (!expr->args.empty() && expr->args[0]->resolvedType) {
            return expr->args[0]->resolvedType;
        }
        return targetType;
    }

    // ─── ptrDiff(p1, p2) returns int64 ─────────────────────────────────────
    if (name == "ptrDiff") {
        return ctx.getIntType();
    }

    // ─── bitcast(T, x) returns the target type T ──────────────────────────
    if (name == "bitcast") {
        // The return type is the target type T passed as the first argument
        // This is stored in expr->resolvedType by the caller
        // We just return the target type from the call
        if (expr->resolvedType) {
            return expr->resolvedType;
        }
        return targetType;
    }

    // ─── String Operations ──────────────────────────────────────────────────
    if (name == "str_len" || name == "str_from_ptr" || name == "str_concat" ||
        name == "str_slice") {
        return ctx.getStringType();
    }

    if (name == "str_ptr") {
        return ctx.getPtrType(ctx.getIntType());
    }

    if (name == "str_eq") {
        return ctx.getBoolType();
    }

    if (name == "str_byte_at") {
        return ctx.getIntType();
    }

    // ─── Memory Management ──────────────────────────────────────────────────
    if (name == "alloc" || name == "arena_alloc") {
        // Return type is *T where T is the pointee type
        // This is stored in the call's resolved type
        if (expr->resolvedType) {
            return expr->resolvedType;
        }
        return ctx.getPtrType(ctx.getIntType());
    }

    if (name == "arena_create") {
        // Return ArenaDescriptor struct type
        // For now, return a pointer to it
        // TODO: Define ArenaDescriptor struct type
        return targetType;
    }

    // ─── SIMD ────────────────────────────────────────────────────────────────
    if (name.find("simd_") == 0) {
        // simd_store returns void
        if (name == "simd_store") {
            return nullptr;
        }
        // simd_splat, simd_load, simd_add, etc. return the vector type
        // This is stored in the call's resolved type
        if (expr->resolvedType) {
            return expr->resolvedType;
        }
        return targetType;
    }

    return targetType;
}

ValueState getIntrinsicValueState(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    if (!expr) return ValueState::Unknown;

    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Void intrinsics produce no value ──────────────────────────────────
    if (isIntrinsicVoidInternal(expr->intrinsicName, ctx)) {
        return ValueState::None;
    }

    // ─── Scope Exit produces no value ─────────────────────────────────────
    if (name == "scope_exit") {
        return ValueState::None;
    }

    // ─── Memory allocations can fail ──────────────────────────────────────
    if (name == "alloc" || name == "arena_alloc") {
        return ValueState::Unknown;
    }

    // ─── toRef asserts non-null - always definite if it returns ──────────
    if (name == "toRef") {
        return ValueState::Definite;
    }

    // ─── Fence and pause always succeed ──────────────────────────────────
    if (name == "fence" || name == "pause") {
        return ValueState::Definite;
    }

    // ─── String operations that can fail ──────────────────────────────────
    if (name == "str_from_ptr" || name == "str_concat" || name == "str_slice") {
        return ValueState::Unknown;
    }

    return ValueState::Definite;
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

bool validatePointerOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "addrof") {
        // addrof can take any expression (even function names, struct fields, etc.)
        // It returns *T, which is valid
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
                              debug::typeToString(funcType, ctx.pool));
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

    LOG_SEMA("validateScopeExit: registered #scope_exit in block with ",
             currentBlock->scopeExits.size(), " total registrations");

    return true;
}

bool validateAtomicOp(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── atomic_store takes ptr, val, ordering ────────────────────────────
    if (name == "atomic_store") {
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
        // val can be any type that matches the pointer's pointee
        // Ordering is validated below
    } else {
        // atomic_load, atomic_add, atomic_sub, atomic_and, atomic_or, atomic_xor, atomic_cas
        if (expr->args.size() >= 1 && !validatePtrArg(expr->args[0], "ptr", ctx)) return false;
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
    const std::string name = ctx.pool.lookup(expr->intrinsicName);

    if (name == "simd_splat") {
        if (expr->args.size() >= 2) {
            TypeAST* nType = resolveExprWithTarget(
                expr->args[1], ctx.getIntType(), ctx
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
        // val is the second argument - any type
        return true;
    }

    return true;
}

bool validateMemoryManagement(IntrinsicCallExprAST* expr, SemaContext& ctx) {
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

    if (name == "arena_reset" || name == "arena_free") {
        if (!expr->args.empty() && !validatePtrArg(expr->args[0], "arena", ctx)) return false;
        return true;
    }

    return true;
}

} // namespace sema