/// @file IntrinsicEmitter.cpp
/// @brief Implementation of intrinsic emission to LLVM IR.

#include "IntrinsicEmitter.hpp"
#include "../support/CodeGenHelpers.hpp"
#include "../CodeGenType.hpp"
#include "../CodeGen.hpp"

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/DerivedTypes.h>

#include <unordered_set>
#include <unordered_map>
#include <cmath>

namespace codegen {

// ─── Forward Declarations ──────────────────────────────────────────────────

static llvm::Value* emitMathIntrinsic(
    const std::string& name,
    std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitBitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitAtomicIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitSIMDIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

static llvm::Value* emitControlIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

// ─── Helper: Get String Type ──────────────────────────────────────────────

static llvm::Type* getStringType(CodeGenContext& ctx) {
    return llvm::PointerType::get(ctx.llvmCtx, 0);
}

// ─── Helper: Get or Insert Function ──────────────────────────────────────

static llvm::Function* getOrInsertFunction(
    llvm::Module* module,
    const std::string& name,
    llvm::FunctionType* type
) {
    llvm::FunctionCallee callee = module->getOrInsertFunction(name, type);
    return llvm::dyn_cast<llvm::Function>(callee.getCallee());
}

// ─── Helper: Get element type for pointer arithmetic ─────────────────────

static llvm::Type* getPointeeTypeFromIntrinsic(
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return llvm::Type::getInt8Ty(ctx.llvmCtx);

    // For ptrOffset<T>(ptr, n), get the pointee type from the first argument
    if (!expr->args.empty()) {
        ExprAST* arg = expr->args[0];
        if (arg && arg->resolvedType) {
            if (arg->resolvedType->isa<PtrTypeAST>()) {
                const PtrTypeAST* ptrType = arg->resolvedType->as<PtrTypeAST>();
                if (ptrType->inner) {
                    return getType(ctx, ptrType->inner);
                }
            }
        }
    }

    return llvm::Type::getInt8Ty(ctx.llvmCtx);
}

// ─── Helper: Get or Create Runtime Function ──────────────────────────────

static llvm::Function* getOrCreateRuntimeFunction(
    const std::string& name,
    llvm::FunctionType* type,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getRuntimeFunction(name);
    if (func) return func;

    func = llvm::Function::Create(
        type,
        llvm::Function::ExternalLinkage,
        name,
        ctx.module
    );
    ctx.setRuntimeFunction(name, func);
    return func;
}

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Value* emitIntrinsicFromAST(
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return nullptr;

    std::string name = ctx.pool.lookup(expr->intrinsicName);
    SourceLocation loc = expr->loc;

    // ─── Special-case intrinsics that need raw addresses ────────────────
    // These intrinsics should NOT load their arguments.
    if (name == "addrof") {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#addrof' requires an argument");
            return nullptr;
        }

        // addrof(x) returns the address of x - do NOT load
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;
        // argVal should already be a pointer (l-value)
        return argVal;
    }

    if (name == "toRef") {
        if (expr->args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toRef' requires an argument");
            return nullptr;
        }

        // toRef(ptr) - do NOT load, but add null check
        llvm::Value* argVal = lowerExpression(expr->args[0], ctx);
        if (!argVal) return nullptr;

        // Add null check assertion
        return emitNullCheck(argVal, "toRef called with null pointer", ctx);
    }

    // ─── Normal path: load lvalues ────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) return nullptr;

        if (arg->isLValue) {
            // Load with explicit element type
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            if (!elemType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, arg->loc,
                                       "cannot determine type of argument for '#", name, "'");
                return nullptr;
            }
            argVal = loadIfNeeded(argVal, elemType, ctx);
            if (!argVal) return nullptr;
        }
        args.push_back(argVal);
    }

    return emitIntrinsic(name, args, expr, ctx);
}

llvm::Value* emitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── Category Dispatch ──────────────────────────────────────────────

    // Floating-Point Math
    static const std::unordered_set<std::string> MATH_INTRINSICS = {
        "sqrt", "abs", "fma", "ceil", "floor", "round", "pow", "min", "max"
    };
    if (MATH_INTRINSICS.find(name) != MATH_INTRINSICS.end()) {
        std::vector<llvm::Value*> mutableArgs = args;
        return emitMathIntrinsic(name, mutableArgs, expr, ctx);
    }

    // Memory Operations
    static const std::unordered_set<std::string> MEMORY_INTRINSICS = {
        "memcpy", "memmove", "memset"
    };
    if (MEMORY_INTRINSICS.find(name) != MEMORY_INTRINSICS.end()) {
        return emitMemoryIntrinsic(name, args, expr, ctx);
    }

    // Bit Manipulation
    static const std::unordered_set<std::string> BIT_INTRINSICS = {
        "clz", "ctz", "popcount", "bswap"
    };
    if (BIT_INTRINSICS.find(name) != BIT_INTRINSICS.end()) {
        return emitBitIntrinsic(name, args, expr, ctx);
    }

    // Atomics
    if (name.find("atomic_") == 0) {
        return emitAtomicIntrinsic(name, args, expr, ctx);
    }

    // SIMD
    if (name.find("simd_") == 0) {
        return emitSIMDIntrinsic(name, args, expr, ctx);
    }

    // String Operations
    static const std::unordered_set<std::string> STRING_INTRINSICS = {
        "str_len", "str_ptr", "str_from_ptr", "str_concat",
        "str_slice", "str_eq", "str_byte_at"
    };
    if (STRING_INTRINSICS.find(name) != STRING_INTRINSICS.end()) {
        return emitStringIntrinsic(name, args, expr, ctx);
    }

    // Pointer Operations (excluding addrof which is special-cased above)
    static const std::unordered_set<std::string> POINTER_INTRINSICS = {
        "toPtr", "ptrOffset", "ptrDiff"
    };
    if (POINTER_INTRINSICS.find(name) != POINTER_INTRINSICS.end()) {
        return emitPointerIntrinsic(name, args, expr, ctx);
    }

    // Memory Management
    static const std::unordered_set<std::string> MEMORY_MGMT_INTRINSICS = {
        "alloc", "free", "arena_create", "arena_alloc",
        "arena_reset", "arena_free"
    };
    if (MEMORY_MGMT_INTRINSICS.find(name) != MEMORY_MGMT_INTRINSICS.end()) {
        return emitMemoryMgmtIntrinsic(name, args, expr, ctx);
    }

    // Type Inspection
    static const std::unordered_set<std::string> TYPE_INTRINSICS = {
        "sizeof", "alignof", "typeof", "nameof", "tostr", "ptrstr", "bitcast"
    };
    if (TYPE_INTRINSICS.find(name) != TYPE_INTRINSICS.end()) {
        return emitTypeIntrinsic(name, args, expr, ctx);
    }

    // Control Flow
    static const std::unordered_set<std::string> CONTROL_INTRINSICS = {
        "scope_exit", "likely", "unlikely", "prefetch", "prefetch_r",
        "prefetch_w", "fence", "pause"
    };
    if (CONTROL_INTRINSICS.find(name) != CONTROL_INTRINSICS.end()) {
        return emitControlIntrinsic(name, args, expr, ctx);
    }

    // Unknown intrinsic
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown intrinsic '#", name, "'");
    return nullptr;
}

// ─── Helper: Get LLVM Intrinsic Declaration ──────────────────────────────

llvm::Function* getLLVMIntrinsicDecl(
    llvm::Intrinsic::ID id,
    llvm::ArrayRef<llvm::Type*> argTypes,
    CodeGenContext& ctx
) {
    return llvm::Intrinsic::getDeclaration(ctx.module, id, argTypes);
}

// ─── Math Intrinsics ──────────────────────────────────────────────────────

static llvm::Value* emitMathIntrinsic(
    const std::string& name,
    std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    if (name == "min" || name == "max") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* a = args[0];
        llvm::Value* b = args[1];

        if (a->getType() != b->getType()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "arguments to '#", name, "' must have the same type");
            return nullptr;
        }

        if (a->getType()->isIntegerTy()) {
            llvm::CmpInst::Predicate pred = (name == "min")
                ? llvm::CmpInst::ICMP_SLT
                : llvm::CmpInst::ICMP_SGT;
            llvm::Value* cmp = ctx.builder.CreateICmp(pred, a, b);
            return ctx.builder.CreateSelect(cmp, a, b);
        } else if (a->getType()->isFloatingPointTy()) {
            llvm::CmpInst::Predicate pred = (name == "min")
                ? llvm::CmpInst::FCMP_OLT
                : llvm::CmpInst::FCMP_OGT;
            llvm::Value* cmp = ctx.builder.CreateFCmp(pred, a, b);
            return ctx.builder.CreateSelect(cmp, a, b);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#", name, "' requires numeric arguments");
            return nullptr;
        }
    }

    if (name == "pow") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#pow' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* a = args[0];
        llvm::Value* b = args[1];

        // ─── Check if both are integers - promote to double ──────────────
        if (a->getType()->isIntegerTy() && b->getType()->isIntegerTy()) {
            a = ctx.builder.CreateSIToFP(a, llvm::Type::getDoubleTy(ctx.llvmCtx));
            b = ctx.builder.CreateSIToFP(b, llvm::Type::getDoubleTy(ctx.llvmCtx));
        }

        // ─── Use the actual type of the first argument ────────────────────
        llvm::Type* resultType = a->getType();
        if (!resultType->isFloatingPointTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#pow' requires floating-point or integer arguments");
            return nullptr;
        }

        // ─── Get or create the pow function for this type ─────────────────
        std::string powName = (resultType->isDoubleTy()) ? "pow" : "powf";
        llvm::FunctionType* powType = llvm::FunctionType::get(
            resultType,
            {resultType, resultType},
            false
        );
        llvm::Function* powFunc = getOrInsertFunction(ctx.module, powName, powType);
        if (!powFunc) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                                   "could not find '#pow' function for type");
            return nullptr;
        }

        return ctx.builder.CreateCall(powFunc, {a, b});
    }

    // ─── Single-argument math intrinsics ──────────────────────────────────
    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "sqrt") id = llvm::Intrinsic::sqrt;
    else if (name == "abs") id = llvm::Intrinsic::fabs;
    else if (name == "fma") id = llvm::Intrinsic::fma;
    else if (name == "ceil") id = llvm::Intrinsic::ceil;
    else if (name == "floor") id = llvm::Intrinsic::floor;
    else if (name == "round") id = llvm::Intrinsic::round;

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown math intrinsic '#", name, "'");
        return nullptr;
    }

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    for (llvm::Value* arg : args) {
        argTypes.push_back(arg->getType());
    }

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Memory Intrinsics ────────────────────────────────────────────────────

static llvm::Value* emitMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "memcpy") id = llvm::Intrinsic::memcpy;
    else if (name == "memmove") id = llvm::Intrinsic::memmove;
    else if (name == "memset") id = llvm::Intrinsic::memset;

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown memory intrinsic '#", name, "'");
        return nullptr;
    }

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    for (llvm::Value* arg : args) {
        argTypes.push_back(arg->getType());
    }

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Bit Intrinsics ───────────────────────────────────────────────────────

static llvm::Value* emitBitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    if (args.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                               "intrinsic '#", name, "' requires an argument");
        return nullptr;
    }

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "clz") id = llvm::Intrinsic::ctlz;
    else if (name == "ctz") id = llvm::Intrinsic::cttz;
    else if (name == "popcount") id = llvm::Intrinsic::ctpop;
    else if (name == "bswap") id = llvm::Intrinsic::bswap;

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown bit intrinsic '#", name, "'");
        return nullptr;
    }

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    argTypes.push_back(args[0]->getType());

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    // clz/ctz need a second argument (is_zero_undef)
    if (name == "clz" || name == "ctz") {
        std::vector<llvm::Value*> callArgs = args;
        callArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0));
        return ctx.builder.CreateCall(intrinsic, callArgs);
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Atomic Intrinsics ────────────────────────────────────────────────────

static llvm::Value* emitAtomicIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // TODO: Implement atomic operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, loc,
                              "atomic intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── SIMD Intrinsics ──────────────────────────────────────────────────────

static llvm::Value* emitSIMDIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // TODO: Implement SIMD operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, loc,
                              "SIMD intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── String Intrinsics ────────────────────────────────────────────────────

static llvm::Value* emitStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // TODO: Implement string operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, loc,
                              "string intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── Pointer Intrinsics ──────────────────────────────────────────────────

static llvm::Value* emitPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    if (name == "toPtr") {
        // toPtr(ref) -> convert reference to pointer
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toPtr' requires an argument");
            return nullptr;
        }
        return args[0];
    }

    if (name == "ptrOffset") {
        // ptrOffset(ptr, n) -> element-scaled pointer arithmetic
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrOffset' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* offset = args[1];

        // ─── Get the element type ──────────────────────────────────────────
        // For ptrOffset<T>(ptr, n), T is the element type
        llvm::Type* elemType = getPointeeTypeFromIntrinsic(expr, ctx);

        // ─── Use InBounds GEP for element-scaled arithmetic ──────────────
        // CreateInBoundsGEP scales the offset by the element size automatically
        llvm::Value* gep = ctx.builder.CreateInBoundsGEP(
            elemType,
            ptr,
            offset,
            "ptr_offset"
        );

        return gep;
    }

    if (name == "ptrDiff") {
        // ptrDiff(p1, p2) -> distance in elements
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrDiff' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* p1 = ctx.builder.CreatePtrToInt(
            args[0],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );
        llvm::Value* p2 = ctx.builder.CreatePtrToInt(
            args[1],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );

        llvm::Value* diffBytes = ctx.builder.CreateSub(p1, p2, "ptr_diff_bytes");

        // ─── Divide by element size to get element count ──────────────────
        llvm::Type* elemType = getPointeeTypeFromIntrinsic(expr, ctx);
        uint64_t elemSize = ctx.module->getDataLayout().getTypeAllocSize(elemType);

        if (elemSize > 1) {
            llvm::Value* elemSizeVal = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                elemSize
            );
            return ctx.builder.CreateSDiv(diffBytes, elemSizeVal, "ptr_diff_elements");
        }

        return diffBytes;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown pointer intrinsic '#", name, "'");
    return nullptr;
}

// ─── Memory Management Intrinsics ────────────────────────────────────────

static llvm::Value* emitMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── #alloc(type, count) -> *T ──────────────────────────────────────
    if (name == "alloc") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#alloc' requires 2 arguments (type, count)");
            return nullptr;
        }

        // Get the runtime alloc function
        llvm::FunctionType* allocType = llvm::FunctionType::get(
            llvm::PointerType::get(ctx.llvmCtx, 0),
            {llvm::Type::getInt64Ty(ctx.llvmCtx)},
            false
        );
        llvm::Function* allocFunc = getOrCreateRuntimeFunction("__lucid_alloc", allocType, ctx);

        // Calculate size: count * sizeof(type)
        // args[0] is the type (not a value), args[1] is the count
        // For now, we use count as bytes
        // TODO: Get element type from the intrinsic's type arguments
        llvm::Value* size = args[1];  // count
        llvm::Value* result = ctx.builder.CreateCall(allocFunc, {size});

        // Cast to the appropriate pointer type
        // TODO: Cast to *T based on type argument
        return result;
    }

    // ─── #free(ptr) ──────────────────────────────────────────────────────
    if (name == "free") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#free' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* freeType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        llvm::Function* freeFunc = getOrCreateRuntimeFunction("__lucid_free", freeType, ctx);

        ctx.builder.CreateCall(freeFunc, {args[0]});
        return nullptr;
    }

    // ─── #arena_create(size) -> ArenaDescriptor ─────────────────────────
    if (name == "arena_create") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_create' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* arenaCreateType = llvm::FunctionType::get(
            llvm::PointerType::get(ctx.llvmCtx, 0),  // ArenaDescriptor*
            {llvm::Type::getInt64Ty(ctx.llvmCtx)},
            false
        );
        llvm::Function* arenaCreateFunc = getOrCreateRuntimeFunction(
            "__lucid_arena_create", arenaCreateType, ctx
        );

        return ctx.builder.CreateCall(arenaCreateFunc, {args[0]});
    }

    // ─── #arena_alloc(arena, type, count) -> *T ─────────────────────────
    if (name == "arena_alloc") {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_alloc' requires 3 arguments");
            return nullptr;
        }

        llvm::FunctionType* arenaAllocType = llvm::FunctionType::get(
            llvm::PointerType::get(ctx.llvmCtx, 0),
            {
                llvm::PointerType::get(ctx.llvmCtx, 0),  // ArenaDescriptor*
                llvm::Type::getInt64Ty(ctx.llvmCtx)      // size
            },
            false
        );
        llvm::Function* arenaAllocFunc = getOrCreateRuntimeFunction(
            "__lucid_arena_alloc", arenaAllocType, ctx
        );

        // args[0] = arena, args[1] = type (not used), args[2] = count
        llvm::Value* result = ctx.builder.CreateCall(arenaAllocFunc, {args[0], args[2]});
        return result;
    }

    // ─── #arena_reset(arena) ─────────────────────────────────────────────
    if (name == "arena_reset") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_reset' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* arenaResetType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        llvm::Function* arenaResetFunc = getOrCreateRuntimeFunction(
            "__lucid_arena_reset", arenaResetType, ctx
        );

        ctx.builder.CreateCall(arenaResetFunc, {args[0]});
        return nullptr;
    }

    // ─── #arena_free(arena) ──────────────────────────────────────────────
    if (name == "arena_free") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_free' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* arenaFreeType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        llvm::Function* arenaFreeFunc = getOrCreateRuntimeFunction(
            "__lucid_arena_free", arenaFreeType, ctx
        );

        ctx.builder.CreateCall(arenaFreeFunc, {args[0]});
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown memory management intrinsic '#", name, "'");
    return nullptr;
}

// ─── Type Inspection Intrinsics ──────────────────────────────────────────

static llvm::Value* emitTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── #sizeof(T) ──────────────────────────────────────────────────────
    if (name == "sizeof") {
        // sizeof(T) - T is passed as a type argument, not a value
        // We need to get the type from the intrinsic's type arguments
        // For now, this is a placeholder
        // TODO: Get type from expr->typeArgs
        if (expr && !expr->args.empty()) {
            const TypeAST* targetType = expr->args[0]->resolvedType;
            if (targetType) {
                llvm::Type* llvmType = getType(ctx, targetType);
                if (llvmType) {
                    uint64_t size = ctx.module->getDataLayout().getTypeAllocSize(llvmType);
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(ctx.llvmCtx),
                        size
                    );
                }
            }
        }
        // Fallback: return 0
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
    }

    // ─── #alignof(T) ──────────────────────────────────────────────────────
    if (name == "alignof") {
        if (expr && !expr->args.empty()) {
            const TypeAST* targetType = expr->args[0]->resolvedType;
            if (targetType) {
                llvm::Type* llvmType = getType(ctx, targetType);
                if (llvmType) {
                    uint64_t alignment = ctx.module->getDataLayout().getABITypeAlign(llvmType).value();
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(ctx.llvmCtx),
                        alignment
                    );
                }
            }
        }
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
    }

    // ─── #bitcast(T, x) ──────────────────────────────────────────────────
    if (name == "bitcast") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#bitcast' requires an argument");
            return nullptr;
        }
        // TODO: Actually bitcast to the target type
        // The target type is the first type argument
        return args[0];
    }

    // ─── #typeof(x), #nameof(x), #tostr(x), #ptrstr(x) ──────────────────
    if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr") {
        // Type inspection string intrinsics
        // For now, return an empty string
        // TODO: Implement actual string return
        llvm::Type* strType = getStringType(ctx);
        return llvm::Constant::getNullValue(strType);
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown type intrinsic '#", name, "'");
    return nullptr;
}

// ─── Control Flow Intrinsics ─────────────────────────────────────────────

static llvm::Value* emitControlIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── scope_exit ──────────────────────────────────────────────────────
    if (name == "scope_exit") {
        // scope_exit is handled in Sema and stored on BlockStmtAST.
        // CodeGenStmt.cpp emits these callbacks.
        // No runtime code is generated at the call site.
        return nullptr;
    }

    // ─── likely / unlikely ──────────────────────────────────────────────
    if (name == "likely" || name == "unlikely") {
        // Branch prediction hints - just return the condition value
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires an argument");
            return nullptr;
        }
        // TODO: Add branch weight metadata
        return args[0];
    }

    // ─── prefetch ────────────────────────────────────────────────────────
    if (name == "prefetch" || name == "prefetch_r" || name == "prefetch_w") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires an argument");
            return nullptr;
        }

        llvm::Value* ptr = args[0];

        int rw = (name == "prefetch_w") ? 1 : 0;
        int locality = 3;
        int cacheType = 0;

        llvm::Function* prefetch = llvm::Intrinsic::getDeclaration(
            ctx.module,
            llvm::Intrinsic::prefetch,
            {ptr->getType()}
        );

        if (!prefetch) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                                   "could not get LLVM prefetch intrinsic");
            return nullptr;
        }

        std::vector<llvm::Value*> prefetchArgs = {
            ptr,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), rw),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), locality),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), cacheType)
        };

        return ctx.builder.CreateCall(prefetch, prefetchArgs);
    }

    // ─── fence ──────────────────────────────────────────────────────────
    if (name == "fence") {
        // fence(ordering) -> LLVM fence instruction
        // Default to seq_cst
        // TODO: Parse ordering argument
        llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;
        ctx.builder.CreateFence(ordering);
        return nullptr;
    }

    // ─── pause ──────────────────────────────────────────────────────────
    if (name == "pause") {
        // pause() -> LLVM x86 pause instruction
        // TODO: Implement pause intrinsic using llvm.x86.sse2.pause
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, loc,
                                  "intrinsic '#pause' not fully implemented");
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown control intrinsic '#", name, "'");
    return nullptr;
}

} // namespace codegen