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
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitBitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitAtomicIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitSIMDIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
);

static llvm::Value* emitTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
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
    // String is represented as a pointer to i8 (null-terminated)
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

// ─── Public API ────────────────────────────────────────────────────────────

llvm::Value* emitIntrinsicFromAST(
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    if (!expr) return nullptr;

    std::string name = ctx.pool.lookup(expr->intrinsicName);

    // ─── Lower arguments ──────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : expr->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return nullptr;
        }
        if (arg->isLValue) {
            argVal = loadIfNeeded(argVal, arg->isLValue, ctx);
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
    // ─── Category Dispatch ──────────────────────────────────────────────

    // Floating-Point Math
    static const std::unordered_set<std::string> MATH_INTRINSICS = {
        "sqrt", "abs", "fma", "ceil", "floor", "round", "pow", "min", "max"
    };
    if (MATH_INTRINSICS.find(name) != MATH_INTRINSICS.end()) {
        return emitMathIntrinsic(name, args, ctx);
    }

    // Memory Operations
    static const std::unordered_set<std::string> MEMORY_INTRINSICS = {
        "memcpy", "memmove", "memset"
    };
    if (MEMORY_INTRINSICS.find(name) != MEMORY_INTRINSICS.end()) {
        return emitMemoryIntrinsic(name, args, ctx);
    }

    // Bit Manipulation
    static const std::unordered_set<std::string> BIT_INTRINSICS = {
        "clz", "ctz", "popcount", "bswap"
    };
    if (BIT_INTRINSICS.find(name) != BIT_INTRINSICS.end()) {
        return emitBitIntrinsic(name, args, ctx);
    }

    // Atomics
    if (name.find("atomic_") == 0) {
        return emitAtomicIntrinsic(name, args, ctx);
    }

    // SIMD
    if (name.find("simd_") == 0) {
        return emitSIMDIntrinsic(name, args, ctx);
    }

    // String Operations
    static const std::unordered_set<std::string> STRING_INTRINSICS = {
        "str_len", "str_ptr", "str_from_ptr", "str_concat",
        "str_slice", "str_eq", "str_byte_at"
    };
    if (STRING_INTRINSICS.find(name) != STRING_INTRINSICS.end()) {
        return emitStringIntrinsic(name, args, ctx);
    }

    // Pointer Operations
    static const std::unordered_set<std::string> POINTER_INTRINSICS = {
        "toRef", "toPtr", "ptrOffset", "ptrDiff", "addrof"
    };
    if (POINTER_INTRINSICS.find(name) != POINTER_INTRINSICS.end()) {
        return emitPointerIntrinsic(name, args, ctx);
    }

    // Memory Management
    static const std::unordered_set<std::string> MEMORY_MGMT_INTRINSICS = {
        "alloc", "free", "arena_create", "arena_alloc",
        "arena_reset", "arena_free"
    };
    if (MEMORY_MGMT_INTRINSICS.find(name) != MEMORY_MGMT_INTRINSICS.end()) {
        return emitMemoryMgmtIntrinsic(name, args, ctx);
    }

    // Type Inspection
    static const std::unordered_set<std::string> TYPE_INTRINSICS = {
        "sizeof", "alignof", "typeof", "nameof", "tostr", "ptrstr", "bitcast"
    };
    if (TYPE_INTRINSICS.find(name) != TYPE_INTRINSICS.end()) {
        return emitTypeIntrinsic(name, args, ctx);
    }

    // Control Flow (includes scope_exit, likely, unlikely, prefetch, fence, pause)
    static const std::unordered_set<std::string> CONTROL_INTRINSICS = {
        "scope_exit", "likely", "unlikely", "prefetch", "prefetch_r",
        "prefetch_w", "fence", "pause"
    };
    if (CONTROL_INTRINSICS.find(name) != CONTROL_INTRINSICS.end()) {
        return emitControlIntrinsic(name, args, expr, ctx);
    }

    // Unknown intrinsic - should have been caught by Sema
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, SourceLocation(),
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
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    if (name == "min" || name == "max") {
        if (args.size() < 2) return nullptr;

        llvm::Value* a = args[0];
        llvm::Value* b = args[1];

        if (a->getType()->isIntegerTy()) {
            llvm::CmpInst::Predicate pred = (name == "min")
                ? llvm::CmpInst::ICMP_SLT
                : llvm::CmpInst::ICMP_SGT;
            llvm::Value* cmp = ctx.builder.CreateICmp(pred, a, b);
            return ctx.builder.CreateSelect(cmp, a, b);
        } else {
            llvm::CmpInst::Predicate pred = (name == "min")
                ? llvm::CmpInst::FCMP_OLT
                : llvm::CmpInst::FCMP_OGT;
            llvm::Value* cmp = ctx.builder.CreateFCmp(pred, a, b);
            return ctx.builder.CreateSelect(cmp, a, b);
        }
    }

    if (name == "pow") {
        // pow needs libm call, not an LLVM intrinsic
        llvm::FunctionType* powType = llvm::FunctionType::get(
            llvm::Type::getDoubleTy(ctx.llvmCtx),
            {llvm::Type::getDoubleTy(ctx.llvmCtx),
             llvm::Type::getDoubleTy(ctx.llvmCtx)},
            false
        );
        llvm::Function* powFunc = getOrInsertFunction(ctx.module, "pow", powType);
        if (!powFunc) return nullptr;
        return ctx.builder.CreateCall(powFunc, args);
    }

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "sqrt") id = llvm::Intrinsic::sqrt;
    else if (name == "abs") id = llvm::Intrinsic::fabs;
    else if (name == "fma") id = llvm::Intrinsic::fma;
    else if (name == "ceil") id = llvm::Intrinsic::ceil;
    else if (name == "floor") id = llvm::Intrinsic::floor;
    else if (name == "round") id = llvm::Intrinsic::round;

    if (id == llvm::Intrinsic::not_intrinsic) return nullptr;

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    for (llvm::Value* arg : args) {
        argTypes.push_back(arg->getType());
    }

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) return nullptr;

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Memory Intrinsics ────────────────────────────────────────────────────

static llvm::Value* emitMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "memcpy") id = llvm::Intrinsic::memcpy;
    else if (name == "memmove") id = llvm::Intrinsic::memmove;
    else if (name == "memset") id = llvm::Intrinsic::memset;

    if (id == llvm::Intrinsic::not_intrinsic) return nullptr;

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    for (llvm::Value* arg : args) {
        argTypes.push_back(arg->getType());
    }

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) return nullptr;

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Bit Intrinsics ───────────────────────────────────────────────────────

static llvm::Value* emitBitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    if (args.empty()) return nullptr;

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (name == "clz") id = llvm::Intrinsic::ctlz;
    else if (name == "ctz") id = llvm::Intrinsic::cttz;
    else if (name == "popcount") id = llvm::Intrinsic::ctpop;
    else if (name == "bswap") id = llvm::Intrinsic::bswap;

    if (id == llvm::Intrinsic::not_intrinsic) return nullptr;

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    argTypes.push_back(args[0]->getType());

    llvm::Function* intrinsic = getLLVMIntrinsicDecl(id, argTypes, ctx);
    if (!intrinsic) return nullptr;

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
    CodeGenContext& ctx
) {
    // TODO: Implement atomic operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                              "atomic intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── SIMD Intrinsics ──────────────────────────────────────────────────────

static llvm::Value* emitSIMDIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    // TODO: Implement SIMD operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                              "SIMD intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── String Intrinsics ────────────────────────────────────────────────────

static llvm::Value* emitStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    // TODO: Implement string operations
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                              "string intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── Pointer Intrinsics ──────────────────────────────────────────────────

static llvm::Value* emitPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    if (name == "addrof") {
        // addrof(x) -> just return the address of the l-value
        if (args.empty()) return nullptr;
        return args[0];
    }

    if (name == "toRef") {
        // toRef(ptr) -> assert non-null and convert to reference
        if (args.empty()) return nullptr;
        // TODO: Add null check assertion
        return args[0];
    }

    if (name == "toPtr") {
        // toPtr(ref) -> convert reference to pointer
        if (args.empty()) return nullptr;
        return args[0];
    }

    if (name == "ptrOffset") {
        // ptrOffset(ptr, n) -> pointer arithmetic
        if (args.size() < 2) return nullptr;

        llvm::Value* ptr = args[0];
        llvm::Value* offset = args[1];

        // With opaque pointers, use i8* as the base type
        llvm::Value* i8Ptr = ctx.builder.CreatePointerCast(
            ptr,
            llvm::PointerType::get(ctx.llvmCtx, 0)
        );

        llvm::Value* gep = ctx.builder.CreateGEP(
            llvm::Type::getInt8Ty(ctx.llvmCtx),
            i8Ptr,
            offset,
            "ptr_offset"
        );

        // Cast back to the original pointer type
        return ctx.builder.CreatePointerCast(gep, ptr->getType());
    }

    if (name == "ptrDiff") {
        // ptrDiff(p1, p2) -> distance between pointers
        if (args.size() < 2) return nullptr;

        llvm::Value* p1 = ctx.builder.CreatePtrToInt(
            args[0],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );
        llvm::Value* p2 = ctx.builder.CreatePtrToInt(
            args[1],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );

        return ctx.builder.CreateSub(p1, p2, "ptr_diff");
    }

    return nullptr;
}

// ─── Memory Management Intrinsics ────────────────────────────────────────

static llvm::Value* emitMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    // TODO: Implement memory management
    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                              "memory management intrinsic '#", name, "' not fully implemented");
    return nullptr;
}

// ─── Type Inspection Intrinsics ──────────────────────────────────────────

static llvm::Value* emitTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    CodeGenContext& ctx
) {
    if (name == "sizeof") {
        // sizeof(T) -> compile-time constant
        // For now, return 0
        // TODO: Actually compute size from type
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
    }

    if (name == "alignof") {
        // alignof(T) -> compile-time constant
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 0);
    }

    if (name == "bitcast") {
        // bitcast(T, x) -> reinterpret bits
        // For now, just return the value
        // TODO: Actually bitcast
        if (args.empty()) return nullptr;
        return args[0];
    }

    if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr") {
        // Type inspection string intrinsics
        // For now, return an empty string
        // TODO: Implement actual string return
        llvm::Type* strType = getStringType(ctx);
        return llvm::Constant::getNullValue(strType);
    }

    return nullptr;
}

// ─── Control Flow Intrinsics ─────────────────────────────────────────────

static llvm::Value* emitControlIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    // ─── scope_exit ──────────────────────────────────────────────────────
    if (name == "scope_exit") {
        // scope_exit is handled in Sema and stored on BlockStmtAST.
        // CodeGenStmt.cpp emits these callbacks.
        // No runtime code is generated at the call site.
        return nullptr;
    }

    // ─── likely / unlikely ──────────────────────────────────────────────
    if (name == "likely" || name == "unlikely") {
        // Branch prediction hints
        // Just return the condition value
        if (args.empty()) return nullptr;
        return args[0];
    }

    // ─── prefetch ────────────────────────────────────────────────────────
    if (name == "prefetch" || name == "prefetch_r" || name == "prefetch_w") {
        // prefetch(ptr) -> LLVM prefetch intrinsic
        if (args.empty()) return nullptr;

        llvm::Value* ptr = args[0];

        int rw = (name == "prefetch_w") ? 1 : 0;
        int locality = 3;
        int cacheType = 0;

        // Get the prefetch intrinsic
        llvm::Function* prefetch = llvm::Intrinsic::getDeclaration(
            ctx.module,
            llvm::Intrinsic::prefetch,
            {ptr->getType()}
        );

        if (!prefetch) return nullptr;

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
        llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;
        ctx.builder.CreateFence(ordering);
        return nullptr;
    }

    // ─── pause ──────────────────────────────────────────────────────────
    if (name == "pause") {
        // pause() -> LLVM x86 pause instruction
        // For now, just return null
        // TODO: Implement pause intrinsic
        return nullptr;
    }

    return nullptr;
}

} // namespace codegen